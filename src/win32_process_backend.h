#pragma once

#include "magnifier_controller.h"

#include <Windows.h>

#include <optional>
#include <string>
#include <vector>

namespace magnify {

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

private:
    bool IsVerifiedProcess(unsigned long pid, ErrorInfo* error) const;
    bool IsVerifiedProcessHandle(HANDLE process, ErrorInfo* error) const;

    std::wstring launch_path_;
    DWORD current_session_id_ = 0;
    std::vector<ExecutableRecord> target_executables_;
};

}  // namespace magnify
