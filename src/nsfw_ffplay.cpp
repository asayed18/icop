#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "nsfw_filter_core.h"

namespace {

using steady_clock = std::chrono::steady_clock;

struct Options {
    std::string input_path;
    std::string provider = "cpu";
    std::string model_profile = "marqo";
    std::string block_style = "black";
    float threshold = 0.5f;
    double hold_seconds = 0.4;
    double buffer_seconds = 2.0;
    double max_buffer_seconds = 5.0;
    double quit_after_seconds = 0.0;
    int analysis_stride = 0;
    int max_display_width = 1280;
    int max_display_height = 720;
};

struct DisplayFrame {
    int64_t pts_ms = 0;
    int64_t duration_ms = 0;
    bool blocked = false;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> pixels;
};

static std::string Utf8FromWide(const wchar_t *text)
{
    int length;
    std::string utf8;

    if (text == nullptr || text[0] == L'\0')
        return utf8;

    length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr,
                                 nullptr);
    if (length <= 1)
        return utf8;

    utf8.resize(static_cast<size_t>(length - 1));
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), length, nullptr,
                        nullptr);
    return utf8;
}

static std::string AvErrorString(int errnum)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = { 0 };

    av_strerror(errnum, buffer, sizeof(buffer));
    return std::string(buffer);
}

static int DefaultAnalysisStride(int width, int height)
{
    const uint64_t pixels = static_cast<uint64_t>(width) *
                            static_cast<uint64_t>(height);

    if (pixels >= static_cast<uint64_t>(3840) * 2160)
        return 3;
    if (pixels >= static_cast<uint64_t>(2560) * 1440)
        return 2;
    return 1;
}

static double RationalToDouble(AVRational value, double fallback)
{
    if (value.num > 0 && value.den > 0)
        return av_q2d(value);
    return fallback;
}

static int64_t ClampPositiveMs(double seconds, int64_t fallback_ms)
{
    if (seconds <= 0.0)
        return fallback_ms;

    const double raw_ms = seconds * 1000.0;
    if (raw_ms < 1.0)
        return 1;

    return static_cast<int64_t>(raw_ms + 0.5);
}

static void FillBgra(uint8_t *pixels, int width, int height, int stride,
                     uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    if (!pixels)
        return;

    for (int y = 0; y < height; ++y) {
        uint8_t *row = pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            uint8_t *px = row + x * 4;
            px[0] = b;
            px[1] = g;
            px[2] = r;
            px[3] = a;
        }
    }
}

static void BlurBgra(uint8_t *pixels, int width, int height, int stride)
{
    std::vector<uint8_t> tmp(static_cast<size_t>(stride) * height);
    if (tmp.empty())
        return;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned sum_b = 0, sum_g = 0, sum_r = 0, sum_a = 0;
            unsigned count = 0;

            for (int ky = -1; ky <= 1; ++ky) {
                int sy = std::clamp(y + ky, 0, height - 1);
                for (int kx = -1; kx <= 1; ++kx) {
                    int sx = std::clamp(x + kx, 0, width - 1);
                    const uint8_t *src = pixels + static_cast<size_t>(sy) * stride + sx * 4;
                    sum_b += src[0];
                    sum_g += src[1];
                    sum_r += src[2];
                    sum_a += src[3];
                    count++;
                }
            }

            uint8_t *dst = tmp.data() + static_cast<size_t>(y) * stride + x * 4;
            dst[0] = static_cast<uint8_t>(sum_b / count);
            dst[1] = static_cast<uint8_t>(sum_g / count);
            dst[2] = static_cast<uint8_t>(sum_r / count);
            dst[3] = static_cast<uint8_t>(sum_a / count);
        }
    }

    std::memcpy(pixels, tmp.data(), tmp.size());
}

static void RedWarningBgra(uint8_t *pixels, int width, int height, int stride)
{
    FillBgra(pixels, width, height, stride, 0, 0, 255, 255);
}

