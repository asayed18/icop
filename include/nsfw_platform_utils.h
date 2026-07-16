#ifndef NSFW_PLATFORM_UTILS_H
#define NSFW_PLATFORM_UTILS_H

#include <string>

std::wstring nsfw_platform_utf8_to_wide(const char *text);
std::string nsfw_platform_wide_to_utf8(const wchar_t *text);
std::string nsfw_platform_get_module_directory(void);
std::string nsfw_platform_module_sibling_path(const char *filename);
bool nsfw_platform_file_exists(const char *path);

#endif
