#include "win32_process_backend.h"

#include <TlHelp32.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <utility>

namespace magnify {

namespace {

ErrorInfo MakeError(const DWORD code, const wchar_t* message) {
    return {code, message};
}

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }

    HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void reset(const HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

std::wstring JoinWindowsPath(const std::wstring& root, const wchar_t* const suffix) {
    std::wstring path = root;
    if (!path.empty() && path.back() != L'\\') {
        path.push_back(L'\\');
    }
    path += suffix;
    return path;
}

bool IsProcessGoneError(const DWORD code) noexcept {
    return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_FOUND;
}

DWORD RemainingTimeout(const ULONGLONG deadline) noexcept {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
        return 0;
    }
    const ULONGLONG remaining = deadline - now;
    return remaining > MAXDWORD ? MAXDWORD : static_cast<DWORD>(remaining);
}

struct CloseWindowsContext {
    const std::vector<unsigned long>* pids = nullptr;
    std::vector<bool>* close_posted = nullptr;
};

BOOL CALLBACK CloseMagnifierWindow(const HWND window, const LPARAM parameter) {
    auto* const context = reinterpret_cast<CloseWindowsContext*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const auto process = std::find(context->pids->begin(), context->pids->end(), pid);
    if (process == context->pids->end()) {
        return TRUE;
    }

    if (PostMessageW(window, WM_CLOSE, 0, 0)) {
        const auto index = static_cast<std::size_t>(
            std::distance(context->pids->begin(), process));
        (*context->close_posted)[index] = true;
    }
    return TRUE;
}

}  // namespace

std::optional<ExecutableRecord> ReadExecutableRecord(
    const std::wstring& path,
    ErrorInfo* const error) {
    const HANDLE file = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = MakeError(GetLastError(), L"无法读取可执行文件标识");
        }
        return std::nullopt;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) {
        const DWORD code = GetLastError();
        CloseHandle(file);
        if (error != nullptr) {
            *error = MakeError(code, L"无法读取可执行文件信息");
        }
        return std::nullopt;
    }

    const unsigned long long file_index =
        (static_cast<unsigned long long>(information.nFileIndexHigh) << 32U) |
        static_cast<unsigned long long>(information.nFileIndexLow);

    const DWORD required_length = GetFinalPathNameByHandleW(
        file,
        nullptr,
        0,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required_length == 0) {
        const DWORD code = GetLastError();
        CloseHandle(file);
        if (error != nullptr) {
            *error = MakeError(code, L"无法规范化可执行文件路径");
        }
        return std::nullopt;
    }

    std::wstring final_path(static_cast<std::size_t>(required_length) + 1U, L'\0');
    const DWORD copied_length = GetFinalPathNameByHandleW(
        file,
        final_path.data(),
        static_cast<DWORD>(final_path.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const DWORD final_path_error = copied_length == 0 ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(file);
    if (copied_length == 0 || copied_length >= final_path.size()) {
        if (error != nullptr) {
            *error = MakeError(
                final_path_error == ERROR_SUCCESS ? ERROR_INSUFFICIENT_BUFFER : final_path_error,
                L"无法读取规范化可执行文件路径");
        }
        return std::nullopt;
    }
    final_path.resize(copied_length);

    return ExecutableRecord{
        FileIdentity{information.dwVolumeSerialNumber, file_index},
        std::move(final_path),
    };
}

std::optional<FileIdentity> ReadFileIdentity(
    const std::wstring& path,
    ErrorInfo* const error) {
    const auto record = ReadExecutableRecord(path, error);
    if (!record.has_value()) {
        return std::nullopt;
    }
    return record->identity;
}

std::optional<DWORD> ReadProcessSessionId(
    const unsigned long pid,
    ErrorInfo* const error) {
    DWORD session_id = 0;
    if (!ProcessIdToSessionId(pid, &session_id)) {
        if (error != nullptr) {
            *error = MakeError(GetLastError(), L"无法读取进程所属会话");
        }
        return std::nullopt;
    }
    return session_id;
}

std::optional<ErrorInfo> Win32MagnifierBackend::Initialize() {
    wchar_t windows_directory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return MakeError(GetLastError(), L"无法确定 Windows 系统目录");
    }

    launch_path_ = JoinWindowsPath(windows_directory, L"System32\\Magnify.exe");
    target_executables_.clear();

    ErrorInfo error;
    const auto current_session = ReadProcessSessionId(GetCurrentProcessId(), &error);
    if (!current_session.has_value()) {
        error.message = L"无法确定 MagnifyTray 所属用户会话";
        return error;
    }
    current_session_id_ = *current_session;

    const auto system32_record = ReadExecutableRecord(launch_path_, &error);
    if (!system32_record.has_value()) {
        error.message = L"无法核验 System32 中的系统放大镜";
        return error;
    }
    target_executables_.push_back(*system32_record);

    const std::wstring syswow64_path =
        JoinWindowsPath(windows_directory, L"SysWOW64\\Magnify.exe");
    const DWORD attributes = GetFileAttributesW(syswow64_path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        const auto syswow64_record = ReadExecutableRecord(syswow64_path, &error);
        if (!syswow64_record.has_value()) {
            error.message = L"无法核验 SysWOW64 中的系统放大镜";
            return error;
        }
        const auto existing = std::find_if(
            target_executables_.begin(),
            target_executables_.end(),
            [&syswow64_record](const ExecutableRecord& target) {
                return target.identity == syswow64_record->identity &&
                    _wcsicmp(
                        target.final_path.c_str(),
                        syswow64_record->final_path.c_str()) == 0;
            });
        if (existing == target_executables_.end()) {
            target_executables_.push_back(*syswow64_record);
        }
    } else {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            return MakeError(code, L"无法检查 SysWOW64 系统放大镜");
        }
    }

    return std::nullopt;
}

