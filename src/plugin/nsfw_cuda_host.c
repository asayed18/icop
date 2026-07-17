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

int nsfw_cuda_host_start(nsfw_cuda_host_t **host, const nsfw_config_t *config)
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
    unsigned threshold_micros;
    int helper_length;

    if (host == NULL || config == NULL)
        return -1;
    *host = NULL;
    if (!nsfw_plat_get_plugin_dir(plugin_dir, sizeof(plugin_dir)) ||
        snprintf(helper_path, sizeof(helper_path), "%sicop_cuda_host.exe", plugin_dir) < 0 ||
        !nsfw_plat_file_exists(helper_path)) {
        return -1;
    }

    helper_length = MultiByteToWideChar(CP_UTF8, 0, helper_path, -1,
                                        helper_wide, MAX_PATH);
    if (helper_length <= 0)
        return -1;

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
    if (!CreateProcessW(helper_wide, NULL, NULL, NULL, TRUE,
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

int nsfw_cuda_host_classify(nsfw_cuda_host_t *host,
                            const uint8_t *frame_data,
                            int width, int height, int channels,
                            nsfw_result_t *result)
{
    nsfw_cuda_host_request_t request;
    nsfw_cuda_host_response_t response;
    size_t byte_count;

    if (host == NULL || frame_data == NULL || result == NULL ||
        width <= 0 || height <= 0 || channels <= 0)
        return -1;

    byte_count = (size_t)width * (size_t)height * (size_t)channels;
    if (byte_count > UINT32_MAX)
        return -1;

    request.magic = NSFW_CUDA_HOST_REQUEST_MAGIC;
    request.width = (uint32_t)width;
    request.height = (uint32_t)height;
    request.channels = (uint32_t)channels;
    request.byte_count = (uint32_t)byte_count;
    if (nsfw_cuda_host_write_exact(host->stdin_write, &request,
                                   sizeof(request)) != 0 ||
        nsfw_cuda_host_write_exact(host->stdin_write, frame_data,
                                   (DWORD)byte_count) != 0 ||
        nsfw_cuda_host_read_exact(host->stdout_read, &response,
                                  sizeof(response)) != 0 ||
        response.magic != NSFW_CUDA_HOST_RESPONSE_MAGIC ||
        response.status != 0) {
        return -1;
    }

    result->is_nsfw = response.is_nsfw;
    result->score = response.score;
    result->threshold = response.threshold;
    return 0;
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

void nsfw_cuda_host_stop(nsfw_cuda_host_t **host)
{
    (void)host;
}

#endif
