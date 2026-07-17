#include "nsfw_platform_utils.h"

#include <cstring>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

std::wstring nsfw_platform_utf8_to_wide(const char *text)
{
    if (!text || text[0] == '\0')
        return std::wstring();

#ifdef _WIN32
    const int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 0)
        return std::wstring();

    std::wstring wide(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, &wide[0], len) <= 0)
        return std::wstring();
    wide.resize(static_cast<size_t>(len - 1));
    return wide;
#else
    return std::wstring();
#endif
}

std::string nsfw_platform_wide_to_utf8(const wchar_t *text)
{
    if (!text || text[0] == L'\0')
        return std::string();

#ifdef _WIN32
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0)
        return std::string();

    std::string utf8(static_cast<size_t>(len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, &utf8[0], len, nullptr,
                            nullptr) <= 0) {
        return std::string();
    }
    utf8.resize(static_cast<size_t>(len - 1));
    return utf8;
#else
    return std::string();
#endif
}

std::string nsfw_platform_get_module_directory(void)
{
#ifdef _WIN32
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    wchar_t *slash = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&nsfw_platform_get_module_directory),
                            &module)) {
        return std::string();
    }

    DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return std::string();

    slash = wcsrchr(path, L'\\');
    if (!slash)
        return std::string();

    *slash = L'\0';
    return nsfw_platform_wide_to_utf8(path);
#else
    Dl_info info;
    const char *path;
    const char *slash;

    std::memset(&info, 0, sizeof(info));
    if (dladdr(reinterpret_cast<const void *>(&nsfw_platform_get_module_directory),
               &info) == 0 ||
        info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
        return std::string();
    }

    path = info.dli_fname;
    slash = std::strrchr(path, '/');
    if (!slash)
        slash = std::strrchr(path, '\\');
    if (!slash)
        return std::string();

    return std::string(path, static_cast<size_t>(slash - path + 1));
#endif
}

std::string nsfw_platform_module_sibling_path(const char *filename)
{
    if (!filename || filename[0] == '\0')
        return std::string();

    std::string dir = nsfw_platform_get_module_directory();
    if (dir.empty())
        return std::string();

    if (dir.back() != '/' && dir.back() != '\\') {
#ifdef _WIN32
        dir += '\\';
#else
        dir += '/';
#endif
    }
    dir += filename;
    return dir;
}

bool nsfw_platform_file_exists(const char *path)
{
    if (path == nullptr || path[0] == '\0')
        return false;

#ifdef _WIN32
    std::wstring wide = nsfw_platform_utf8_to_wide(path);
    if (wide.empty())
        return false;

    DWORD attrs = GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}
