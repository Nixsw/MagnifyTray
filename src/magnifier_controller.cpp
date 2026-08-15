#include "magnifier_controller.h"

namespace magnify {

namespace {

constexpr unsigned long kStartTimeoutMs = 2000;
constexpr unsigned long kGracefulCloseTimeoutMs = 1500;
constexpr unsigned long kForcedCloseTimeoutMs = 1000;

}  // namespace

MagnifierController::MagnifierController(IMagnifierBackend& backend) noexcept : backend_(backend) {}

ToggleResult MagnifierController::Toggle() {
    const ProcessQuery query = backend_.FindVerifiedProcesses();
    if (!query.ok) {
        return {ToggleStatus::Failed, query.error};
    }

    if (query.pids.empty()) {
        const auto error = backend_.StartAndVerify(kStartTimeoutMs);
        if (error.has_value()) {
            return {ToggleStatus::Failed, *error};
        }
        return {ToggleStatus::Started, {}};
    }

    const CloseAttempt close = backend_.RequestCloseAndWait(
        query.pids,
        kGracefulCloseTimeoutMs);
    if (close.error.has_value()) {
        return {ToggleStatus::Failed, *close.error};
    }

    if (!close.survivors.empty()) {
        const auto error = backend_.ForceTerminateVerified(
            close.survivors,
            kForcedCloseTimeoutMs);
        if (error.has_value()) {
            return {ToggleStatus::Failed, *error};
        }
    }

    return {ToggleStatus::Closed, {}};
}

}  // namespace magnify
