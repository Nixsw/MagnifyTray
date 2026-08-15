#include "win32_process_backend.h"

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"MagnifyTray.HiddenWindow";

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

class Failure final : public std::runtime_error {
public:
    explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        throw Failure(message);
    }
}

std::wstring AdjacentExecutable(const wchar_t* const name) {
    wchar_t module_path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0) {
        throw Failure("unable to locate the live acceptance executable");
    }
    std::wstring path = module_path;
    const auto separator = path.find_last_of(L"\\/");
    path.resize(separator + 1);
    path += name;
    return path;
}

bool IsRunning(magnify::Win32MagnifierBackend& backend) {
    const auto query = backend.FindVerifiedProcesses();
    if (!query.ok) {
        throw Failure("unable to query verified Magnifier processes");
    }
    return !query.pids.empty();
}

bool WaitForState(
    magnify::Win32MagnifierBackend& backend,
    const bool expected_running,
    const DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        if (IsRunning(backend) == expected_running) {
            return true;
        }
        Sleep(50);
    } while (GetTickCount64() < deadline);
    return IsRunning(backend) == expected_running;
}

bool EnsureMagnifierClosed(magnify::Win32MagnifierBackend& backend) noexcept {
    const auto query = backend.FindVerifiedProcesses();
    if (!query.ok) {
        return false;
    }
    if (query.pids.empty()) {
        return true;
    }
    const auto close = backend.RequestCloseAndWait(query.pids, 1500);
    if (close.error.has_value()) {
        return false;
    }
    if (!close.survivors.empty()) {
        const auto terminate_error = backend.ForceTerminateVerified(close.survivors, 1000);
        if (terminate_error.has_value()) {
            return false;
        }
    }
    const auto final_query = backend.FindVerifiedProcesses();
    return final_query.ok && final_query.pids.empty();
}

class ApplicationProcess {
public:
    ApplicationProcess() {
        const std::wstring executable = AdjacentExecutable(L"MagnifyTray.exe");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                executable.c_str(),
                nullptr,
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                nullptr,
                &startup,
                &process)) {
            throw Failure("unable to launch MagnifyTray.exe");
        }
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        pid_ = process.dwProcessId;
    }

    ~ApplicationProcess() {
        const HWND window = FindHiddenWindowForProcess(pid_);
        if (window != nullptr) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        if (process_ != nullptr) {
            if (WaitForSingleObject(process_, 3000) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 1000);
            }
            CloseHandle(process_);
        }
    }

    ApplicationProcess(const ApplicationProcess&) = delete;
    ApplicationProcess& operator=(const ApplicationProcess&) = delete;

    DWORD pid() const noexcept { return pid_; }

private:
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
};

HWND WaitForApplicationWindow(const DWORD pid, const DWORD timeout_ms) {
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

void WaitForApplicationIdle(const HWND window) {
    // A cross-thread sent message can be dispatched while the target is still
    // inside its current handler. Let the backend's 50 ms state poll finish
    // before using WM_NULL as the final message-loop barrier.
    Sleep(250);

    DWORD_PTR ignored_result = 0;
    const LRESULT delivered = SendMessageTimeoutW(
        window,
        WM_NULL,
        0,
        0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        5000,
        &ignored_result);
    if (delivered == 0) {
        throw Failure("MagnifyTray did not finish its current toggle within 5 seconds");
    }
}

void SendAltW() {
    INPUT inputs[4]{};
    for (auto& input : inputs) {
        input.type = INPUT_KEYBOARD;
    }
    inputs[0].ki.wVk = VK_MENU;
    inputs[1].ki.wVk = 'W';
    inputs[2].ki.wVk = 'W';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].ki.wVk = VK_MENU;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    const UINT input_count = static_cast<UINT>(std::size(inputs));
    if (SendInput(input_count, inputs, sizeof(INPUT)) != input_count) {
        throw Failure("SendInput could not deliver Alt+W");
    }
}