ProcessQuery Win32MagnifierBackend::FindVerifiedProcesses() {
    if (target_executables_.empty()) {
        return {false, {}, MakeError(ERROR_INVALID_STATE, L"放大镜后端尚未初始化")};
    }

    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return {false, {}, MakeError(GetLastError(), L"无法创建进程快照")};
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return {false, {}, MakeError(GetLastError(), L"无法读取进程快照")};
    }

    std::vector<unsigned long> pids;
    do {
        if (_wcsicmp(entry.szExeFile, L"Magnify.exe") != 0) {
            continue;
        }

        ErrorInfo error;
        if (IsVerifiedProcess(entry.th32ProcessID, &error)) {
            pids.push_back(entry.th32ProcessID);
            continue;
        }

        if (error.code == ERROR_INVALID_DATA || IsProcessGoneError(error.code)) {
            continue;
        }
        return {false, {}, error};
    } while (Process32NextW(snapshot.get(), &entry));

    const DWORD enumeration_error = GetLastError();
    if (enumeration_error != ERROR_NO_MORE_FILES) {
        return {false, {}, MakeError(enumeration_error, L"进程枚举未能完整结束")};
    }

    return {true, std::move(pids), {}};
}

std::optional<ErrorInfo> Win32MagnifierBackend::StartAndVerify(
    const unsigned long timeout_ms) {
    if (launch_path_.empty() || target_executables_.empty()) {
        return MakeError(ERROR_INVALID_STATE, L"放大镜后端尚未初始化");
    }

    SHELLEXECUTEINFOW execution{};
    execution.cbSize = sizeof(execution);
    execution.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execution.lpVerb = L"open";
    execution.lpFile = launch_path_.c_str();
    execution.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execution)) {
        return MakeError(GetLastError(), L"无法启动系统放大镜");
    }

    UniqueHandle process_handle(execution.hProcess);
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;

    do {
        const ProcessQuery query = FindVerifiedProcesses();
        if (!query.ok) {
            return query.error;
        }
        if (!query.pids.empty()) {
            return std::nullopt;
        }
        Sleep(50);
    } while (GetTickCount64() < deadline);

    return MakeError(WAIT_TIMEOUT, L"系统放大镜已启动，但未能在时限内确认进程");
}

CloseAttempt Win32MagnifierBackend::RequestCloseAndWait(
    const std::vector<unsigned long>& pids,
    const unsigned long timeout_ms) {
    std::vector<std::pair<unsigned long, UniqueHandle>> processes;
    processes.reserve(pids.size());
    for (const unsigned long pid : pids) {
        UniqueHandle process(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE,
            pid));
        if (process.get() == nullptr) {
            const DWORD code = GetLastError();
            if (IsProcessGoneError(code)) {
                continue;
            }
            return {{}, MakeError(code, L"无法打开系统放大镜进行正常关闭")};
        }
        if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
            continue;
        }

        ErrorInfo error;
        const auto session_id = ReadProcessSessionId(pid, &error);
        if (!session_id.has_value()) {
            if (IsProcessGoneError(error.code)) {
                continue;
            }
            return {{}, error};
        }
        if (*session_id != current_session_id_) {
            continue;
        }
        if (!IsVerifiedProcessHandle(process.get(), &error)) {
            if (error.code == ERROR_INVALID_DATA || IsProcessGoneError(error.code)) {
                continue;
            }
            return {{}, error};
        }
        processes.emplace_back(pid, std::move(process));
    }

    if (processes.empty()) {
        return {{}, std::nullopt};
    }

    std::vector<unsigned long> verified_pids;
    verified_pids.reserve(processes.size());
    for (const auto& process : processes) {
        verified_pids.push_back(process.first);
    }

    std::vector<bool> close_posted(verified_pids.size(), false);
    CloseWindowsContext context{&verified_pids, &close_posted};
    if (!EnumWindows(CloseMagnifierWindow, reinterpret_cast<LPARAM>(&context))) {
        return {{}, MakeError(GetLastError(), L"无法枚举系统放大镜窗口")};
    }

    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (std::size_t index = 0; index < processes.size(); ++index) {
        // A protected Magnifier window can reject WM_CLOSE through UIPI. Waiting
        // for such a process can only consume the whole graceful-close budget.
        if (!close_posted[index]) {
            continue;
        }
        auto& process = processes[index];
        if (WaitForSingleObject(process.second.get(), RemainingTimeout(deadline)) == WAIT_FAILED) {
            return {{}, MakeError(GetLastError(), L"等待系统放大镜正常退出失败")};
        }
    }

    std::vector<unsigned long> survivors;
    for (auto& process : processes) {
        if (WaitForSingleObject(process.second.get(), 0) == WAIT_TIMEOUT) {
            survivors.push_back(process.first);
        }
    }
    return {std::move(survivors), std::nullopt};
}

