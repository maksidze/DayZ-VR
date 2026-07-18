#pragma once

#include <string_view>

namespace logging
{
    void Initialize(std::wstring_view fileName = L"dayz_openxr.log") noexcept;
    void Info(std::string_view message) noexcept;
    void Error(std::string_view message) noexcept;
    void XrError(std::string_view operation, long result) noexcept;
}
