#pragma once

#include "magnifier_controller.h"
#include "win32_process_backend.h"

#include <Windows.h>

#include <string>

namespace magnify {

class TrayApplication {
public:
    explicit TrayApplication(HINSTANCE instance) noexcept;
    ~TrayApplication();

    TrayApplication(const TrayApplication&) = delete;
    TrayApplication& operator=(const TrayApplication&) = delete;

    int Run();

private:
    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM w_param,
        LPARAM l_param);

    bool Initialize();
    void Shutdown() noexcept;
    bool AddTrayIcon();
    void RemoveTrayIcon() noexcept;
    void HandleHotkey();
    void ShowExitMenu(POINT location);
    void ShowError(const std::wstring& message, unsigned long code) const;

    HINSTANCE instance_ = nullptr;
    HANDLE instance_mutex_ = nullptr;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbar_created_message_ = 0;
    bool class_registered_ = false;
    bool hotkey_registered_ = false;
    bool tray_icon_added_ = false;
    bool toggling_ = false;
    bool menu_open_ = false;

    Win32MagnifierBackend backend_;
    MagnifierController controller_;
};

}  // namespace magnify
