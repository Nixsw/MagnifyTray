#pragma once

#include <optional>
#include <string>
#include <vector>

namespace magnify {

struct ErrorInfo {
    unsigned long code = 0;
    std::wstring message;
};

struct ProcessQuery {
    bool ok = false;
    std::vector<unsigned long> pids;
    ErrorInfo error;
};

struct CloseAttempt {
    std::vector<unsigned long> survivors;
    std::optional<ErrorInfo> error;
};

enum class ToggleStatus {
    Started,
    Closed,
    Failed,
};

struct ToggleResult {
    ToggleStatus status = ToggleStatus::Failed;
    ErrorInfo error;
};

class IMagnifierBackend {
public:
    virtual ~IMagnifierBackend() = default;

    virtual ProcessQuery FindVerifiedProcesses() = 0;
    virtual std::optional<ErrorInfo> StartAndVerify(unsigned long timeout_ms) = 0;
    virtual CloseAttempt RequestCloseAndWait(
        const std::vector<unsigned long>& pids,
        unsigned long timeout_ms) = 0;
    virtual std::optional<ErrorInfo> ForceTerminateVerified(
        const std::vector<unsigned long>& pids,
        unsigned long timeout_ms) = 0;
};

class MagnifierController {
public:
    explicit MagnifierController(IMagnifierBackend& backend) noexcept;

    ToggleResult Toggle();

private:
    IMagnifierBackend& backend_;
};

}  // namespace magnify