std::optional<ErrorInfo> Win32MagnifierBackend::ForceTerminateVerified(
    const std::vector<unsigned long>& pids,
    const unsigned long timeout_ms) {
    std::vector<UniqueHandle> terminated_processes;
    terminated_processes.reserve(pids.size());

    for (const unsigned long pid : pids) {
        UniqueHandle process(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE,
            pid));
        if (process.get() == nullptr) {
            const DWORD code = GetLastError();
            if (IsProcessGoneError(code)) {
                continue;
            }
            return MakeError(code, L"无法打开系统放大镜进行强制关闭");
        }
        if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
            continue;
        }

        ErrorInfo session_error;
        const auto session_id = ReadProcessSessionId(pid, &session_error);
        if (!session_id.has_value()) {
            if (IsProcessGoneError(session_error.code) ||
                WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
                continue;
            }
            return session_error;
        }
        if (*session_id != current_session_id_) {
            return MakeError(ERROR_ACCESS_DENIED, L"目标不属于当前用户会话，拒绝强制关闭");
        }

        ErrorInfo verification_error;
        if (!IsVerifiedProcessHandle(process.get(), &verification_error)) {
            if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
                continue;
            }
            return MakeError(ERROR_ACCESS_DENIED, L"目标不是已核验的系统放大镜，拒绝强制关闭");
        }

        if (!TerminateProcess(process.get(), ERROR_CANCELLED)) {
            if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
                continue;
            }
            return MakeError(GetLastError(), L"强制关闭系统放大镜失败");
        }
        terminated_processes.push_back(std::move(process));
    }

    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (auto& process : terminated_processes) {
        const DWORD wait_result = WaitForSingleObject(
            process.get(),
            RemainingTimeout(deadline));
        if (wait_result == WAIT_TIMEOUT) {
            return MakeError(WAIT_TIMEOUT, L"系统放大镜在强制关闭后仍未退出");
        }
        if (wait_result == WAIT_FAILED) {
            return MakeError(GetLastError(), L"无法确认系统放大镜已强制退出");
        }
    }
    return std::nullopt;
}

bool Win32MagnifierBackend::IsVerifiedProcess(
    const unsigned long pid,
    ErrorInfo* const error) const {
    if (target_executables_.empty()) {
        if (error != nullptr) {
            *error = MakeError(ERROR_INVALID_STATE, L"放大镜后端尚未初始化");
        }
        return false;
    }

    const auto initial_session_id = ReadProcessSessionId(pid, error);
    if (!initial_session_id.has_value()) {
        return false;
    }
    if (*initial_session_id != current_session_id_) {
        if (error != nullptr) {
            *error = MakeError(ERROR_INVALID_DATA, L"同名进程不属于当前用户会话");
        }
        return false;
    }

    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (process.get() == nullptr) {
        if (error != nullptr) {
            *error = MakeError(GetLastError(), L"无法打开同名进程进行身份核验");
        }
        return false;
    }

    const auto confirmed_session_id = ReadProcessSessionId(pid, error);
    if (!confirmed_session_id.has_value()) {
        return false;
    }
    if (*confirmed_session_id != current_session_id_) {
        if (error != nullptr) {
            *error = MakeError(ERROR_INVALID_DATA, L"同名进程在核验期间发生会话变化");
        }
        return false;
    }

    return IsVerifiedProcessHandle(process.get(), error);
}

bool Win32MagnifierBackend::IsVerifiedProcessHandle(
    const HANDLE process,
    ErrorInfo* const error) const {
    if (target_executables_.empty()) {
        if (error != nullptr) {
            *error = MakeError(ERROR_INVALID_STATE, L"放大镜后端尚未初始化");
        }
        return false;
    }

    wchar_t image_path[32768]{};
    DWORD path_length = static_cast<DWORD>(std::size(image_path));
    if (!QueryFullProcessImageNameW(process, 0, image_path, &path_length)) {
        if (error != nullptr) {
            *error = MakeError(GetLastError(), L"无法读取同名进程的完整路径");
        }
        return false;
    }

    ErrorInfo record_error;
    const auto executable = ReadExecutableRecord(image_path, &record_error);
    if (!executable.has_value()) {
        if (error != nullptr) {
            *error = record_error;
        }
        return false;
    }

    for (const auto& target : target_executables_) {
        if (target.identity == executable->identity &&
            _wcsicmp(target.final_path.c_str(), executable->final_path.c_str()) == 0) {
            return true;
        }
    }

    if (error != nullptr) {
        *error = MakeError(ERROR_INVALID_DATA, L"同名进程不是 Windows 系统放大镜");
    }
    return false;
}

}  // namespace magnify
