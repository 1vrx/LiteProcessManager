#include "com.h"
#include <sstream>

std::wstring client::error_text(DWORD code) {
    wchar_t* message = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = size ? std::wstring(message, size) : L"Unknown error";
    if (message) LocalFree(message);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return text + L" (" + std::to_wstring(code) + L")";
}
bool client::Channel::fail(const wchar_t* operation, DWORD code) {
    error_ = std::wstring(operation) + L": " + error_text(code);
    close();
    return false;
}
void client::Channel::close() {
    if (shared_) { UnmapViewOfFile(shared_); shared_ = nullptr; }
    for (auto handle : { response_, request_, section_ }) if (handle) CloseHandle(handle);
    response_ = request_ = section_ = nullptr;
    if (session_owned_) { ReleaseMutex(session_); session_owned_ = false; }
    if (session_) { CloseHandle(session_); session_ = nullptr; }
    epoch_ = 0;
}
bool client::Channel::open() {
    close();
    error_.clear();
    session_ = CreateMutexW(nullptr, FALSE, names_.session);
    const DWORD create_error = GetLastError();
    if (!session_) return fail(L"Open session lease", create_error);
    const DWORD lease = WaitForSingleObject(session_, 0);
    if (lease != WAIT_OBJECT_0 && lease != WAIT_ABANDONED)
        return fail(L"Another monitor owns the channel", lease == WAIT_FAILED ? GetLastError() : ERROR_BUSY);
    session_owned_ = true;
    section_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, names_.section);
    if (!section_) return fail(L"Open shared section (driver running? elevated?)", GetLastError());
    shared_ = static_cast<protocol::SharedData*>(MapViewOfFile(section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(protocol::SharedData)));
    if (!shared_) return fail(L"Map shared section", GetLastError());
    request_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, names_.request);
    if (!request_) return fail(L"Open request event", GetLastError());
    response_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, names_.response);
    if (!response_) return fail(L"Open response event", GetLastError());
    if (shared_->magic != protocol::magic || shared_->version != protocol::version ||
        shared_->size != sizeof(*shared_) || !InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared_->ready), 0, 0))
        return fail(L"Driver/protocol not ready", ERROR_REVISION_MISMATCH);
    epoch_ = shared_->epoch;
    const ULONGLONG deadline = GetTickCount64() + 1500;
    for (;;) {
        const LONG state = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared_->state), 0, 0);
        if (state == static_cast<LONG>(protocol::State::idle) || state == static_cast<LONG>(protocol::State::complete)) break;
        if (state != static_cast<LONG>(protocol::State::submitted) && state != static_cast<LONG>(protocol::State::processing))
            return fail(L"Invalid channel state", ERROR_INVALID_DATA);
        if (GetTickCount64() >= deadline) return fail(L"Previous request did not complete", ERROR_TIMEOUT);
        if (WaitForSingleObject(response_, 50) == WAIT_FAILED)
            return fail(L"Wait for previous request", GetLastError());
    }
    if (!ResetEvent(response_)) return fail(L"Reset response event", GetLastError());
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared_->state), static_cast<LONG>(protocol::State::idle));
    return true;
}
bool client::Channel::exchange(protocol::Request request, protocol::Response& response) {
    response = {};
    if (!shared_) { error_ = L"Channel is disconnected"; return false; }
    if (!InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared_->ready), 0, 0) || shared_->epoch != epoch_)
        return fail(L"Driver disconnected or restarted", ERROR_DEVICE_NOT_CONNECTED);
    request.id = ++id_;
    if (!ResetEvent(response_)) return fail(L"Reset response event", GetLastError());
    shared_->request = request;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared_->state), static_cast<LONG>(protocol::State::submitted));
    if (!SetEvent(request_)) return fail(L"Signal request", GetLastError());
    const ULONGLONG deadline = GetTickCount64() + 1500;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return fail(L"Driver response timeout", ERROR_TIMEOUT);
        const DWORD result = WaitForSingleObject(response_, static_cast<DWORD>(deadline - now));
        if (result != WAIT_OBJECT_0) return fail(L"Wait for response", result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        if (!InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared_->ready), 0, 0))
            return fail(L"Driver disconnected", ERROR_DEVICE_NOT_CONNECTED);
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared_->state), 0, 0) != static_cast<LONG>(protocol::State::complete)) continue;
        response = shared_->response;
        if (response.id != request.id) continue;
        if (response.version != protocol::version || response.size != sizeof(response) || response.epoch != epoch_ ||
            response.count > protocol::batch_capacity || response.bytes != protocol::payload_bytes(response.count))
            return fail(L"Malformed driver response", ERROR_INVALID_DATA);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared_->state), static_cast<LONG>(protocol::State::idle));
        if (response.status < 0) {
            std::wostringstream message;
            message << L"Driver rejected request: NTSTATUS 0x" << std::hex << static_cast<uint32_t>(response.status);
            error_ = message.str();
            return false;
        }
        error_.clear();
        return true;
    }
}
