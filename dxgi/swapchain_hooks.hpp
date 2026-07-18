#pragma once
#include <Unknwn.h>
#include <dxgi.h>
#include <string_view>
namespace hooks { void ConfigureResolution(std::wstring_view configPath) noexcept; void AttachToFactory(IUnknown* factory) noexcept; void OnSwapChainCreated(IDXGISwapChain* swapChain) noexcept; }
