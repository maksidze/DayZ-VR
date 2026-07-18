#pragma once
#include <Windows.h>
namespace proxy { bool EnsureSystemDxgiLoaded() noexcept; FARPROC Resolve(const char* name) noexcept; }
