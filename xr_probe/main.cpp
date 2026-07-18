#include "openxr_host.hpp"

#include <Windows.h>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
    void PauseIfLaunchedByExplorer()
    {
        DWORD processes[4]{};
        if (GetConsoleProcessList(processes, static_cast<DWORD>(std::size(processes))) <= 1)
        {
            std::wcout << L"Press Enter to close..." << std::flush;
            std::wstring ignored;
            std::getline(std::wcin, ignored);
        }
    }
}

int wmain()
{
    std::wcout << L"DayZ OpenXR standalone probe\n"
                  L"Press Esc to exit. The active OpenXR runtime must have a connected HMD.\n";

    auto& host = OpenXrHost::Instance();
    if (!host.InitializeStandalone())
    {
        std::wcerr << L"OpenXR initialization failed. See xr_probe.log.\n";
        host.Shutdown();
        PauseIfLaunchedByExplorer();
        return 1;
    }

    while (!host.ShouldExit() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0)
    {
        host.Tick();
        if (!host.IsSessionRunning())
            Sleep(10);
    }

    host.Shutdown();
    return 0;
}
