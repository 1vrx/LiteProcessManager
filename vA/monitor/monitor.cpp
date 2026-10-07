#include "monitor.h"

namespace {
monitor::Ring ring;
KSPIN_LOCK ring_lock;
bool registered;

void notify(PEPROCESS process, HANDLE process_id, PPS_CREATE_NOTIFY_INFO create_info) {
    UNREFERENCED_PARAMETER(process);
    protocol::Event event{};
    LARGE_INTEGER time;
    KeQuerySystemTimePrecise(&time);
    event.time = static_cast<uint64_t>(time.QuadPart);
    event.process_id = HandleToULong(process_id);
    event.kind = create_info ? protocol::Kind::created : protocol::Kind::exited;
    if (create_info) {
        event.parent_id = HandleToULong(create_info->ParentProcessId);
        if (create_info->ImageFileName && create_info->ImageFileName->Buffer) {
            const USHORT available = create_info->ImageFileName->Length / sizeof(WCHAR);
            const USHORT copy_chars = available < protocol::image_chars ? available : protocol::image_chars - 1;
            RtlCopyMemory(event.image, create_info->ImageFileName->Buffer, copy_chars * sizeof(WCHAR));
            event.image_truncated = available >= protocol::image_chars ? 1u : 0u;
        }
    }
    KIRQL old_irql;
    KeAcquireSpinLock(&ring_lock, &old_irql);
    monitor::append(ring, event);
    KeReleaseSpinLock(&ring_lock, old_irql);
}
}

void kernel_monitor::initialize() {
    RtlZeroMemory(&ring, sizeof(ring));
    KeInitializeSpinLock(&ring_lock);
    registered = false;
}
NTSTATUS kernel_monitor::start() {
    const NTSTATUS status = PsSetCreateProcessNotifyRoutineEx(notify, FALSE);
    registered = NT_SUCCESS(status);
    return status;
}
void kernel_monitor::stop() {
    if (registered) {
        // Removal waits for in-flight callbacks. Storage remains alive until that finishes.
        const NTSTATUS status = PsSetCreateProcessNotifyRoutineEx(notify, TRUE);
        if (!NT_SUCCESS(status))
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "KPM: callback removal invariant failed 0x%08X\n", status);
        NT_ASSERT(NT_SUCCESS(status));
        registered = false;
    }
}
void kernel_monitor::snapshot(protocol::Response& response) {
    KIRQL old_irql;
    KeAcquireSpinLock(&ring_lock, &old_irql);
    response.latest_sequence = ring.latest;
    response.created = ring.created;
    response.exited = ring.exited;
    KeReleaseSpinLock(&ring_lock, old_irql);
}
NTSTATUS kernel_monitor::events(const protocol::Request& request, protocol::Response& response) {
    KIRQL old_irql;
    KeAcquireSpinLock(&ring_lock, &old_irql);
    const bool valid = monitor::read(ring, request.after_sequence, request.max_events, response);
    KeReleaseSpinLock(&ring_lock, old_irql);
    return valid ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
NTSTATUS kernel_monitor::process(ULONG pid, protocol::Response& response) {
    PEPROCESS process = nullptr;
    const NTSTATUS status = PsLookupProcessByProcessId(ULongToHandle(pid), &process);
    if (!NT_SUCCESS(status)) return status;
    response.process_id = HandleToULong(PsGetProcessId(process));
    response.process_create_time = static_cast<uint64_t>(PsGetProcessCreateTimeQuadPart(process));
    response.process_exit_status = PsGetProcessExitStatus(process);
    ObDereferenceObject(process);
    return STATUS_SUCCESS;
}
