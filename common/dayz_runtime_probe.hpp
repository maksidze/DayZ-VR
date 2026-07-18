#pragma once

struct ID3D11Device;
struct IDXGISwapChain;

namespace dayz::runtime_probe
{
    // Installs observation-only hooks for the one DayZ build described by output/output_p1_dr.
    // No engine argument or object is modified. A failed identity/signature check leaves stock
    // execution untouched.
    bool Initialize() noexcept;
    void AttachD3DDevice(ID3D11Device* device) noexcept;
    void BeforePresent(IDXGISwapChain* swapChain) noexcept;
    void OnPresent() noexcept;
    bool IsActive() noexcept;
}
