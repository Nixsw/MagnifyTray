#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"MagnifyTray.HiddenWindow";
constexpr wchar_t kProductName[] = L"MagnifyTray";
constexpr int kTestHotkeyId = 0x4321;
constexpr UINT kTrayIconId = 1;

struct HiddenWindowSearch {
    DWORD pid = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindHiddenWindow(const HWND window, const LPARAM parameter) {
    auto* const search = reinterpret_cast<HiddenWindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid) {
        return TRUE;
    }

    wchar_t class_name[128]{};
    GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
    if (std::wstring(class_name) == kWindowClass) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindHiddenWindowForProcess(const DWORD pid) {
    HiddenWindowSearch search{pid, nullptr};
    EnumWindows(FindHiddenWindow, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

HWND FindPopupMenuForProcess(const DWORD pid) {
    struct MenuSearch {
        DWORD pid = 0;
        HWND window = nullptr;
    } search{pid, nullptr};

    EnumWindows(
        [](const HWND window, const LPARAM parameter) -> BOOL {
            auto* const candidate = reinterpret_cast<MenuSearch*>(parameter);
            DWORD window_pid = 0;
            GetWindowThreadProcessId(window, &window_pid);
            if (window_pid != candidate->pid) {
                return TRUE;
            }
            wchar_t class_name[32]{};
            GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
            if (std::wstring(class_name) == L"#32768") {
                candidate->window = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.window;
}

HWND WaitForPopupMenuForProcess(const DWORD pid, const DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        const HWND menu = FindPopupMenuForProcess(pid);
        if (menu != nullptr) {
            return menu;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return nullptr;
}

class Failure final : public std::runtime_error {
public:
    explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

[[noreturn]] void Fail(const std::string& message) {
    throw Failure(message);
}

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        Fail(message);
    }
}

std::wstring AdjacentExecutable(const wchar_t* const name) {
    wchar_t module_path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0) {
        Fail("unable to locate the lifecycle test executable");
    }
    std::wstring path = module_path;
    const auto separator = path.find_last_of(L"\\/");
    path.resize(separator + 1);
    path += name;
    return path;
}

class ManagedProcess {
public:
    explicit ManagedProcess(const std::wstring& path) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                path.c_str(),
                nullptr,
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                nullptr,
                &startup,
                &process)) {
            Fail("unable to launch MagnifyTray.exe");
        }
        CloseHandle(process.hThread);
        handle_ = process.hProcess;
        pid_ = process.dwProcessId;
    }

    ~ManagedProcess() {
        if (handle_ != nullptr) {
            const HWND window = FindHiddenWindowForProcess(pid_);
            if (window != nullptr) {
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            if (WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT) {
                TerminateProcess(handle_, 1);
                WaitForSingleObject(handle_, 2000);
            }
            CloseHandle(handle_);
        }
    }

    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;

    DWORD pid() const noexcept { return pid_; }

    bool WaitForExit(const DWORD timeout_ms) const noexcept {
        return WaitForSingleObject(handle_, timeout_ms) == WAIT_OBJECT_0;
    }

    bool IsRunning() const noexcept {
        return WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
    }

private:
    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;
};

HWND WaitForHiddenWindow(const DWORD pid, const DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        const HWND window = FindHiddenWindowForProcess(pid);
        if (window != nullptr) {
            return window;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return nullptr;
}

struct DialogSearch {
    DWORD pid = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindProductDialog(const HWND window, const LPARAM parameter) {
    auto* const search = reinterpret_cast<DialogSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid) {
        return TRUE;
    }

    wchar_t title[128]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (std::wstring(title) == kProductName) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND WaitForProductDialog(const DWORD pid, const DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        DialogSearch search{pid, nullptr};
        EnumWindows(FindProductDialog, reinterpret_cast<LPARAM>(&search));
        if (search.window != nullptr) {
            return search.window;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return nullptr;
}

void DismissTrayMenu(const DWORD pid, const HWND hidden_window) {
    PostMessageW(hidden_window, WM_CANCELMODE, 0, 0);
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (FindPopupMenuForProcess(pid) != nullptr && GetTickCount64() < deadline) {
        Sleep(25);
    }
    Expect(FindPopupMenuForProcess(pid) == nullptr, "tray popup menu did not dismiss");
}

void PostTrayContextMenu(const HWND hidden_window, const POINT location) {
    const WPARAM coordinates = static_cast<WPARAM>(MAKELPARAM(location.x, location.y));
    const LPARAM notification = MAKELPARAM(WM_CONTEXTMENU, kTrayIconId);
    Expect(
        PostMessageW(hidden_window, WM_APP + 1, coordinates, notification) != FALSE,
        "unable to deliver the tray context-menu callback");
}

void TestRightClickOnOpenTrayMenuDoesNotChooseExit() {
    const std::wstring executable = AdjacentExecutable(L"MagnifyTray.exe");
    ManagedProcess application(executable);
    const HWND hidden_window = WaitForHiddenWindow(application.pid(), 5000);
    Expect(hidden_window != nullptr, "right-click instance must create its hidden window");

    const POINT menu_location{1000, 800};
    PostTrayContextMenu(hidden_window, menu_location);
    const HWND popup_menu = WaitForPopupMenuForProcess(application.pid(), 2000);
    Expect(popup_menu != nullptr, "right-click test must open a popup menu");

    RECT menu_rectangle{};
    Expect(GetWindowRect(popup_menu, &menu_rectangle) != FALSE, "unable to read the tray menu bounds");
    const int item_x = (menu_rectangle.right - menu_rectangle.left) / 2;
    const int item_y = (menu_rectangle.bottom - menu_rectangle.top) / 2;
    const LPARAM item_location = MAKELPARAM(item_x, item_y);
    Expect(
        PostMessageW(popup_menu, WM_MOUSEMOVE, 0, item_location) != FALSE,
        "unable to move over the tray menu item");
    Expect(
        PostMessageW(popup_menu, WM_RBUTTONDOWN, MK_RBUTTON, item_location) != FALSE,
        "unable to deliver the tray menu right-button press");
    Expect(
        PostMessageW(popup_menu, WM_RBUTTONUP, 0, item_location) != FALSE,
        "unable to deliver the tray menu right-button release");
    Sleep(300);

    const bool survived_secondary_click = application.IsRunning();
    if (survived_secondary_click) {
        DismissTrayMenu(application.pid(), hidden_window);
        PostMessageW(hidden_window, WM_CLOSE, 0, 0);
    }
    Expect(
        survived_secondary_click,
        "a repeated right-click selected Exit from the open tray menu");
    Expect(application.WaitForExit(2000), "right-click test instance did not exit cleanly");
}

void TestRepeatedTrayRightClicksDoNotStrandApplication() {
    const std::wstring executable = AdjacentExecutable(L"MagnifyTray.exe");
    ManagedProcess application(executable);
    const HWND hidden_window = WaitForHiddenWindow(application.pid(), 5000);
    Expect(hidden_window != nullptr, "tray stress instance must create its hidden window");

    const POINT menu_location{1000, 800};
    PostTrayContextMenu(hidden_window, menu_location);
    const HWND popup_menu = WaitForPopupMenuForProcess(application.pid(), 2000);
    Expect(popup_menu != nullptr, "the first tray callback must open a popup menu");
    Expect(IsWindowVisible(popup_menu) != FALSE, "the tray popup menu must be visible");

    for (int click = 0; click < 32; ++click) {
        PostTrayContextMenu(hidden_window, menu_location);
    }
    Sleep(250);
    Expect(application.IsRunning(), "repeated tray callbacks terminated MagnifyTray");

    DismissTrayMenu(application.pid(), hidden_window);
    PostMessageW(hidden_window, WM_CLOSE, 0, 0);
    Expect(
        application.WaitForExit(2000),
        "one dismissed tray menu left nested menu loops that prevented shutdown");
}

void TestTrayMenuCanOpenTwice() {
    const std::wstring executable = AdjacentExecutable(L"MagnifyTray.exe");
    ManagedProcess application(executable);
    const HWND hidden_window = WaitForHiddenWindow(application.pid(), 5000);
    Expect(hidden_window != nullptr, "repeat-open instance must create its hidden window");

    const POINT menu_location{1000, 800};
    PostTrayContextMenu(hidden_window, menu_location);
    Expect(
        WaitForPopupMenuForProcess(application.pid(), 2000) != nullptr,
        "the first tray menu did not appear");
    DismissTrayMenu(application.pid(), hidden_window);

    PostTrayContextMenu(hidden_window, menu_location);
    const HWND second_menu = WaitForPopupMenuForProcess(application.pid(), 2000);
    Expect(second_menu != nullptr, "the second tray menu did not appear");
    Sleep(300);
    Expect(
        FindPopupMenuForProcess(application.pid()) != nullptr && IsWindowVisible(second_menu),
        "the second tray menu appeared and immediately disappeared");

    DismissTrayMenu(application.pid(), hidden_window);
    PostMessageW(hidden_window, WM_CLOSE, 0, 0);
    Expect(application.WaitForExit(2000), "repeat-open instance did not exit cleanly");
}

void TestLifecycleAndSingleInstance() {
    const std::wstring executable = AdjacentExecutable(L"MagnifyTray.exe");
    ManagedProcess first(executable);

    const HWND hidden_window = WaitForHiddenWindow(first.pid(), 5000);
    Expect(hidden_window != nullptr, "the first instance must create its hidden message window");

    const BOOL conflicting_registration = RegisterHotKey(
        nullptr,
        kTestHotkeyId,
        MOD_ALT | MOD_NOREPEAT,
        'W');
    if (conflicting_registration) {
        UnregisterHotKey(nullptr, kTestHotkeyId);
        Fail("MagnifyTray must own the Alt+W global hotkey while running");
    }

    ManagedProcess second(executable);
    const HWND duplicate_dialog = WaitForProductDialog(second.pid(), 5000);
    Expect(duplicate_dialog != nullptr, "a duplicate launch must show an already-running dialog");
    PostMessageW(hidden_window, WM_CLOSE, 0, 0);
    Expect(first.WaitForExit(3000), "primary instance must exit after WM_CLOSE");

    const BOOL registration_after_exit = RegisterHotKey(
        nullptr,
        kTestHotkeyId,
        MOD_ALT | MOD_NOREPEAT,
        'W');
    Expect(registration_after_exit, "Alt+W must be released after MagnifyTray exits");
    UnregisterHotKey(nullptr, kTestHotkeyId);

    ManagedProcess restarted(executable);
    const HWND restarted_window = WaitForHiddenWindow(restarted.pid(), 5000);
    Expect(restarted_window != nullptr, "an open duplicate dialog must not prevent restarting");
    PostMessageW(duplicate_dialog, WM_CLOSE, 0, 0);
    Expect(second.WaitForExit(3000), "duplicate instance must exit after its dialog closes");
    PostMessageW(restarted_window, WM_CLOSE, 0, 0);
    Expect(restarted.WaitForExit(3000), "restarted instance must exit cleanly");
}

}  // namespace

int main() {
    try {
        TestRightClickOnOpenTrayMenuDoesNotChooseExit();
        TestRepeatedTrayRightClicksDoNotStrandApplication();
        TestTrayMenuCanOpenTwice();
        TestLifecycleAndSingleInstance();
        std::cout << "All MagnifyTray lifecycle tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
