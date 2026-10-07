#pragma once
#include "protocol.h"

namespace monitor {
struct Ring {
    protocol::Event records[protocol::ring_capacity];
    uint64_t latest;
    uint64_t created;
    uint64_t exited;
};
inline void append(Ring& ring, protocol::Event event) {
    event.sequence = ++ring.latest;
    ring.records[(event.sequence - 1) % protocol::ring_capacity] = event;
    if (event.kind == protocol::Kind::created) ++ring.created;
    else ++ring.exited;
}
inline bool read(const Ring& ring, uint64_t after, uint32_t maximum, protocol::Response& response) {
    if (after > ring.latest || !maximum || maximum > protocol::batch_capacity) return false;
    response.latest_sequence = ring.latest;
    response.created = ring.created;
    response.exited = ring.exited;
    response.next_sequence = after;
    response.count = 0;
    response.dropped = 0;
    if (after == ring.latest) return true;
    const uint64_t oldest = ring.latest > protocol::ring_capacity ? ring.latest - protocol::ring_capacity + 1 : 1;
    uint64_t next = after + 1;
    if (next < oldest) {
        response.dropped = oldest - next;
        next = oldest;
    }
    while (next <= ring.latest && response.count < maximum) {
        response.events[response.count++] = ring.records[(next - 1) % protocol::ring_capacity];
        response.next_sequence = next++;
    }
    return true;
}
}
