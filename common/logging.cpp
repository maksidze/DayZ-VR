#include "logging.hpp"

#include <Windows.h>
#include <openxr/openxr.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace
{
    std::mutex g_mutex;
    std::ofstream g_stream;
    XrInstance g_instance = XR_NULL_HANDLE;

    std::string Timestamp()
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        char value[32]{};
        sprintf_s(value, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
            time.wSecond, time.wMilliseconds);
        return value;
    }

    std::filesystem::path ResolveLogPath(std::wstring_view fileName)
    {
        std::filesystem::path path(fileName);
        if (path.is_absolute())
            return path;

        std::wstring executablePath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
            static_cast<DWORD>(executablePath.size()));
        if (length == 0 || length >= executablePath.size())
            return path;
        executablePath.resize(length);
        return std::filesystem::path(executablePath).parent_path() / path;
    }

    void Write(const char* level, std::string_view message) noexcept
    {
        std::scoped_lock lock(g_mutex);
        if (!g_stream.is_open())
            return;
        g_stream << Timestamp() << " [" << level << "] " << message << '\n';
        g_stream.flush();
    }
}

namespace logging
{
    void Initialize(std::wstring_view fileName) noexcept
    {
        std::scoped_lock lock(g_mutex);
        if (g_stream.is_open())
            return;
        g_stream.open(ResolveLogPath(fileName), std::ios::out | std::ios::app);
    }

    void Info(std::string_view message) noexcept { Write("INFO", message); }
    void Error(std::string_view message) noexcept { Write("ERROR", message); }

    void XrError(std::string_view operation, long result) noexcept
    {
        std::ostringstream text;
        text << operation << " failed with XrResult " << result;
        Write("XR", text.str());
    }
}
