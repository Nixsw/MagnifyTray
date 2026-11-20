#include "win32_process_backend.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

class TemporaryFile {
public:
    TemporaryFile() {
        wchar_t temp_path[MAX_PATH]{};
        wchar_t temp_file[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, temp_path) == 0 ||
            GetTempFileNameW(temp_path, L"MGT", 0, temp_file) == 0) {
            Fail("unable to create a temporary file");
        }
        path_ = temp_file;
    }

    ~TemporaryFile() {
        DeleteFileW(path_.c_str());
        if (!hard_link_.empty()) {
            DeleteFileW(hard_link_.c_str());
        }
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    const std::wstring& path() const noexcept { return path_; }

    std::wstring CreateHardLink() {
        hard_link_ = path_ + L".hardlink";
        if (!CreateHardLinkW(hard_link_.c_str(), path_.c_str(), nullptr)) {
            Fail("unable to create a hard link");
        }
        return hard_link_;
    }

private:
    std::wstring path_;
    std::wstring hard_link_;
};

class FixtureProcess {
public:
    FixtureProcess() {
        wchar_t module_path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, module_path, MAX_PATH) == 0) {
            Fail("unable to locate the test executable");
        }

        std::wstring fixture_path = module_path;
        const auto separator = fixture_path.find_last_of(L"\\/");
        fixture_path.resize(separator + 1);
        fixture_path += L"Magnify.exe";

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                fixture_path.c_str(),
                nullptr,
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process)) {
            Fail("unable to launch the same-name process fixture");
        }

        CloseHandle(process.hThread);
        process_ = process.hProcess;
        pid_ = process.dwProcessId;
    }

    ~FixtureProcess() {
        if (process_ != nullptr) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 2000);
            CloseHandle(process_);
        }
    }

    FixtureProcess(const FixtureProcess&) = delete;
    FixtureProcess& operator=(const FixtureProcess&) = delete;

    unsigned long pid() const noexcept { return pid_; }

    bool IsRunning() const noexcept {
        return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }

private:
    HANDLE process_ = nullptr;
    unsigned long pid_ = 0;
};

void TestSameFileHasSameIdentity() {
    TemporaryFile file;
    magnify::ErrorInfo error;

    const auto first = magnify::ReadFileIdentity(file.path(), &error);
    const auto second = magnify::ReadFileIdentity(file.path(), &error);

    Expect(first.has_value(), "an existing file must have an identity");
    Expect(second.has_value(), "the second identity query must succeed");
    Expect(*first == *second, "the same file must keep the same identity");
}

void TestHardLinkHasSameIdentity() {
    TemporaryFile file;
    const std::wstring hard_link = file.CreateHardLink();
    magnify::ErrorInfo error;

    const auto original = magnify::ReadFileIdentity(file.path(), &error);
    const auto linked = magnify::ReadFileIdentity(hard_link, &error);

    Expect(original.has_value() && linked.has_value(), "hard-link identity queries must succeed");
    Expect(*original == *linked, "hard links must compare as the same executable file");
}

void TestDifferentFilesHaveDifferentIdentity() {
    TemporaryFile first_file;
    TemporaryFile second_file;
    magnify::ErrorInfo error;

    const auto first = magnify::ReadFileIdentity(first_file.path(), &error);
    const auto second = magnify::ReadFileIdentity(second_file.path(), &error);

    Expect(first.has_value() && second.has_value(), "temporary file identity queries must succeed");
    Expect(!(*first == *second), "different files must not compare as the same executable");
}

void TestMissingFileFailsWithWin32Error() {
    magnify::ErrorInfo error;
    const auto identity = magnify::ReadFileIdentity(
        L"C:\\path-that-does-not-exist\\Magnify.exe",
        &error);

    Expect(!identity.has_value(), "missing file must not produce an identity");
    Expect(error.code != ERROR_SUCCESS, "missing file must preserve a Win32 error code");
}

void TestExecutableRecordDistinguishesHardLinkLocations() {
    TemporaryFile file;
    const std::wstring hard_link = file.CreateHardLink();
    magnify::ErrorInfo error;

    const auto original = magnify::ReadExecutableRecord(file.path(), &error);
    const auto linked = magnify::ReadExecutableRecord(hard_link, &error);

    Expect(original.has_value() && linked.has_value(), "executable records must be readable");
    Expect(original->identity == linked->identity, "hard links must keep the same file identity");
    Expect(_wcsicmp(original->final_path.c_str(), linked->final_path.c_str()) != 0,
           "hard-link locations must keep distinct normalized paths");
}

