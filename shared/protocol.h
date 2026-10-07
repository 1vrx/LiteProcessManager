#pragma once
#include <stddef.h>
#if defined(KPM_KERNEL)

using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long long;
using int32_t = int;
#else
#include <stdint.h>
#endif

namespace protocol {
constexpr uint32_t magic = 0x4B504D32; // KPM2
constexpr uint32_t version = 2;
constexpr uint32_t ring_capacity = 128;
constexpr uint32_t batch_capacity = 16;
constexpr uint32_t image_chars = 64;
constexpr wchar_t section_name[] = L"Global\\KpmSharedDataV2";
constexpr wchar_t request_name[] = L"Global\\KpmRequestV2";
constexpr wchar_t response_name[] = L"Global\\KpmResponseV2";
constexpr wchar_t session_name[] = L"Global\\KpmClientV2";

enum class Command : uint32_t { info = 1, events = 2, process = 3 };
enum class State : int32_t { idle = 0, submitted = 1, processing = 2, complete = 3 };
enum class Kind : uint32_t { created = 1, exited = 2 };
enum class Validation { valid, bad_version, bad_size, bad_command, bad_arguments };

#pragma pack(push, 8)
struct Request {
    uint32_t version;
    uint32_t size;
    uint64_t id;
    Command command;
    uint32_t reserved;
    uint64_t epoch;
    uint64_t after_sequence;
    uint32_t process_id;
    uint32_t max_events;
};
struct Event {
    uint64_t sequence;
    uint64_t time; 
    uint32_t process_id;
    uint32_t parent_id; 
    Kind kind;
    uint32_t image_truncated;
    uint16_t image[image_chars]; 
};
struct Response {
    uint32_t version;
    uint32_t size;
    uint64_t id;
    int32_t status; 
    uint32_t bytes; 
    uint64_t epoch;
    uint64_t latest_sequence;
    uint64_t next_sequence;
    uint64_t dropped;
    uint64_t created;
    uint64_t exited;
    uint64_t process_create_time;
    int32_t process_exit_status;
    uint32_t process_id;
    uint32_t count;
    uint32_t reserved;
    Event events[batch_capacity];
};
struct SharedData {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    volatile int32_t ready;
    volatile int32_t state;
    uint32_t reserved;
    uint64_t epoch;
    Request request;
    Response response;
};
#pragma pack(pop)
static_assert(sizeof(Request) == 48, "Request ABI changed");
static_assert(sizeof(Event) == 160, "Event ABI changed");
static_assert(sizeof(Response) == 2656, "Response ABI changed");
static_assert(sizeof(SharedData) == 2736, "Shared ABI changed");
static_assert(offsetof(SharedData, state) % 4 == 0, "Interlocked alignment");

inline Validation validate(const Request& request) {
    if (request.version != version) return Validation::bad_version;
    if (request.size != sizeof(Request)) return Validation::bad_size;
    if (!request.id || request.reserved) return Validation::bad_arguments;
    switch (request.command) {
    case Command::info:
        if (request.epoch || request.after_sequence || request.process_id || request.max_events)
            return Validation::bad_arguments;
        break;
    case Command::events:
        if (!request.epoch || request.process_id || !request.max_events || request.max_events > batch_capacity)
            return Validation::bad_arguments;
        break;
    case Command::process:
        if (!request.process_id || request.epoch || request.after_sequence || request.max_events)
            return Validation::bad_arguments;
        break;
    default: return Validation::bad_command;
    }
    return Validation::valid;
}

inline uint32_t payload_bytes(uint32_t count) {
    return static_cast<uint32_t>(offsetof(Response, events) + count * sizeof(Event));
}
}
