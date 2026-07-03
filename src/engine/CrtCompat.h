#pragma once
// Warning-clean, portable wrappers around CRT calls that MSVC flags with
// C4996 deprecation warnings, plus char8_t-safe filesystem path construction
// replacing the C++20-deprecated std::filesystem::u8path.
//
// Use these instead of raw strncpy / fopen / getenv / strerror / fs::u8path.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace sv {

// Truncating, always-null-terminating string copy (replaces strncpy).
inline void StrCopy(char* dst, size_t dstSize, const char* src)
{
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    std::snprintf(dst, dstSize, "%s", src);
}

// Fixed-size array overload: sv::StrCopy(buf, src).
template <size_t N>
inline void StrCopy(char (&dst)[N], const char* src)
{
    StrCopy(dst, N, src);
}

// fopen without C4996 (fopen_s on MSVC). Returns nullptr on failure.
inline std::FILE* FOpen(const char* path, const char* mode)
{
#ifdef _MSC_VER
    std::FILE* fp = nullptr;
    if (fopen_s(&fp, path, mode) != 0) return nullptr;
    return fp;
#else
    return std::fopen(path, mode);
#endif
}

// getenv without C4996. Returns an empty string when the variable is unset.
inline std::string GetEnv(const char* name)
{
#ifdef _MSC_VER
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

// strerror without C4996 (strerror_s on MSVC).
inline std::string StrError(int errnum)
{
    char buf[256] = {};
#ifdef _MSC_VER
    strerror_s(buf, sizeof(buf), errnum);
    return std::string(buf);
#else
    std::snprintf(buf, sizeof(buf), "%s", std::strerror(errnum));
    return std::string(buf);
#endif
}

// UTF-8 string -> std::filesystem::path (replaces deprecated fs::u8path).
inline std::filesystem::path U8Path(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::filesystem::path U8Path(const char* utf8)
{
    return U8Path(std::string(utf8 ? utf8 : ""));
}

} // namespace sv
