#include "tray_application.h"

#include "../resources/resource.h"

#include <shellapi.h>
#include <windowsx.h>

#include <cwchar>
#include <string>

namespace magnify {

namespace {

constexpr wchar_t kProductName[] = L"MagnifyTray";
constexpr wchar_t kWindowClass[] = L"MagnifyTray.HiddenWindow";
constexpr wchar_t kMutexName[] = L"Local\\MagnifyTray.21E788D8-2553-4680-A1E1-9B46E2D51A91";
constexpr wchar_t kTrayTooltip[] = L"MagnifyTray - Alt+W 切换放大镜";
constexpr UINT kHotkeyId = 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kExitCommand = 1001;
constexpr UINT_PTR kCloseConfirmationTimer = 1;
constexpr UINT kDeferredCloseMs = 4000;

DWORD ShellErrorOrFallback() noexcept {
    const DWORD code = GetLastError();
    return code == ERROR_SUCCESS ? ERROR_GEN_FAILURE : code;
}

}  // namespace

TrayApplication::TrayApplication(const HINSTANCE instance) noexcept
    : instance_(instance), controller_(backend_) {}

TrayApplication::~TrayApplication() {
    Shutdown();
}

int TrayApplication::Run() {
    if (!Initialize()) {
        return 1;
    }

    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (result == -1) {
        ShowError(L"Windows 消息循环异常终止", GetLastError());
        return 1;
    }
    return 0;
}

bool TrayApplication::Initialize() {
    instance_mutex_ = CreateMutexW(nullptr, FALSE, kMutexName);
    if (instance_mutex_ == nullptr) {
        ShowError(L"无法创建单实例互斥锁", GetLastError());
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Only the running instance should keep the named object alive.
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
        MessageBoxW(
            nullptr,
            L"MagnifyTray 已在运行。",
            kProductName,
            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return false;
    }

    const auto backend_error = backend_.Initialize();
    if (backend_error.has_value()) {
        ShowError(backend_error->message, backend_error->code);
        return false;
    }

    icon_ = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (icon_ == nullptr) {
        ShowError(L"无法加载托盘图标", GetLastError());
        return false;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance_;
    window_class.hIcon = icon_;
    window_class.hIconSm = icon_;
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        ShowError(L"无法注册隐藏窗口类", GetLastError());
        return false;
    }
    class_registered_ = true;

    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass,
        L"",
        WS_POPUP,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        ShowError(L"无法创建隐藏消息窗口", GetLastError());
        return false;
    }

    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbar_created_message_ == 0) {
        ShowError(L"无法注册任务栏重建消息", GetLastError());
        return false;
    }

    if (!RegisterHotKey(window_, kHotkeyId, MOD_ALT | MOD_NOREPEAT, 'W')) {
        ShowError(
            L"无法注册 Alt+W 全局热键；该组合键可能已被其他程序占用",
            GetLastError());
        return false;
    }
    hotkey_registered_ = true;

    if (!AddTrayIcon()) {
        ShowError(L"无法创建托盘图标", ShellErrorOrFallback());
        return false;
    }
    return true;
}

void TrayApplication::Shutdown() noexcept {
    if (deferred_close_deadline_ != 0 && window_ != nullptr) {
        KillTimer(window_, kCloseConfirmationTimer);
    }
    RemoveTrayIcon();

    if (hotkey_registered_) {
        UnregisterHotKey(window_, kHotkeyId);
        hotkey_registered_ = false;
    }

    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }

    if (class_registered_) {
        UnregisterClassW(kWindowClass, instance_);
        class_registered_ = false;
    }

    if (instance_mutex_ != nullptr) {
        CloseHandle(instance_mutex_);
        instance_mutex_ = nullptr;
    }
}

bool TrayApplication::AddTrayIcon() {
    NOTIFYICONDATAW icon_data{};
    icon_data.cbSize = sizeof(icon_data);
    icon_data.hWnd = window_;
    icon_data.uID = kTrayIconId;
    icon_data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon_data.uCallbackMessage = kTrayCallbackMessage;
    icon_data.hIcon = icon_;
    wcsncpy_s(icon_data.szTip, kTrayTooltip, _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &icon_data)) {
        tray_icon_added_ = false;
        return false;
    }

    icon_data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon_data);
    tray_icon_added_ = true;
    return true;
}

void TrayApplication::RemoveTrayIcon() noexcept {
    if (!tray_icon_added_ || window_ == nullptr) {
        return;
    }

    NOTIFYICONDATAW icon_data{};
    icon_data.cbSize = sizeof(icon_data);
    icon_data.hWnd = window_;
    icon_data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &icon_data);
    tray_icon_added_ = false;
}

