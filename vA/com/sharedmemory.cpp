#include "sharedmemory.hpp"
#include "../monitor/monitor.h"

namespace {
HANDLE section_handle;
HANDLE request_handle;
HANDLE response_handle;
HANDLE worker_handle;
protocol::SharedData* shared;
volatile LONG stopping;
uint64_t epoch;
KEVENT started;
NTSTATUS start_status;

NTSTATUS descriptor(SECURITY_DESCRIPTOR& security, ACL* acl, ULONG capacity) {
    NTSTATUS status = RtlCreateSecurityDescriptor(&security, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(status)) return status;
    status = RtlCreateAcl(acl, capacity, ACL_REVISION);
    if (!NT_SUCCESS(status)) return status;
    status = RtlAddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, SeExports->SeLocalSystemSid);
    if (!NT_SUCCESS(status)) return status;
    status = RtlAddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, SeExports->SeAliasAdminsSid);
    if (!NT_SUCCESS(status)) return status;
    return RtlSetDaclSecurityDescriptor(&security, TRUE, acl, FALSE);
}
NTSTATUS event(HANDLE& handle, PCWSTR name, SECURITY_DESCRIPTOR* security) {
    UNICODE_STRING object_name;
    RtlInitUnicodeString(&object_name, name);
    OBJECT_ATTRIBUTES attributes;
    // Fail on name collision rather than adopting an object created by another process.
    InitializeObjectAttributes(&attributes, &object_name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, security);
    return ZwCreateEvent(&handle, EVENT_ALL_ACCESS, &attributes, SynchronizationEvent, FALSE);
}
NTSTATUS execute(const protocol::Request& request, protocol::Response& response) {
    const auto validation = protocol::validate(request);
    if (validation == protocol::Validation::bad_version) return STATUS_REVISION_MISMATCH;
    if (validation != protocol::Validation::valid) return STATUS_INVALID_PARAMETER;
    switch (request.command) {
    case protocol::Command::info:
        kernel_monitor::snapshot(response);
        return STATUS_SUCCESS;
    case protocol::Command::events:
        if (request.epoch != epoch) return STATUS_REVISION_MISMATCH;
        return kernel_monitor::events(request, response);
    case protocol::Command::process:
        return kernel_monitor::process(request.process_id, response);
    default: return STATUS_INVALID_PARAMETER;
    }
}
void worker(PVOID context) {
    UNREFERENCED_PARAMETER(context);
    PVOID base = nullptr;
    SIZE_T view_size = 0;
    start_status = ZwMapViewOfSection(section_handle, ZwCurrentProcess(), &base, 0, 0, nullptr,
                                      &view_size, ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(start_status)) {
        KeSetEvent(&started, IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(start_status);
        return;
    }
    shared = static_cast<protocol::SharedData*>(base);
    if (view_size < sizeof(*shared)) {
        start_status = STATUS_BUFFER_TOO_SMALL;
        ZwUnmapViewOfSection(ZwCurrentProcess(), shared);
        shared = nullptr;
        KeSetEvent(&started, IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(start_status);
        return;
    }
    RtlZeroMemory(shared, sizeof(*shared));
    LARGE_INTEGER time;
    KeQuerySystemTimePrecise(&time);
    epoch = static_cast<uint64_t>(time.QuadPart);
    shared->magic = protocol::magic;
    shared->version = protocol::version;
    shared->size = sizeof(*shared);
    shared->epoch = epoch;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->ready), 1);
    KeSetEvent(&started, IO_NO_INCREMENT, FALSE);
    while (!InterlockedCompareExchange(&stopping, 0, 0)) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -10'000'000; 
        const NTSTATUS wait = ZwWaitForSingleObject(request_handle, FALSE, &timeout);
        if (InterlockedCompareExchange(&stopping, 0, 0)) break;
        if (wait == STATUS_TIMEOUT) continue;
        if (!NT_SUCCESS(wait)) break;
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared->state),
                static_cast<LONG>(protocol::State::processing),
                static_cast<LONG>(protocol::State::submitted)) != static_cast<LONG>(protocol::State::submitted))
            continue;
        const protocol::Request request = shared->request;
        protocol::Response response{};
        response.version = protocol::version;
        response.size = sizeof(response);
        response.id = request.id;
        response.epoch = epoch;
        response.status = execute(request, response);
        if (!NT_SUCCESS(response.status)) {
            RtlZeroMemory(reinterpret_cast<UCHAR*>(&response) + offsetof(protocol::Response, latest_sequence),
                          sizeof(response) - offsetof(protocol::Response, latest_sequence));
        }
        response.bytes = protocol::payload_bytes(response.count);
        shared->response = response;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->state), static_cast<LONG>(protocol::State::complete));
        const NTSTATUS signal = ZwSetEvent(response_handle, nullptr);
        if (!NT_SUCCESS(signal)) break;
    }
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->ready), 0);
    if (response_handle) ZwSetEvent(response_handle, nullptr);
    ZwUnmapViewOfSection(ZwCurrentProcess(), shared);
    shared = nullptr;
    PsTerminateSystemThread(STATUS_SUCCESS);
}
void close(HANDLE& handle) {
    if (handle) { ZwClose(handle); handle = nullptr; }
}
}

NTSTATUS com::start() {
    InterlockedExchange(&stopping, 0);
    SECURITY_DESCRIPTOR security;
    alignas(ACL) UCHAR acl_storage[128];
    NTSTATUS status = descriptor(security, reinterpret_cast<ACL*>(acl_storage), sizeof(acl_storage));
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\KpmSharedDataV2");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, &security);
    LARGE_INTEGER maximum;
    maximum.QuadPart = PAGE_SIZE;
    status = ZwCreateSection(&section_handle, SECTION_ALL_ACCESS, &attributes, &maximum,
                             PAGE_READWRITE, SEC_COMMIT, nullptr);
    if (!NT_SUCCESS(status)) return status;
    status = event(request_handle, L"\\BaseNamedObjects\\KpmRequestV2", &security);
    if (!NT_SUCCESS(status)) return status;
    status = event(response_handle, L"\\BaseNamedObjects\\KpmResponseV2", &security);
    if (!NT_SUCCESS(status)) return status;
    OBJECT_ATTRIBUTES thread_attributes;
    InitializeObjectAttributes(&thread_attributes, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
    KeInitializeEvent(&started, NotificationEvent, FALSE);
    start_status = STATUS_UNSUCCESSFUL;
    status = PsCreateSystemThread(&worker_handle, SYNCHRONIZE, &thread_attributes, nullptr, nullptr, worker, nullptr);
    if (!NT_SUCCESS(status)) return status;
    KeWaitForSingleObject(&started, Executive, KernelMode, FALSE, nullptr);
    return start_status;
}
void com::stop() {
    InterlockedExchange(&stopping, 1);
    if (request_handle) ZwSetEvent(request_handle, nullptr);
    if (worker_handle) ZwWaitForSingleObject(worker_handle, FALSE, nullptr);
    close(worker_handle);
    close(response_handle);
    close(request_handle);
    close(section_handle);
}