class PlayerApp {
public:
    explicit PlayerApp(Options options)
        : options_(std::move(options))
        , target_buffer_ms_(ClampPositiveMs(options_.buffer_seconds, 2000))
        , max_buffer_ms_(std::max(
              target_buffer_ms_,
              ClampPositiveMs(options_.max_buffer_seconds, target_buffer_ms_)))
        , quit_after_ms_(ClampPositiveMs(options_.quit_after_seconds, 0))
    {}

    int Run()
    {
        if (!CreatePlayerWindow())
            return 1;

        decoder_thread_ = std::thread(&PlayerApp::DecoderMain, this);

        MSG msg;
        while (!stop_requested_) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    stop_requested_ = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (stop_requested_)
                break;

            StepPlayback();
            Sleep(1);
        }

        stop_requested_ = true;
        queue_cv_.notify_all();
        if (decoder_thread_.joinable())
            decoder_thread_.join();

        if (decoder_failed_) {
            std::lock_guard<std::mutex> lock(error_mutex_);
            if (!decoder_error_.empty())
                std::fprintf(stderr, "nsfw_ffplay_proto: %s\n",
                             decoder_error_.c_str());
        }

        return decoder_failed_ ? 1 : 0;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam)
    {
        PlayerApp *self = nullptr;

        if (message == WM_NCCREATE) {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            self = static_cast<PlayerApp *>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<PlayerApp *>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self != nullptr)
            return self->HandleWindowMessage(hwnd, message, wparam, lparam);

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT HandleWindowMessage(HWND hwnd, UINT message, WPARAM wparam,
                                LPARAM lparam)
    {
        switch (message) {
            case WM_ERASEBKGND:
                return 1;

            case WM_KEYDOWN:
                if (wparam == VK_SPACE) {
                    user_paused_ = !user_paused_;
                    return 0;
                }
                if (wparam == VK_ESCAPE || wparam == 'Q') {
                    stop_requested_ = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                break;

            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                DrawCurrentFrame(dc);
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_DESTROY:
                stop_requested_ = true;
                PostQuitMessage(0);
                return 0;
        }

        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool CreatePlayerWindow()
    {
        WNDCLASSW window_class = {};
        const wchar_t *class_name = L"NsfwFfplayProtoWindow";

        window_class.lpfnWndProc = &PlayerApp::WindowProc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = class_name;
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        if (!RegisterClassW(&window_class) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            std::fprintf(stderr, "nsfw_ffplay_proto: failed to register window class\n");
            return false;
        }

        hwnd_ = CreateWindowExW(
            0, class_name, L"NSFW FFPlay Prototype", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr,
            GetModuleHandleW(nullptr), this);
        if (hwnd_ == nullptr) {
            std::fprintf(stderr, "nsfw_ffplay_proto: failed to create window\n");
            return false;
        }

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        return true;
    }

    void DrawCurrentFrame(HDC dc)
    {
        RECT client = {};
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        GetClientRect(hwnd_, &client);
        FillRect(dc, &client, brush);
        DeleteObject(brush);

        if (!current_frame_ || current_frame_->blocked || current_frame_->pixels.empty())
            return;

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = current_frame_->width;
        info.bmiHeader.biHeight = -current_frame_->height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        const int client_width = std::max(
            1, static_cast<int>(client.right - client.left));
        const int client_height = std::max(
            1, static_cast<int>(client.bottom - client.top));
        const double scale_x = static_cast<double>(client_width) /
                               static_cast<double>(current_frame_->width);
        const double scale_y = static_cast<double>(client_height) /
                               static_cast<double>(current_frame_->height);
        const double scale = std::min(scale_x, scale_y);
        const int draw_width = std::max(
            1, static_cast<int>(current_frame_->width * scale + 0.5));
        const int draw_height = std::max(
            1, static_cast<int>(current_frame_->height * scale + 0.5));
        const int draw_x = (client_width - draw_width) / 2;
        const int draw_y = (client_height - draw_height) / 2;

        StretchDIBits(dc, draw_x, draw_y, draw_width, draw_height, 0, 0,
                      current_frame_->width, current_frame_->height,
                      current_frame_->pixels.data(), &info, DIB_RGB_COLORS,
                      SRCCOPY);
    }

    int64_t BufferedMsLocked() const
    {
        if (queue_.empty())
            return 0;

        const auto &first = queue_.front();
        const auto &last = queue_.back();
        return std::max<int64_t>(
            0, (last->pts_ms + last->duration_ms) - first->pts_ms);
    }

    void SetDecoderError(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        decoder_failed_ = true;
        decoder_error_ = message;
        stop_requested_ = true;
    }

    void UpdateWindowTitle(int64_t buffered_ms, bool started, bool paused,
                           bool eof)
    {
        const char *state = nullptr;
        char title[256];

        if (decoder_failed_) {
            std::snprintf(title, sizeof(title), "NSFW FFPlay Prototype - error");
            SetWindowTextA(hwnd_, title);
            return;
        }

        if (!started)
            state = "buffering";
        else if (paused)
            state = "paused";
        else
            state = "playing";

        std::snprintf(title, sizeof(title),
                      "NSFW FFPlay Prototype - %s - buffered %.2fs%s",
                      state, buffered_ms / 1000.0, eof ? " - eof" : "");
        SetWindowTextA(hwnd_, title);
    }

    void TransitionPauseState(bool paused, steady_clock::time_point now)
    {
        if (!playback_started_)
            return;
        if (paused == effective_paused_)
            return;

        if (paused) {
            pause_started_at_ = now;
        } else {
            playback_anchor_ += now - pause_started_at_;
        }

        effective_paused_ = paused;
    }

    void StepPlayback()
    {
        const auto now = steady_clock::now();
        bool should_repaint = false;
        int64_t buffered_ms = 0;
        bool eof = false;
        bool started = false;
        bool paused = true;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            buffered_ms = BufferedMsLocked();
            eof = decoder_eof_;

            if (!playback_started_) {
                if (!queue_.empty() &&
                    (buffered_ms >= target_buffer_ms_ || eof)) {
                    playback_started_ = true;
                    playback_anchor_ = now;
                    playback_base_pts_ms_ = queue_.front()->pts_ms;
                    effective_paused_ = false;
                    auto_paused_ = false;
                    pause_started_at_ = now;
                } else {
                    auto_paused_ = true;
                }
            } else {
                auto_paused_ = !eof && buffered_ms < target_buffer_ms_;
            }

            started = playback_started_;
            paused = !playback_started_ || user_paused_ || auto_paused_;
            TransitionPauseState(paused, now);

            if (playback_started_ && !effective_paused_) {
                int64_t elapsed_ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(now - playback_anchor_).count();

                while (!queue_.empty()) {
                    const auto due_ms =
                        queue_.front()->pts_ms - playback_base_pts_ms_;
                    if (due_ms > elapsed_ms)
                        break;

                    current_frame_ = std::move(queue_.front());
                    queue_.pop_front();
                    should_repaint = true;
                    buffered_ms = BufferedMsLocked();

                    if (quit_after_ms_ > 0 &&
                        current_frame_->pts_ms - playback_base_pts_ms_ >=
                            quit_after_ms_) {
                        stop_requested_ = true;
                        break;
                    }
                }
            }

            if (decoder_eof_ && queue_.empty() && playback_started_)
                stop_requested_ = true;
        }

        queue_cv_.notify_all();

        if (should_repaint)
            InvalidateRect(hwnd_, nullptr, FALSE);

        UpdateWindowTitle(buffered_ms, started, paused, eof);

        if (decoder_failed_) {
            std::lock_guard<std::mutex> lock(error_mutex_);
            if (!decoder_error_.empty())
                std::fprintf(stderr, "nsfw_ffplay_proto: %s\n",
                             decoder_error_.c_str());
        }
    }

    bool PushFrame(std::shared_ptr<DisplayFrame> frame)
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        queue_cv_.wait(lock, [&]() {
            return stop_requested_ || BufferedMsLocked() < max_buffer_ms_;
        });
        if (stop_requested_)
            return false;

        queue_.push_back(std::move(frame));
        return true;
    }

    void DecoderMain()
    {
        AVFormatContext *format = nullptr;
        AVCodecContext *codec = nullptr;
        const AVCodec *decoder = nullptr;
        AVPacket *packet = nullptr;
        AVFrame *frame = nullptr;
        SwsContext *display_sws = nullptr;
        SwsContext *model_sws = nullptr;
        nsfw_detector_t *detector = nullptr;
        int video_stream_index = -1;
        int64_t last_pts_ms = 0;
        int64_t synthetic_pts_ms = 0;
        int64_t frame_duration_ms = 42;
        int analysis_stride = 1;
        int hold_remaining = 0;
        int positive_hold_frames = 1;
        int display_width = 0;
        int display_height = 0;
        int model_width = 0;
        int model_height = 0;
        int frame_index = 0;
        bool drain = false;

        auto cleanup = [&]() {
            if (detector)
                nsfw_detector_destroy(detector);
            if (display_sws)
                sws_freeContext(display_sws);
            if (model_sws)
                sws_freeContext(model_sws);
            if (packet)
                av_packet_free(&packet);
            if (frame)
                av_frame_free(&frame);
            if (codec)
                avcodec_free_context(&codec);
            if (format)
                avformat_close_input(&format);
        };

        _putenv_s("NSFW_ONNX_PROVIDER", options_.provider.c_str());

        if (avformat_open_input(&format, options_.input_path.c_str(), nullptr,
                                nullptr) < 0) {
            SetDecoderError("failed to open input");
            cleanup();
            return;
        }

        if (avformat_find_stream_info(format, nullptr) < 0) {
            SetDecoderError("failed to read stream info");
            cleanup();
            return;
        }

        video_stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1,
                                                 -1, &decoder, 0);
        if (video_stream_index < 0 || decoder == nullptr) {
            SetDecoderError("no video stream found");
            cleanup();
            return;
        }

        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr) {
            SetDecoderError("failed to allocate decoder context");
            cleanup();
            return;
        }

        if (avcodec_parameters_to_context(
                codec, format->streams[video_stream_index]->codecpar) < 0) {
            SetDecoderError("failed to copy codec parameters");
            cleanup();
            return;
        }

        codec->thread_count = 0;
        codec->thread_type = FF_THREAD_FRAME;

        if (avcodec_open2(codec, decoder, nullptr) < 0) {
            SetDecoderError("failed to open decoder");
            cleanup();
            return;
        }

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            SetDecoderError("failed to allocate FFmpeg frame state");
            cleanup();
            return;
        }

        {
            const double fps = RationalToDouble(
                format->streams[video_stream_index]->avg_frame_rate,
                RationalToDouble(codec->framerate, 24.0));
            if (fps > 0.0)
                frame_duration_ms = std::max<int64_t>(
                    1, static_cast<int64_t>(1000.0 / fps + 0.5));
            positive_hold_frames = std::max(
                1, static_cast<int>(options_.hold_seconds * fps + 0.5));
        }

        analysis_stride = options_.analysis_stride > 0
            ? options_.analysis_stride
            : DefaultAnalysisStride(codec->width, codec->height);

        {
            nsfw_config_t config = nsfw_config_default();
            nsfw_model_profile_t profile = NSFW_MODEL_PROFILE_MARQO;
            config.threshold = options_.threshold;
            if (nsfw_model_profile_parse(options_.model_profile.c_str(), &profile)) {
                nsfw_config_set_model_profile(&config, profile);
            }
            detector = nsfw_detector_create(&config);
            if (detector == nullptr) {
                SetDecoderError("failed to create ONNX detector");
                cleanup();
                return;
            }
            model_width = config.model_width;
            model_height = config.model_height;
        }

        {
            const double scale_x = static_cast<double>(options_.max_display_width) /
                                   static_cast<double>(std::max(codec->width, 1));
            const double scale_y = static_cast<double>(options_.max_display_height) /
                                   static_cast<double>(std::max(codec->height, 1));
            const double scale = std::min(1.0, std::min(scale_x, scale_y));
            display_width = std::max(
                1, static_cast<int>(codec->width * scale + 0.5));
            display_height = std::max(
                1, static_cast<int>(codec->height * scale + 0.5));
        }

        std::fprintf(stderr,
                     "nsfw_ffplay_proto: decoding %dx%d, display %dx%d, analysis stride %d, target buffer %.2fs\n",
                     codec->width, codec->height, display_width, display_height,
                     analysis_stride, target_buffer_ms_ / 1000.0);

        display_sws = sws_getContext(codec->width, codec->height, codec->pix_fmt,
                                     display_width, display_height,
                                     AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
        model_sws = sws_getContext(codec->width, codec->height, codec->pix_fmt,
                                   model_width, model_height, AV_PIX_FMT_RGB24,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (display_sws == nullptr || model_sws == nullptr) {
            SetDecoderError("failed to create scaling contexts");
            cleanup();
            return;
        }

        std::vector<uint8_t> model_rgb(
            static_cast<size_t>(model_width) * model_height * 3);

        while (!stop_requested_) {
            const auto *stream = format->streams[video_stream_index];
            int read_status;
            int send_status;

            if (!drain) {
                read_status = av_read_frame(format, packet);
                if (read_status == AVERROR_EOF) {
                    drain = true;
                    send_status = avcodec_send_packet(codec, nullptr);
                } else if (read_status < 0) {
                    SetDecoderError("failed to read packet: " +
                                    AvErrorString(read_status));
                    cleanup();
                    return;
                } else if (packet->stream_index != video_stream_index) {
                    av_packet_unref(packet);
                    continue;
                } else {
                    send_status = avcodec_send_packet(codec, packet);
                    av_packet_unref(packet);
                }

                if (send_status < 0 && send_status != AVERROR(EAGAIN) &&
                    send_status != AVERROR_EOF) {
                    SetDecoderError("failed to send packet to decoder: " +
                                    AvErrorString(send_status));
                    cleanup();
                    return;
                }
            }

            for (;;) {
                const int receive_status = avcodec_receive_frame(codec, frame);
                if (receive_status == AVERROR(EAGAIN))
                    break;
                if (receive_status == AVERROR_EOF) {
                    drain = true;
                    goto decoding_done;
                }
                if (receive_status < 0) {
                    SetDecoderError("failed to receive decoded frame: " +
                                    AvErrorString(receive_status));
                    cleanup();
                    return;
                }

                int64_t pts_ms = AV_NOPTS_VALUE;
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    pts_ms = av_rescale_q(frame->best_effort_timestamp,
                                          stream->time_base,
                                          AVRational{ 1, 1000 });
                } else if (frame->pts != AV_NOPTS_VALUE) {
                    pts_ms = av_rescale_q(frame->pts, stream->time_base,
                                          AVRational{ 1, 1000 });
                }

                if (pts_ms == AV_NOPTS_VALUE) {
                    pts_ms = synthetic_pts_ms;
                } else if (frame_index > 0 && pts_ms <= last_pts_ms) {
                    pts_ms = last_pts_ms + frame_duration_ms;
                }

                synthetic_pts_ms = pts_ms + frame_duration_ms;
                last_pts_ms = pts_ms;

                bool blocked = false;
                if (hold_remaining > 0) {
                    blocked = true;
                    hold_remaining--;
                }

                if ((frame_index % analysis_stride) == 0) {
                    uint8_t *model_data[4] = {
                        model_rgb.data(), nullptr, nullptr, nullptr
                    };
                    int model_linesize[4] = { model_width * 3, 0, 0, 0 };
                    sws_scale(model_sws, frame->data, frame->linesize, 0,
                              codec->height, model_data, model_linesize);

                    const nsfw_result_t result = nsfw_detector_classify(
                        detector, model_rgb.data(), model_width, model_height, 3);
                    if (result.is_nsfw) {
                        blocked = true;
                        hold_remaining = positive_hold_frames;
                        std::fprintf(stderr,
                                     "nsfw_ffplay_proto: blocked frame at %.3fs (score %.3f >= %.3f)\n",
                                     pts_ms / 1000.0, result.score,
                                     result.threshold);
                    }
                }

                auto output = std::make_shared<DisplayFrame>();
                output->pts_ms = pts_ms;
                output->duration_ms = frame_duration_ms;
                output->blocked = blocked;
                output->width = display_width;
                output->height = display_height;
                output->stride = display_width * 4;

                output->pixels.resize(
                    static_cast<size_t>(output->stride) * display_height);

                if (!blocked || options_.block_style != "black") {
                    uint8_t *display_data[4] = {
                        output->pixels.data(), nullptr, nullptr, nullptr
                    };
                    int display_linesize[4] = { output->stride, 0, 0, 0 };
                    sws_scale(display_sws, frame->data, frame->linesize, 0,
                              codec->height, display_data, display_linesize);
                } else {
                    std::memset(output->pixels.data(), 0,
                                output->pixels.size());
                }

                if (blocked) {
                    if (options_.block_style == "blur") {
                        BlurBgra(output->pixels.data(), display_width,
                                 display_height, output->stride);
                    } else if (options_.block_style == "warning") {
                        RedWarningBgra(output->pixels.data(), display_width,
                                       display_height, output->stride);
                    }
                }

                if (!PushFrame(std::move(output))) {
                    cleanup();
                    return;
                }

                frame_index++;
                av_frame_unref(frame);
            }
        }

decoding_done:

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            decoder_eof_ = true;
        }
        queue_cv_.notify_all();
        cleanup();
    }

    Options options_;
    const int64_t target_buffer_ms_;
    const int64_t max_buffer_ms_;
    const int64_t quit_after_ms_;

    HWND hwnd_ = nullptr;
    std::thread decoder_thread_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::shared_ptr<DisplayFrame>> queue_;
    std::shared_ptr<DisplayFrame> current_frame_;

    std::mutex error_mutex_;
    std::string decoder_error_;

    std::atomic<bool> stop_requested_ { false };
    std::atomic<bool> decoder_failed_ { false };
    bool decoder_eof_ = false;
    bool playback_started_ = false;
    bool user_paused_ = false;
    bool auto_paused_ = true;
    bool effective_paused_ = true;
    int64_t playback_base_pts_ms_ = 0;
    steady_clock::time_point playback_anchor_ {};
    steady_clock::time_point pause_started_at_ {};
};

