#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include "../shared/event_ring.h"
#include "../um/com/com.h"

namespace {
const client::Names mock_names{L"Local\\KpmTestData", L"Local\\KpmTestRequest", L"Local\\KpmTestResponse", L"Local\\KpmTestClient"};
int checks;
void check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << name << '\n'; throw name; }
}
protocol::Request make_request(protocol::Command command) {
    protocol::Request request{}; request.version = protocol::version;
    request.size = sizeof(request); request.id = 1; request.command = command;
    return request;
}
void core_tests() {
    auto request = make_request(protocol::Command::info);
    check(protocol::validate(request) == protocol::Validation::valid, "info request");
    request.version = 99;
    check(protocol::validate(request) == protocol::Validation::bad_version, "version mismatch");
    request = make_request(protocol::Command::info); --request.size;
    check(protocol::validate(request) == protocol::Validation::bad_size, "truncated request");
    request = make_request(protocol::Command::info); request.reserved = 1;
    check(protocol::validate(request) == protocol::Validation::bad_arguments, "reserved fields rejected");
    request = make_request(static_cast<protocol::Command>(0xFFFFFFFF));
    check(protocol::validate(request) == protocol::Validation::bad_command, "unknown command");
    request = make_request(protocol::Command::process);
    check(protocol::validate(request) == protocol::Validation::bad_arguments, "zero pid");
    request.process_id = 100;
    check(protocol::validate(request) == protocol::Validation::valid, "valid process query");
    request = make_request(protocol::Command::events); request.epoch = 1; request.max_events = 17;
    check(protocol::validate(request) == protocol::Validation::bad_arguments, "oversized batch");
    request.max_events = 16;
    check(protocol::validate(request) == protocol::Validation::valid, "valid event request");
    monitor::Ring ring{}; protocol::Response response{};
    check(monitor::read(ring, 0, 16, response) && response.count == 0 && response.next_sequence == 0, "empty ring");
    check(!monitor::read(ring, 1, 16, response), "future cursor rejected");
    for (uint32_t i = 1; i <= 150; ++i) {
        protocol::Event event{}; event.process_id = i; event.kind = i % 2 ? protocol::Kind::created : protocol::Kind::exited;
        monitor::append(ring, event);
    }
    check(monitor::read(ring, 0, 16, response), "overflow batch readable");
    check(response.dropped == 22 && response.count == 16 && response.events[0].sequence == 23 && response.next_sequence == 38,
          "overwritten events reported exactly");
    uint64_t cursor = response.next_sequence; uint32_t total = response.count;
    while (cursor < 150) {
        check(monitor::read(ring, cursor, 16, response), "subsequent batch");
        check(!response.dropped && response.events[0].sequence == cursor + 1, "no duplicate or missing retained event");
        total += response.count; cursor = response.next_sequence;
    }
    check(total == 128 && cursor == 150, "all retained events read once");
    protocol::Response second{};
    check(monitor::read(ring, 140, 16, second) && second.count == 10, "independent reader cursor");
    check(ring.created == 75 && ring.exited == 75, "counters survive ring overwrite");
    check(protocol::payload_bytes(0) == 96 && protocol::payload_bytes(16) == sizeof(protocol::Response), "payload length contract");

    // Exercise the documented caller-held-lock contract with simultaneous producer/reader activity.
    monitor::Ring concurrent{}; std::mutex lock; std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint32_t i = 1; i <= 10000; ++i) {
            protocol::Event event{}; event.process_id = i; event.kind = protocol::Kind::created;
            std::lock_guard<std::mutex> guard(lock); monitor::append(concurrent, event);
        }
        done.store(true);
    });
    cursor = 0;
    bool ordered = true;
    do {
        std::lock_guard<std::mutex> guard(lock);
        protocol::Response batch{};
        if (!monitor::read(concurrent, cursor, 16, batch)) ordered = false;
        for (uint32_t i = 0; i < batch.count; ++i) {
            if (batch.events[i].sequence != cursor + batch.dropped + 1 + i || batch.events[i].process_id != batch.events[i].sequence) ordered = false;
        }
        cursor = batch.next_sequence;
    } while (!done.load() || cursor < 10000);
    producer.join();
    check(ordered && concurrent.created == 10000, "concurrent readers see consistent ordered batches");
}

