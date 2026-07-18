#include "nsfw_cuda_host.h"

#ifdef _WIN32

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nsfw_cuda_host_protocol.h"
#include "platform_abstraction.h"

struct nsfw_cuda_host_t {
    HANDLE process;
    HANDLE stdin_write;
    HANDLE stdout_read;
};

static int nsfw_cuda_host_write_exact(HANDLE handle, const void *data, DWORD size)
{
    const uint8_t *cursor = (const uint8_t *)data;

    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(handle, cursor, size, &written, NULL) || written == 0)
            return -1;
        cursor += written;
        size -= written;
    }
    return 0;
}

static int nsfw_cuda_host_read_exact(HANDLE handle, void *data, DWORD size)
{
    uint8_t *cursor = (uint8_t *)data;

    while (size > 0) {
        DWORD read = 0;
        if (!ReadFile(handle, cursor, size, &read, NULL) || read == 0)
            return -1;
        cursor += read;
        size -= read;
    }
    return 0;
}

static int nsfw_inference_host_start(nsfw_cuda_host_t **host,
                                     const nsfw_config_t *config,
                                     const char *helper_name,
                                     const char *provider)
{
    SECURITY_ATTRIBUTES attributes;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HANDLE stdin_read = NULL;
    HANDLE stdin_write = NULL;
    HANDLE stdout_read = NULL;
    HANDLE stdout_write = NULL;
    HANDLE stderr_write = NULL;
    nsfw_cuda_host_t *created = NULL;
    char plugin_dir[1024];
    char helper_path[1200];
    wchar_t helper_wide[MAX_PATH];
    wchar_t provider_wide[16];
    wchar_t command_line[MAX_PATH + 48];
    nsfw_cuda_host_response_t ready;
    unsigned threshold_micros;
    int helper_length;

    if (host == NULL || config == NULL || helper_name == NULL ||
        provider == NULL)
        return -1;
    *host = NULL;
    if (!nsfw_plat_get_plugin_dir(plugin_dir, sizeof(plugin_dir)))
        return -1;
    helper_length = snprintf(helper_path, sizeof(helper_path), "%s%s",
                             plugin_dir, helper_name);
    if (helper_length < 0 || (size_t)helper_length >= sizeof(helper_path) ||
        !nsfw_plat_file_exists(helper_path)) {
        return -1;
    }

    helper_length = MultiByteToWideChar(CP_UTF8, 0, helper_path, -1,
                                        helper_wide, MAX_PATH);
    if (helper_length <= 0)
        return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, provider, -1, provider_wide,
                            sizeof(provider_wide) / sizeof(provider_wide[0])) <= 0 ||
        swprintf(command_line, sizeof(command_line) / sizeof(command_line[0]),
                 L"\"%ls\" --provider %ls", helper_wide, provider_wide) < 0) {
        return -1;
    }

    threshold_micros = (unsigned)(config->threshold * 1000000.0f + 0.5f);
    nsfw_plat_set_env_unsigned("NSFW_CUDA_HOST_PROFILE",
                               (unsigned)config->model_profile);
    nsfw_plat_set_env_unsigned("NSFW_CUDA_HOST_WIDTH",
                               (unsigned)config->model_width);
    nsfw_plat_set_env_unsigned("NSFW_CUDA_HOST_HEIGHT",
                               (unsigned)config->model_height);
    nsfw_plat_set_env_unsigned("NSFW_CUDA_HOST_THRESHOLD_MICROS",
                               threshold_micros);

    memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    stderr_write = CreateFileW(L"NUL", GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &attributes, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (stderr_write == INVALID_HANDLE_VALUE) {
        stderr_write = NULL;
        goto fail;
    }
    if (!CreatePipe(&stdin_read, &stdin_write, &attributes, 0) ||
        !SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stdout_read, &stdout_write, &attributes, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        goto fail;
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(helper_wide, command_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        goto fail;
    }
    CloseHandle(process.hThread);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    created = (nsfw_cuda_host_t *)calloc(1, sizeof(*created));
    if (created == NULL) {
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        return -1;
    }

    created->process = process.hProcess;
    created->stdin_write = stdin_write;
    created->stdout_read = stdout_read;

    if (nsfw_cuda_host_read_exact(created->stdout_read, &ready,
                                  sizeof(ready)) != 0 ||
        ready.magic != NSFW_CUDA_HOST_READY_MAGIC || ready.status != 0) {
        nsfw_cuda_host_stop(&created);
        return -1;
    }
    *host = created;
    return 0;

fail:
    if (stdin_read != NULL) CloseHandle(stdin_read);
    if (stdin_write != NULL) CloseHandle(stdin_write);
    if (stdout_read != NULL) CloseHandle(stdout_read);
    if (stdout_write != NULL) CloseHandle(stdout_write);
    if (stderr_write != NULL) CloseHandle(stderr_write);
    return -1;
}

int nsfw_cuda_host_start(nsfw_cuda_host_t **host, const nsfw_config_t *config)
{
    return nsfw_inference_host_start(host, config, "icop_cuda_host.exe",
                                     "cuda");
}

int nsfw_dml_host_start(nsfw_cuda_host_t **host, const nsfw_config_t *config)
{
    return nsfw_inference_host_start(host, config,
                                     "dml\\icop_dml_host.exe", "dml");
}

int nsfw_cuda_host_classify(nsfw_cuda_host_t *host,
                            const uint8_t *frame_data,
                            int width, int height, int channels,
                            nsfw_result_t *result)
{
    return nsfw_cuda_host_classify_batch(host, frame_data, 1, width, height,
                                         channels, result);
}

int nsfw_cuda_host_classify_batch(nsfw_cuda_host_t *host,
                                  const uint8_t *frame_data,
                                  unsigned frame_count,
                                  int width, int height, int channels,
                                  nsfw_result_t *results)
{
    nsfw_cuda_host_request_t request;
    size_t frame_byte_count;
    size_t total_byte_count;
    unsigned i;
    int overall_status = 0;

    if (host == NULL || frame_data == NULL || results == NULL ||
        frame_count == 0 || frame_count > NSFW_CUDA_HOST_MAX_BATCH_SIZE ||
        width <= 0 || height <= 0 || channels <= 0) {
        return -1;
    }

    frame_byte_count = (size_t)width * (size_t)height * (size_t)channels;
    total_byte_count = frame_byte_count * (size_t)frame_count;
    if (frame_byte_count == 0 || frame_byte_count > UINT32_MAX ||
        total_byte_count > UINT32_MAX) {
        return -1;
    }

    request.magic = NSFW_CUDA_HOST_REQUEST_MAGIC;
    request.width = (uint32_t)width;
    request.height = (uint32_t)height;
    request.channels = (uint32_t)channels;
    request.frame_count = frame_count;
    request.frame_byte_count = (uint32_t)frame_byte_count;
    if (nsfw_cuda_host_write_exact(host->stdin_write, &request,
                                   sizeof(request)) != 0 ||
        nsfw_cuda_host_write_exact(host->stdin_write, frame_data,
                                   (DWORD)total_byte_count) != 0) {
        return -1;
    }

    for (i = 0; i < frame_count; ++i) {
        nsfw_cuda_host_response_t response;
        int status;

        if (nsfw_cuda_host_read_exact(host->stdout_read, &response,
                                      sizeof(response)) != 0 ||
            response.magic != NSFW_CUDA_HOST_RESPONSE_MAGIC) {
            return -1;
        }
        status = response.status;
        if (status != 0 && status != NSFW_BATCH_UNSUPPORTED)
            return -1;
        results[i].is_nsfw = response.is_nsfw;
        results[i].score = response.score;
        results[i].threshold = response.threshold;
        if (status == NSFW_BATCH_UNSUPPORTED)
            overall_status = NSFW_BATCH_UNSUPPORTED;
    }

    return overall_status;
}

void nsfw_cuda_host_stop(nsfw_cuda_host_t **host)
{
    nsfw_cuda_host_t *active;

    if (host == NULL || *host == NULL)
        return;
    active = *host;
    if (active->stdin_write != NULL) CloseHandle(active->stdin_write);
    if (active->stdout_read != NULL) CloseHandle(active->stdout_read);
    if (active->process != NULL) {
        if (WaitForSingleObject(active->process, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(active->process, 0);
            WaitForSingleObject(active->process, 2000);
        }
        CloseHandle(active->process);
    }
    free(active);
    *host = NULL;
}

#else

int nsfw_cuda_host_start(nsfw_cuda_host_t **host, const nsfw_config_t *config)
{
    (void)host;
    (void)config;
    return -1;
}

int nsfw_dml_host_start(nsfw_cuda_host_t **host, const nsfw_config_t *config)
{
    (void)host;
    (void)config;
    return -1;
}

int nsfw_cuda_host_classify(nsfw_cuda_host_t *host,
                            const uint8_t *frame_data,
                            int width, int height, int channels,
                            nsfw_result_t *result)
{
    (void)host;
    (void)frame_data;
    (void)width;
    (void)height;
    (void)channels;
    (void)result;
    return -1;
}

int nsfw_cuda_host_classify_batch(nsfw_cuda_host_t *host,
                                  const uint8_t *frame_data,
                                  unsigned frame_count,
                                  int width, int height, int channels,
                                  nsfw_result_t *results)
{
    (void)host;
    (void)frame_data;
    (void)frame_count;
    (void)width;
    (void)height;
    (void)channels;
    (void)results;
    return -1;
}

void nsfw_cuda_host_stop(nsfw_cuda_host_t **host)
{
    (void)host;
}

#endif
