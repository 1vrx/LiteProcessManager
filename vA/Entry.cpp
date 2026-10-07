#include "com/sharedmemory.hpp"
#include "monitor/monitor.h"

void unload(PDRIVER_OBJECT driver) {
    UNREFERENCED_PARAMETER(driver);
    kernel_monitor::stop();
    com::stop();
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "KPM: unloaded\n");
}

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(registry_path);
    kernel_monitor::initialize();
    NTSTATUS status = kernel_monitor::start();
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "KPM: callback registration failed 0x%08X\n", status);
        return status;
    }
    status = com::start();
    if (!NT_SUCCESS(status)) {
        kernel_monitor::stop();
        com::stop();
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "KPM: channel startup failed 0x%08X\n", status);
        return status;
    }
    driver->DriverUnload = unload;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "KPM: shared-memory monitor ready\n");
    return STATUS_SUCCESS;
}
