#pragma once
#include <ntifs.h>
#include "../../shared/event_ring.h"

namespace kernel_monitor {
void initialize();
NTSTATUS start();
void stop();
void snapshot(protocol::Response& response);
NTSTATUS events(const protocol::Request& request, protocol::Response& response);
NTSTATUS process(ULONG pid, protocol::Response& response);
}