void TestCurrentProcessSessionCanBeRead() {
    magnify::ErrorInfo error;
    const auto session_id = magnify::ReadProcessSessionId(GetCurrentProcessId(), &error);

    Expect(session_id.has_value(), "current process session ID must be readable");
    DWORD expected_session_id = 0;
    Expect(ProcessIdToSessionId(GetCurrentProcessId(), &expected_session_id) != FALSE,
           "Windows must report the current process session ID");
    Expect(*session_id == expected_session_id, "session helper must preserve the Windows session ID");
}

void TestSameNameNonSystemProcessIsIgnoredAndCannotBeTerminated() {
    FixtureProcess fixture;
    magnify::Win32MagnifierBackend backend;
    const auto initialization_error = backend.Initialize();
    Expect(!initialization_error.has_value(), "backend initialization must find system Magnifier");

    const auto query = backend.FindVerifiedProcesses();
    Expect(query.ok, "process enumeration must succeed");
    const bool fixture_was_verified =
        std::find(query.pids.begin(), query.pids.end(), fixture.pid()) != query.pids.end();
    Expect(!fixture_was_verified, "same-name executable outside Windows directories must be ignored");

    const auto terminate_error = backend.ForceTerminateVerified({fixture.pid()}, 1000);
    Expect(terminate_error.has_value(), "unverified process termination must be rejected");
    Expect(terminate_error->code == ERROR_ACCESS_DENIED,
           "unverified process rejection must use ERROR_ACCESS_DENIED");
    Expect(fixture.IsRunning(), "unverified same-name process must remain alive");
}

// Manual-reset events have the same wait semantics as process termination:
// once signaled they remain signaled. They let us exercise a late exit reliably.
void TestLateForcedExits() {
    magnify::ForcedExitBatch batch;
    magnify::UniqueHandle first(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    magnify::UniqueHandle second(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Expect(first && second, "must create both waitable objects");
    const HANDLE first_signal = first.get();
    const HANDLE second_signal = second.get();
    batch.Add(std::move(first));
    batch.Add(std::move(second));

    const auto expired = batch.Wait(1, magnify::ExitCheck::AllowDeferred);
    Expect(expired && expired->code == WAIT_TIMEOUT && batch.pending(),
           "an initial timeout must retain the exact outstanding objects");
    Expect(SetEvent(second_signal) != FALSE, "second exit must signal");
    const auto partial = batch.Wait(0, magnify::ExitCheck::AllowDeferred);
    Expect(partial && partial->code == WAIT_TIMEOUT && batch.pending(),
           "a hotkey recheck must keep waiting for the other instance");
    Expect(SetEvent(first_signal) != FALSE, "first exit must signal");
    Expect(!batch.Wait(0, magnify::ExitCheck::Final) && !batch.pending(),
           "a late confirmed exit must succeed and release all handles");
}

void TestPersistentForcedExitTimeoutAndWaitError() {
    magnify::ForcedExitBatch batch;
    magnify::UniqueHandle original(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Expect(original != nullptr, "must create the unsignaled waitable object");
    HANDLE restricted = nullptr;
    Expect(DuplicateHandle(GetCurrentProcess(), original.get(), GetCurrentProcess(),
                           &restricted, EVENT_MODIFY_STATE, FALSE, 0) != FALSE,
           "must create an event handle without synchronize access");
    batch.Add(magnify::UniqueHandle(restricted));
    const auto failed = batch.Wait(0, magnify::ExitCheck::AllowDeferred);
    Expect(failed && failed->code == ERROR_ACCESS_DENIED && !batch.pending(),
           "a real wait failure must report its own error immediately");
    batch.Add(std::move(original));
    const auto expired = batch.Wait(0, magnify::ExitCheck::Final);
    Expect(expired && expired->code == WAIT_TIMEOUT && !batch.pending(),
           "an unconfirmed final exit must report timeout and release handles");
}

}  // namespace

int main() {
    try {
        TestSameFileHasSameIdentity();
        TestHardLinkHasSameIdentity();
        TestDifferentFilesHaveDifferentIdentity();
        TestMissingFileFailsWithWin32Error();
        TestExecutableRecordDistinguishesHardLinkLocations();
        TestCurrentProcessSessionCanBeRead();
        TestSameNameNonSystemProcessIsIgnoredAndCannotBeTerminated();
        TestLateForcedExits();
        TestPersistentForcedExitTimeoutAndWaitError();
        std::cout << "All MagnifyTray file identity tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
