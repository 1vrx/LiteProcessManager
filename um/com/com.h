#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include "../../shared/protocol.h"

namespace client {
struct Names {
    const wchar_t* section = protocol::section_name;
    const wchar_t* request = protocol::request_name;
    const wchar_t* response = protocol::response_name;
    const wchar_t* session = protocol::session_name;
};
class Channel {
public:
    explicit Channel(Names names = {}) : names_(names) {}
    ~Channel() { close(); }
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    bool open();
    void close();
    bool exchange(protocol::Request request, protocol::Response& response);
    const std::wstring& error() const { return error_; }
    uint64_t epoch() const { return epoch_; }
private:
    Names names_;
    bool fail(const wchar_t* operation, DWORD code);
    HANDLE section_ = nullptr;
    HANDLE request_ = nullptr;
    HANDLE response_ = nullptr;
    HANDLE session_ = nullptr;
    bool session_owned_ = false;
    protocol::SharedData* shared_ = nullptr;
    uint64_t epoch_ = 0;
    uint64_t id_ = 0;
    std::wstring error_;
};
std::wstring error_text(DWORD code);
}