bool TrayApplication::RecheckDeferredClose(const ExitCheck check) {
    const auto error = backend_.RecheckForcedExit(check);
    if (backend_.HasPendingForcedExit()) {
        return false; // Still exiting. Keep the original confirmation deadline.
    }
    KillTimer(window_, kCloseConfirmationTimer);
    deferred_close_deadline_ = 0;
    if (error) {
        ShowError(error->message, error->code);
        return false;
    }
    return true;
}

void TrayApplication::HandleHotkey() {
    if (toggling_) {
        return;
    }

    toggling_ = true;
    if (backend_.HasPendingForcedExit()) {
        const auto check = GetTickCount64() >= deferred_close_deadline_
            ? ExitCheck::Final : ExitCheck::AllowDeferred;
        if (!RecheckDeferredClose(check)) {
            toggling_ = false;
            return;
        }
    }
    const ToggleResult result = controller_.Toggle();

    MSG queued_message{};
    while (PeekMessageW(
        &queued_message,
        window_,
        WM_HOTKEY,
        WM_HOTKEY,
        PM_REMOVE)) {
        if (queued_message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(queued_message.wParam));
            toggling_ = false;
            return;
        }
    }

    if (result.status == ToggleStatus::Failed) {
        if (backend_.HasPendingForcedExit()) {
            deferred_close_deadline_ = GetTickCount64() + kDeferredCloseMs;
            if (!SetTimer(window_, kCloseConfirmationTimer, kDeferredCloseMs, nullptr)) {
                RecheckDeferredClose(ExitCheck::Final);
            }
        } else {
            ShowError(result.error.message, result.error.code);
        }
    }
    toggling_ = false;
}

void TrayApplication::ShowExitMenu(POINT location) {
    if (menu_open_) {
        return;
    }
    menu_open_ = true;

    if (location.x == -1 && location.y == -1) {
        GetCursorPos(&location);
    }

    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        menu_open_ = false;
        ShowError(L"无法创建托盘菜单", GetLastError());
        return;
    }

    if (!AppendMenuW(menu, MF_STRING, kExitCommand, L"退出")) {
        const DWORD code = GetLastError();
        DestroyMenu(menu);
        menu_open_ = false;
        ShowError(L"无法添加托盘菜单的“退出”命令", code);
        return;
    }
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        location.x,
        location.y,
        0,
        window_,
        nullptr);
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    menu_open_ = false;

    if (command == kExitCommand) {
        SendMessageW(window_, WM_COMMAND, kExitCommand, 0);
    }

    NOTIFYICONDATAW icon_data{};
    icon_data.cbSize = sizeof(icon_data);
    icon_data.hWnd = window_;
    icon_data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_SETFOCUS, &icon_data);
}

void TrayApplication::ShowError(
    const std::wstring& message,
    const unsigned long code) const {
    std::wstring text = message;
    text += L"\n\nWin32 错误码：";
    text += std::to_wstring(code);
    MessageBoxW(
        window_,
        text.c_str(),
        kProductName,
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

LRESULT CALLBACK TrayApplication::WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM w_param,
    const LPARAM l_param) {
    TrayApplication* application = reinterpret_cast<TrayApplication*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        application = static_cast<TrayApplication*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(application));
    }

    if (application != nullptr &&
        application->taskbar_created_message_ != 0 &&
        message == application->taskbar_created_message_) {
        application->tray_icon_added_ = false;
        if (!application->AddTrayIcon()) {
            application->ShowError(L"任务栏重建后无法恢复托盘图标", ShellErrorOrFallback());
        }
        return 0;
    }

    switch (message) {
        case WM_TIMER:
            if (application != nullptr && w_param == kCloseConfirmationTimer &&
                !application->toggling_ && application->deferred_close_deadline_ != 0 &&
                GetTickCount64() >= application->deferred_close_deadline_) {
                application->toggling_ = true;
                application->RecheckDeferredClose(ExitCheck::Final);
                application->toggling_ = false;
            }
            return 0;

        case WM_HOTKEY:
            if (application != nullptr && w_param == kHotkeyId) {
                application->HandleHotkey();
            }
            return 0;

        case kTrayCallbackMessage:
            if (application != nullptr) {
                const UINT notification = LOWORD(l_param);
                if (notification == WM_CONTEXTMENU || notification == WM_RBUTTONUP) {
                    POINT location{
                        GET_X_LPARAM(w_param),
                        GET_Y_LPARAM(w_param),
                    };
                    if (notification == WM_RBUTTONUP) {
                        GetCursorPos(&location);
                    }
                    application->ShowExitMenu(location);
                }
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(w_param) == kExitCommand) {
                PostQuitMessage(0);
            }
            return 0;

        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;

        case WM_DESTROY:
            if (application != nullptr) {
                application->window_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window, message, w_param, l_param);
    }
}

}  // namespace magnify