// User-mode transport simulation; no driver loading or privileged memory operations.
struct Mock {
    enum class Mode { normal, bad_length, wrong_id };
    std::atomic<Mode> mode{Mode::normal};
    HANDLE section = nullptr, request = nullptr, response = nullptr;
    protocol::SharedData* data = nullptr;
    std::atomic<bool> stop{false};
    std::thread thread;
    ~Mock() {
        stop.store(true); if (request) SetEvent(request);
        if (thread.joinable()) thread.join();
        if (data) UnmapViewOfFile(data);
        for (HANDLE handle : {response, request, section}) if (handle) CloseHandle(handle);
    }
    void start() {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, mock_names.section);
        DWORD error = GetLastError();
        if (!section || error == ERROR_ALREADY_EXISTS) throw "mock requires unused local test names";
        request = CreateEventW(nullptr, FALSE, FALSE, mock_names.request);
        error = GetLastError();
        if (!request || error == ERROR_ALREADY_EXISTS) throw "request object exists";
        response = CreateEventW(nullptr, FALSE, FALSE, mock_names.response);
        error = GetLastError();
        if (!response || error == ERROR_ALREADY_EXISTS) throw "response object exists";
        data = static_cast<protocol::SharedData*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*data)));
        check(data != nullptr, "mock mapping");
        *data = {};
        data->magic = protocol::magic; data->version = protocol::version; data->size = sizeof(*data); data->epoch = 123;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&data->ready), 1);
        thread = std::thread([&] {
            while (!stop.load()) {
                if (WaitForSingleObject(request, 100) != WAIT_OBJECT_0 || stop.load()) continue;
                if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&data->state), 2, 1) != 1) continue;
                const auto input = data->request;
                if (input.after_sequence == 999) continue; // deliberate unresponsive peer
                protocol::Response output{};
                output.version = protocol::version; output.size = sizeof(output); output.id = input.id;
                output.epoch = 123; output.bytes = protocol::payload_bytes(0);
                if (protocol::validate(input) != protocol::Validation::valid) output.status = static_cast<int32_t>(0xC000000D);
                if (mode.load() == Mode::bad_length) ++output.bytes;
                if (mode.load() == Mode::wrong_id) ++output.id;
                data->response = output;
                InterlockedExchange(reinterpret_cast<volatile LONG*>(&data->state), 3);
                SetEvent(response);
            }
        });
    }
};
void transport_tests() {
    Mock mock; mock.start(); client::Channel channel(mock_names);
    check(channel.open(), "client opens shared section/events");
    // A Win32 mutex is recursive for the owning thread; test exclusion from a different thread.
    bool excluded = false;
    std::thread contender([&] { client::Channel other(mock_names); excluded = !other.open(); }); contender.join();
    check(excluded, "second frontend excluded");
    protocol::Response output{};
    check(channel.exchange(make_request(protocol::Command::info), output) && output.id != 0, "request/response correlation");
    auto invalid = make_request(protocol::Command::process);
    check(!channel.exchange(invalid, output) && output.status < 0 && channel.epoch() == 123, "operation failure does not falsely succeed or destroy healthy channel");
    check(channel.exchange(make_request(protocol::Command::info), output), "healthy request after rejection");
    mock.mode.store(Mock::Mode::bad_length);
    check(!channel.exchange(make_request(protocol::Command::info), output) && !channel.epoch(), "malformed payload length disconnects");
    mock.mode.store(Mock::Mode::normal);
    check(channel.open(), "reopen after malformed response");
    mock.mode.store(Mock::Mode::wrong_id);
    check(!channel.exchange(make_request(protocol::Command::info), output) && !channel.epoch(), "stale response id cannot complete a different request");
    mock.mode.store(Mock::Mode::normal);
    check(channel.open(), "reopen after unmatched response");
    auto timeout = make_request(protocol::Command::events); timeout.epoch = 123; timeout.max_events = 16; timeout.after_sequence = 999;
    const auto start = std::chrono::steady_clock::now();
    check(!channel.exchange(timeout, output) && !channel.epoch(), "timeout disconnects without reusing in-flight slot");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(4), "finite timeout");
    const client::Names absent{L"Local\\KpmTestAbsentData", L"Local\\KpmTestAbsentRequest", L"Local\\KpmTestAbsentResponse", L"Local\\KpmTestAbsentClient"};
    client::Channel missing(absent);
    check(!missing.open() && !missing.epoch() && !missing.error().empty(), "missing driver produces a useful connection error");
}
void live_tests() {
    client::Channel channel;
    check(channel.open(), "open live driver");
    protocol::Response output{};
    check(channel.exchange(make_request(protocol::Command::info), output), "live info");
    auto query = make_request(protocol::Command::process); query.process_id = GetCurrentProcessId();
    check(channel.exchange(query, output) && output.process_id == GetCurrentProcessId() && output.process_create_time != 0, "kernel queries test process");
    query.process_id = 0;
    check(!channel.exchange(query, output) && output.status < 0, "live invalid pid rejected");
    auto invalid = make_request(protocol::Command::info); invalid.version = 99;
    check(!channel.exchange(invalid, output) && output.status < 0, "live unsupported protocol rejected");
    check(channel.exchange(make_request(protocol::Command::info), output), "driver remains usable after malformed requests");
    uint64_t cursor = output.latest_sequence;
    wchar_t system_directory[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (!length || length >= MAX_PATH) throw "system directory unavailable";
    const std::wstring executable = std::wstring(system_directory) + L"\\cmd.exe";
    wchar_t arguments[] = L"cmd.exe /c exit /b 0";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    check(CreateProcessW(executable.c_str(), arguments, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &child) != FALSE, "spawn controlled event test process");
    const DWORD child_pid = child.dwProcessId;
    const DWORD wait = WaitForSingleObject(child.hProcess, 5000);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    check(wait == WAIT_OBJECT_0, "controlled process exits");
    bool created = false, exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        auto read = make_request(protocol::Command::events);
        read.epoch = channel.epoch(); read.after_sequence = cursor; read.max_events = protocol::batch_capacity;
        if (!channel.exchange(read, output)) throw "live event read failed";
        for (uint32_t i = 0; i < output.count; ++i) {
            if (output.events[i].process_id == child_pid) {
                created |= output.events[i].kind == protocol::Kind::created;
                exited |= output.events[i].kind == protocol::Kind::exited;
            }
        }
        cursor = output.next_sequence;
        if (cursor == output.latest_sequence) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while ((!created || !exited) && std::chrono::steady_clock::now() < deadline);
    check(created && exited, "kernel callback records controlled create and exit events");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 1) core_tests();
        else if (argc == 2 && strcmp(argv[1], "--transport") == 0) transport_tests();
        else if (argc == 2 && strcmp(argv[1], "--live") == 0) live_tests();
        else { std::cerr << "Usage: KpmTests [--transport | --live]\n"; return 2; }
        std::cout << "PASS: " << checks << " checks\n";
        return 0;
    } catch (const char* failure) { std::cerr << failure << '\n'; return 1; }
}
