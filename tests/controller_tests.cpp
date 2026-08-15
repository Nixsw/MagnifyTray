#include "magnifier_controller.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using magnify::CloseAttempt;
using magnify::ErrorInfo;
using magnify::IMagnifierBackend;
using magnify::MagnifierController;
using magnify::ProcessQuery;
using magnify::ToggleStatus;

class FakeBackend final : public IMagnifierBackend {
public:
    ProcessQuery query{true, {}, {}};
    std::optional<ErrorInfo> start_error;
    CloseAttempt close{{}, std::nullopt};
    std::optional<ErrorInfo> terminate_error;

    int start_calls = 0;
    int close_calls = 0;
    int terminate_calls = 0;
    unsigned long start_timeout_ms = 0;
    unsigned long close_timeout_ms = 0;
    unsigned long terminate_timeout_ms = 0;
    std::vector<unsigned long> close_pids;
    std::vector<unsigned long> terminate_pids;

    ProcessQuery FindVerifiedProcesses() override { return query; }

    std::optional<ErrorInfo> StartAndVerify(const unsigned long timeout_ms) override {
        ++start_calls;
        start_timeout_ms = timeout_ms;
        return start_error;
    }

    CloseAttempt RequestCloseAndWait(
        const std::vector<unsigned long>& pids,
        const unsigned long timeout_ms) override {
        ++close_calls;
        close_pids = pids;
        close_timeout_ms = timeout_ms;
        return close;
    }

    std::optional<ErrorInfo> ForceTerminateVerified(
        const std::vector<unsigned long>& pids,
        const unsigned long timeout_ms) override {
        ++terminate_calls;
        terminate_pids = pids;
        terminate_timeout_ms = timeout_ms;
        return terminate_error;
    }
};

[[noreturn]] void Fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        Fail(message);
    }
}

void TestStartsWhenNoVerifiedInstanceExists() {
    FakeBackend backend;
    MagnifierController controller(backend);

    const auto result = controller.Toggle();

    Expect(result.status == ToggleStatus::Started, "empty process set must start Magnifier");
    Expect(backend.start_calls == 1, "start must be attempted exactly once");
    Expect(backend.start_timeout_ms == 2000, "start verification timeout must be 2000 ms");
    Expect(backend.close_calls == 0, "close must not run while Magnifier is absent");
}

void TestReportsStartFailure() {
    FakeBackend backend;
    backend.start_error = ErrorInfo{5, L"启动被拒绝"};
    MagnifierController controller(backend);

    const auto result = controller.Toggle();

    Expect(result.status == ToggleStatus::Failed, "start failure must be returned");
    Expect(result.error.code == 5, "start failure must preserve the Win32 error code");
}

void TestGracefulCloseDoesNotForceTerminate() {
    FakeBackend backend;
    backend.query.pids = {41, 42};
    MagnifierController controller(backend);

    const auto result = controller.Toggle();

    Expect(result.status == ToggleStatus::Closed, "graceful exit must report closed");
    Expect(backend.close_pids == std::vector<unsigned long>({41, 42}),
           "all verified processes must receive graceful close");
    Expect(backend.close_timeout_ms == 1500, "graceful close timeout must be 1500 ms");
    Expect(backend.terminate_calls == 0, "force terminate must be skipped after graceful exit");
}

void TestForcesOnlyGracefulCloseSurvivors() {
    FakeBackend backend;
    backend.query.pids = {51, 52};
    backend.close.survivors = {52};
    MagnifierController controller(backend);

    const auto result = controller.Toggle();

    Expect(result.status == ToggleStatus::Closed, "successful fallback must report closed");
    Expect(backend.terminate_pids == std::vector<unsigned long>({52}),
           "only surviving verified processes may be force terminated");
    Expect(backend.terminate_timeout_ms == 1000, "force termination timeout must be 1000 ms");
}

void TestStopsOnProcessQueryFailure() {
    FakeBackend backend;
    backend.query = ProcessQuery{false, {}, ErrorInfo{87, L"无法核验进程"}};
    MagnifierController controller(backend);

    const auto result = controller.Toggle();

    Expect(result.status == ToggleStatus::Failed, "query failure must fail closed");
    Expect(result.error.code == 87, "query failure must preserve the Win32 error code");
    Expect(backend.start_calls == 0, "query failure must not start another Magnifier");
    Expect(backend.close_calls == 0, "query failure must not close an unknown process");
}

void TestReportsForcedTerminationFailure() {
    FakeBackend backend;
    backend.query.pids = {61};
    backend.close.survivors = {61};
    backend.terminate_error = ErrorInfo{5, L"强制关闭被拒绝"};
    MagnifierController controller(backend);

    const auto result = controller.Toggle();

    Expect(result.status == ToggleStatus::Failed, "force termination failure must be returned");
    Expect(result.error.code == 5, "force termination error code must be preserved");
}

}  // namespace

int main() {
    TestStartsWhenNoVerifiedInstanceExists();
    TestReportsStartFailure();
    TestGracefulCloseDoesNotForceTerminate();
    TestForcesOnlyGracefulCloseSurvivors();
    TestStopsOnProcessQueryFailure();
    TestReportsForcedTerminationFailure();
    std::cout << "All MagnifyTray controller tests passed.\n";
    return EXIT_SUCCESS;
}