static void PrintUsage()
{
    std::fprintf(stderr,
                 "Usage: nsfw_ffplay_proto --input <video> [options]\n"
                 "Options:\n"
                 "  --provider <cpu|cuda>          ONNX provider (default: cpu)\n"
                 "  --model <name>                 Model profile (marqo, adamcodd, falconsai, falconsai-base, falconsai-official, legacy; default: marqo)\n"
                 "  --block-style <black|blur|warning>\n"
                 "                                 Blocked-frame style (default: black)\n"
                 "  --threshold <value>            Detection threshold (default: 0.5)\n"
                 "  --hold-seconds <value>         Block hold after hit (default: 0.4)\n"
                 "  --buffer-seconds <value>       Required buffered lead (default: 2)\n"
                 "  --max-buffer-seconds <value>   Queue cap before decoder waits (default: 5)\n"
                 "  --analysis-stride <frames>     Analyze every Nth frame (default: auto)\n"
                 "  --max-display-width <pixels>   Display/storage width cap (default: 1280)\n"
                 "  --max-display-height <pixels>  Display/storage height cap (default: 720)\n"
                 "  --quit-after <seconds>         Exit automatically after playback time\n");
}

static bool ParseDouble(const char *text, double *value)
{
    char *end = nullptr;
    double parsed;

    if (text == nullptr || value == nullptr)
        return false;

    parsed = std::strtod(text, &end);
    if (end == text || end == nullptr || *end != '\0')
        return false;

    *value = parsed;
    return true;
}

