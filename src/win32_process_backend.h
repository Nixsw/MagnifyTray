#pragma once

#include "magnifier_controller.h"

#include <Windows.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace magnify {

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

enum class ExitCheck { AllowDeferred, Final };

// Keep the exact kernel objects across a timeout, including for multiple instances.
class ForcedExitBatch {
public:
    void Add(UniqueHandle process) { processes_.push_back(std::move(process)); }
    bool pending() const noexcept { return !processes_.empty(); }
    std::optional<ErrorInfo> Wait(DWORD timeout_ms, ExitCheck check);

private:
    std::vector<UniqueHandle> processes_;
};

struct FileIdentity {
    DWORD volume_serial = 0;
    unsigned long long file_index = 0;

    bool operator==(const FileIdentity& other) const noexcept {
        return volume_serial == other.volume_serial && file_index == other.file_index;
    }
};

struct ExecutableRecord {
    FileIdentity identity;
    std::wstring final_path;
};

std::optional<ExecutableRecord> ReadExecutableRecord(
    const std::wstring& path,
    ErrorInfo* error);

std::optional<FileIdentity> ReadFileIdentity(
    const std::wstring& path,
    ErrorInfo* error);

std::optional<DWORD> ReadProcessSessionId(
    unsigned long pid,
    ErrorInfo* error);

class Win32MagnifierBackend final : public IMagnifierBackend {
public:
    Win32MagnifierBackend() = default;

    std::optional<ErrorInfo> Initialize();

    ProcessQuery FindVerifiedProcesses() override;
    std::optional<ErrorInfo> StartAndVerify(unsigned long timeout_ms) override;
    CloseAttempt RequestCloseAndWait(
        const std::vector<unsigned long>& pids,
        unsigned long timeout_ms) override;
    std::optional<ErrorInfo> ForceTerminateVerified(
        const std::vector<unsigned long>& pids,
        unsigned long timeout_ms) override;
    bool HasPendingForcedExit() const noexcept { return forced_exits_.pending(); }
    std::optional<ErrorInfo> RecheckForcedExit(ExitCheck check) {
        return forced_exits_.Wait(0, check);
    }

private:
    bool IsVerifiedProcess(unsigned long pid, ErrorInfo* error) const;
    bool IsVerifiedProcessHandle(HANDLE process, ErrorInfo* error) const;

    std::wstring launch_path_;
    DWORD current_session_id_ = 0;
    std::vector<ExecutableRecord> target_executables_;
    ForcedExitBatch forced_exits_;
};

}  // namespace magnify