void SendRepeatedAltWKeyDowns() {
    INPUT inputs[9]{};
    for (auto& input : inputs) {
        input.type = INPUT_KEYBOARD;
    }
    inputs[0].ki.wVk = VK_MENU;
    for (int index = 1; index <= 6; ++index) {
        inputs[index].ki.wVk = 'W';
    }
    inputs[7].ki.wVk = 'W';
    inputs[7].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[8].ki.wVk = VK_MENU;
    inputs[8].ki.dwFlags = KEYEVENTF_KEYUP;

    const UINT input_count = static_cast<UINT>(std::size(inputs));
    if (SendInput(input_count, inputs, sizeof(INPUT)) != input_count) {
        throw Failure("SendInput could not deliver repeated Alt+W key-down events");
    }
}

void RunLiveAcceptance() {
    magnify::Win32MagnifierBackend backend;
    const auto initialization_error = backend.Initialize();
    Expect(!initialization_error.has_value(), "backend initialization failed");
    Expect(EnsureMagnifierClosed(backend), "unable to establish a closed initial state");

    ApplicationProcess application;
    const HWND application_window = WaitForApplicationWindow(application.pid(), 5000);
    Expect(application_window != nullptr, "MagnifyTray hidden window did not appear");

    for (int cycle = 1; cycle <= 3; ++cycle) {
        const ULONGLONG start_begin = GetTickCount64();
        SendAltW();
        Expect(WaitForState(backend, true, 5000), "Alt+W did not start Magnifier");
        const ULONGLONG start_visible = GetTickCount64();
        WaitForApplicationIdle(application_window);
        Expect(IsRunning(backend), "Magnifier was not running after the start handler completed");
        const ULONGLONG start_complete = GetTickCount64();

        const ULONGLONG close_begin = GetTickCount64();
        SendAltW();
        Expect(WaitForState(backend, false, 5000), "Alt+W did not close Magnifier");
        const ULONGLONG close_visible = GetTickCount64();
        WaitForApplicationIdle(application_window);
        Expect(!IsRunning(backend), "Magnifier was still running after the close handler completed");
        const ULONGLONG close_complete = GetTickCount64();
        Expect(
            start_visible - start_begin <= 500,
            "Alt+W start latency exceeded 500 ms");
        Expect(
            close_visible - close_begin <= 500,
            "Alt+W close latency exceeded 500 ms");
        std::cout << "Cycle " << cycle
                  << ": start process=" << (start_visible - start_begin)
                  << " ms, start handler=" << (start_complete - start_begin)
                  << " ms, close process=" << (close_visible - close_begin)
                  << " ms, close handler=" << (close_complete - close_begin)
                  << " ms.\n";
    }

    const auto start_error = backend.StartAndVerify(2000);
    Expect(!start_error.has_value(), "external Magnifier start failed");
    SendAltW();
    Expect(WaitForState(backend, false, 5000), "Alt+W did not close externally started Magnifier");
    WaitForApplicationIdle(application_window);
    Expect(!IsRunning(backend), "external Magnifier was still running after close completed");
    std::cout << "External-start close passed.\n";

    SendRepeatedAltWKeyDowns();
    Expect(WaitForState(backend, true, 5000), "repeated key-down sequence did not start Magnifier");
    WaitForApplicationIdle(application_window);
    Expect(IsRunning(backend), "Magnifier was not running after repeated key-down handling completed");
    Sleep(500);
    Expect(IsRunning(backend), "MOD_NOREPEAT failed and repeated key-downs toggled Magnifier off");
    SendAltW();
    Expect(WaitForState(backend, false, 5000), "cleanup Alt+W did not close Magnifier");
    WaitForApplicationIdle(application_window);
    Expect(!IsRunning(backend), "Magnifier was still running after final cleanup completed");
    std::cout << "MOD_NOREPEAT hold behavior passed.\n";
}

}  // namespace

int main() {
    magnify::Win32MagnifierBackend cleanup_backend;
    cleanup_backend.Initialize();
    try {
        RunLiveAcceptance();
        EnsureMagnifierClosed(cleanup_backend);
        std::cout << "All MagnifyTray live acceptance tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        EnsureMagnifierClosed(cleanup_backend);
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