static bool ParseInt(const char *text, int *value)
{
    char *end = nullptr;
    long parsed;

    if (text == nullptr || value == nullptr)
        return false;

    parsed = std::strtol(text, &end, 10);
    if (end == text || end == nullptr || *end != '\0')
        return false;

    *value = static_cast<int>(parsed);
    return true;
}

static bool ParseArguments(int argc, char **argv, Options *options)
{
    int i;

    if (options == nullptr)
        return false;

    for (i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "nsfw_ffplay_proto: missing value for %s\n",
                             name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--input") {
            const char *value = require_value("--input");
            if (!value)
                return false;
            options->input_path = value;
        } else if (arg == "--provider") {
            const char *value = require_value("--provider");
            if (!value)
                return false;
            options->provider = value;
        } else if (arg == "--model") {
            const char *value = require_value("--model");
            nsfw_model_profile_t profile = NSFW_MODEL_PROFILE_MARQO;
            if (!value || !nsfw_model_profile_parse(value, &profile)) {
                std::fprintf(stderr,
                             "nsfw_ffplay_proto: unrecognized model profile %s\n",
                             value ? value : "(null)");
                return false;
            }
            options->model_profile = value;
        } else if (arg == "--block-style") {
            const char *value = require_value("--block-style");
            if (!value)
                return false;
            options->block_style = value;
        } else if (arg == "--threshold") {
            const char *value = require_value("--threshold");
            double parsed = 0.0;
            if (!value || !ParseDouble(value, &parsed))
                return false;
            options->threshold = static_cast<float>(parsed);
        } else if (arg == "--hold-seconds") {
            const char *value = require_value("--hold-seconds");
            if (!value || !ParseDouble(value, &options->hold_seconds))
                return false;
        } else if (arg == "--buffer-seconds") {
            const char *value = require_value("--buffer-seconds");
            if (!value || !ParseDouble(value, &options->buffer_seconds))
                return false;
        } else if (arg == "--max-buffer-seconds") {
            const char *value = require_value("--max-buffer-seconds");
            if (!value || !ParseDouble(value, &options->max_buffer_seconds))
                return false;
        } else if (arg == "--analysis-stride") {
            const char *value = require_value("--analysis-stride");
            if (!value || !ParseInt(value, &options->analysis_stride))
                return false;
        } else if (arg == "--max-display-width") {
            const char *value = require_value("--max-display-width");
            if (!value || !ParseInt(value, &options->max_display_width))
                return false;
        } else if (arg == "--max-display-height") {
            const char *value = require_value("--max-display-height");
            if (!value || !ParseInt(value, &options->max_display_height))
                return false;
        } else if (arg == "--quit-after") {
            const char *value = require_value("--quit-after");
            if (!value || !ParseDouble(value, &options->quit_after_seconds))
                return false;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "nsfw_ffplay_proto: unknown argument %s\n",
                         arg.c_str());
            return false;
        }
    }

    return !options->input_path.empty();
}

} // namespace

int main(int argc, char **argv)
{
    Options options;

    SetConsoleOutputCP(CP_UTF8);
    av_log_set_level(AV_LOG_ERROR);

    if (!ParseArguments(argc, argv, &options)) {
        PrintUsage();
        return 1;
    }

    PlayerApp app(std::move(options));
    return app.Run();
}
