#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <CommCtrl.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "com/com.h"

namespace {
constexpr UINT update_message = WM_APP + 1;
constexpr int filter_id = 100;
struct Process { DWORD id; DWORD parent; DWORD threads; std::wstring image; };
struct Update {
    std::vector<Process> processes;
    std::vector<protocol::Event> events;
    std::wstring status;
    std::wstring detail;
};
HWND process_list, event_list, status_label, detail_label, filter_edit;
std::atomic<bool> stopping{false};
std::atomic<DWORD> selected_pid{0};
std::thread worker_thread;
std::vector<Process> processes;
bool demo;
bool smoke_test;
HFONT font;

std::wstring time_text(uint64_t value) {
    if (!value) return L"-";
    ULARGE_INTEGER time;
    time.QuadPart = value;
    FILETIME utc{ time.LowPart, time.HighPart }, local;
    SYSTEMTIME system;
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &system)) return L"Invalid time";
    wchar_t buffer[40];
    swprintf_s(buffer, L"%02u:%02u:%02u.%03u", system.wHour, system.wMinute, system.wSecond, system.wMilliseconds);
    return buffer;
}
std::vector<Process> snapshot() {
    std::vector<Process> result;
    HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Process snapshot failed");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(handle, &entry)) {
        do { result.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.cntThreads, entry.szExeFile}); }
        while (Process32NextW(handle, &entry));
    }
    const DWORD error = GetLastError();
    CloseHandle(handle);
    if (error != ERROR_NO_MORE_FILES) throw std::runtime_error("Process enumeration failed");
    std::sort(result.begin(), result.end(), [](const Process& a, const Process& b) { return a.id < b.id; });
    return result;
}
protocol::Request request(protocol::Command command) {
    protocol::Request value{};
    value.version = protocol::version;
    value.size = sizeof(value);
    value.command = command;
    return value;
}
void run(HWND window) {
    client::Channel channel; // this thread owns both the channel and its session mutex
    bool connected = false;
    uint64_t cursor = 0;
    uint64_t lost = 0;
    uint64_t sample_sequence = 0;
    auto next_connect = std::chrono::steady_clock::now();
    while (!stopping.load()) {
        auto update = std::make_unique<Update>();
        try { update->processes = snapshot(); }
        catch (const std::exception&) { update->status = L"Windows process enumeration failed"; }
        if (demo) {
            update->status = L"DEMO: process list is live Windows data; kernel events below are synthetic samples.";
            protocol::Event event{};
            event.sequence = ++sample_sequence;
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER time;
            time.LowPart = now.dwLowDateTime; time.HighPart = now.dwHighDateTime;
            event.time = time.QuadPart;
            event.process_id = 4242;
            event.parent_id = 100;
            event.kind = sample_sequence % 2 ? protocol::Kind::created : protocol::Kind::exited;
            const wchar_t image[] = L"SampleProcess.exe (synthetic)";
            memcpy(event.image, image, sizeof(image));
            update->events.push_back(event);
            update->detail = L"Demo mode: kernel query unavailable. Run the signed driver and launch without --demo for real events.";
        } else {
            if (!connected && std::chrono::steady_clock::now() >= next_connect) {
                connected = channel.open();
                protocol::Response info{};
                if (connected) connected = channel.exchange(request(protocol::Command::info), info);
                if (connected) { cursor = info.latest_sequence; lost = 0; }
                next_connect = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            }
            if (connected) {
                auto read = request(protocol::Command::events);
                read.epoch = channel.epoch(); read.after_sequence = cursor; read.max_events = protocol::batch_capacity;
                protocol::Response response{};
                // Drain up to the ring capacity per refresh so a short burst does not
                // leave us permanently processing just sixteen events per second.
                bool read_ok = true;
                for (uint32_t batch = 0; batch < protocol::ring_capacity / protocol::batch_capacity; ++batch) {
                    read.after_sequence = cursor;
                    if (!channel.exchange(read, response)) { read_ok = false; break; }
                    cursor = response.next_sequence;
                    lost += response.dropped;
                    update->events.insert(update->events.end(), response.events, response.events + response.count);
                    if (cursor == response.latest_sequence) break;
                }
                if (read_ok) {
                    update->status = L"Kernel connected | Shared section + events | Creates: " + std::to_wstring(response.created) +
                        L" | Exits: " + std::to_wstring(response.exited) + L" | Lost since connection: " + std::to_wstring(lost);
                } else { connected = false; channel.close(); }
                const DWORD pid = selected_pid.load();
                if (connected && pid) {
                    auto query = request(protocol::Command::process); query.process_id = pid;
                    protocol::Response response_info{};
                    if (channel.exchange(query, response_info)) {
                        update->detail = L"Kernel PID " + std::to_wstring(pid) + L" | Created: " + time_text(response_info.process_create_time) +
                            L" | " + (response_info.process_exit_status == 0x103 ? std::wstring(L"Running at query time") : std::wstring(L"Exited"));
                    } else {
                        update->detail = L"Selected process: " + channel.error() + L" (it may have exited).";
                        // Operation failure is distinct from a broken transport. Reopen if transport closed.
                        if (!channel.epoch()) connected = false;
                    }
                } else if (connected) update->detail = L"Select a process to query its creation time and exit status through the driver.";
            }
            if (!connected) {
                if (update->status.empty()) update->status = L"Kernel disconnected: " + channel.error() + L" | Retrying every 3 seconds.";
                if (update->detail.empty()) update->detail = L"The Windows process list remains available. Real kernel events require the signed driver.";
            }
        }
        Update* packet = update.release();
        if (!PostMessageW(window, update_message, 0, reinterpret_cast<LPARAM>(packet))) delete packet;
        for (int slice = 0; slice < 10 && !stopping.load(); ++slice) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
void column(HWND list, int index, const wchar_t* text, int width) {
    LVCOLUMNW value{}; value.mask = LVCF_TEXT | LVCF_WIDTH; value.cx = width; value.pszText = const_cast<wchar_t*>(text);
    ListView_InsertColumn(list, index, &value);
}
void cell(HWND list, int row, int col, const std::wstring& text) {
    if (!col) {
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = row; item.pszText = const_cast<wchar_t*>(text.c_str());
        ListView_InsertItem(list, &item);
    } else ListView_SetItemText(list, row, col, const_cast<wchar_t*>(text.c_str()));
}
void display_processes() {
    wchar_t filter[256]; GetWindowTextW(filter_edit, filter, 256);
    std::wstring needle(filter);
    std::transform(needle.begin(), needle.end(), needle.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    SendMessageW(process_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(process_list);
    int row = 0;
    const DWORD selected = selected_pid.load();
    for (const auto& process : processes) {
        std::wstring name = process.image;
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (!needle.empty() && name.find(needle) == std::wstring::npos && std::to_wstring(process.id).find(needle) == std::wstring::npos) continue;
        cell(process_list, row, 0, process.image);
        cell(process_list, row, 1, std::to_wstring(process.id));
        cell(process_list, row, 2, std::to_wstring(process.parent));
        cell(process_list, row, 3, std::to_wstring(process.threads));
        LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = row; item.lParam = process.id;
        ListView_SetItem(process_list, &item);
        if (process.id == selected) ListView_SetItemState(process_list, row, LVIS_SELECTED, LVIS_SELECTED);
        ++row;
    }
    SendMessageW(process_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(process_list, nullptr, TRUE);
}
void layout(HWND window) {
    RECT rect; GetClientRect(window, &rect);
    const int width = rect.right;
    const int height = rect.bottom;
    const int top_height = std::max(100, (height - 150) / 2);
    MoveWindow(filter_edit, 90, 12, std::max(100, width - 110), 26, TRUE);
    MoveWindow(process_list, 12, 50, width - 24, top_height, TRUE);
    MoveWindow(detail_label, 12, top_height + 60, width - 24, 34, TRUE);
    MoveWindow(event_list, 12, top_height + 102, width - 24, std::max(80, height - top_height - 150), TRUE);
    MoveWindow(status_label, 12, height - 36, width - 24, 28, TRUE);
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        CreateWindowW(L"STATIC", L"Filter:", WS_CHILD | WS_VISIBLE, 12, 16, 70, 22, window, nullptr, nullptr, nullptr);
        filter_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            90, 12, 400, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(filter_id)), nullptr, nullptr);
        const DWORD style = WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS;
        process_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Processes", style, 0, 0, 1, 1, window, nullptr, nullptr, nullptr);
        event_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Kernel events", style, 0, 0, 1, 1, window, nullptr, nullptr, nullptr);
        detail_label = CreateWindowW(L"STATIC", L"Starting monitor...", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, nullptr, nullptr, nullptr);
        status_label = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, nullptr, nullptr, nullptr);
        if (!filter_edit || !process_list || !event_list || !detail_label || !status_label) return -1;
        for (HWND child : {filter_edit, process_list, event_list, detail_label, status_label}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        for (HWND list : {process_list, event_list}) ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        column(process_list, 0, L"Process (Windows snapshot)", 450); column(process_list, 1, L"PID", 100);
        column(process_list, 2, L"Parent PID", 100); column(process_list, 3, L"Threads", 100);
        column(event_list, 0, L"Kernel event", 130); column(event_list, 1, L"Local time", 140);
        column(event_list, 2, L"PID", 80); column(event_list, 3, L"Parent PID", 90);
        column(event_list, 4, L"Image (creation only; may be truncated)", 460); column(event_list, 5, L"Sequence", 100);
        layout(window);
        try { worker_thread = std::thread(run, window); }
        catch (const std::exception&) { return -1; }
        if (smoke_test) SetTimer(window, 1, 1600, nullptr);
        return 0;
    }
    case WM_SIZE: if (process_list) layout(window); return 0;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize = {800, 550}; return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == filter_id && HIWORD(wparam) == EN_CHANGE) display_processes();
        return 0;
    case WM_NOTIFY: {
        auto header = reinterpret_cast<NMHDR*>(lparam);
        if (header->hwndFrom == process_list && header->code == LVN_ITEMCHANGED) {
            auto change = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED)) selected_pid.store(static_cast<DWORD>(change->lParam));
        }
        return 0;
    }
    case update_message: {
        std::unique_ptr<Update> update(reinterpret_cast<Update*>(lparam));
        processes = std::move(update->processes); display_processes();
        SetWindowTextW(status_label, update->status.c_str()); SetWindowTextW(detail_label, update->detail.c_str());
        for (const auto& event : update->events) {
            if (ListView_GetItemCount(event_list) >= 1000) ListView_DeleteItem(event_list, 0);
            const int row = ListView_GetItemCount(event_list);
            cell(event_list, row, 0, event.kind == protocol::Kind::created ? L"Created" : L"Exited");
            cell(event_list, row, 1, time_text(event.time)); cell(event_list, row, 2, std::to_wstring(event.process_id));
            cell(event_list, row, 3, event.kind == protocol::Kind::created ? std::to_wstring(event.parent_id) : L"-");
            // Treat the external UTF-16 array as bounded even if the peer is malformed.
            size_t length = 0; while (length < protocol::image_chars && event.image[length]) ++length;
            std::wstring image; image.reserve(length);
            for (size_t i = 0; i < length; ++i) image.push_back(static_cast<wchar_t>(event.image[i]));
            if (event.image_truncated) image += L"...";
            cell(event_list, row, 4, image); cell(event_list, row, 5, std::to_wstring(event.sequence));
        }
        return 0;
    }
    case WM_TIMER: if (smoke_test) PostMessageW(window, WM_CLOSE, 0, 0); return 0;
    case WM_CLOSE: {
        stopping.store(true);
        if (worker_thread.joinable()) worker_thread.join();
        MSG pending;
        while (PeekMessageW(&pending, window, update_message, update_message, PM_REMOVE)) delete reinterpret_cast<Update*>(pending.lParam);
        DestroyWindow(window); return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--demo") == 0) demo = true;
        else if (wcscmp(argv[i], L"--smoke-test") == 0) smoke_test = true;
        else { LocalFree(argv); return 2; }
    }
    LocalFree(argv);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) return 1;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.hInstance = instance; window_class.lpfnWndProc = procedure;
    window_class.lpszClassName = L"KpmMonitor"; window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&window_class)) return 1;
    HWND window = CreateWindowExW(0, window_class.lpszClassName, demo ? L"Kernel Process Monitor - DEMO" : L"Kernel Process Monitor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 780, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG message;
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (worker_thread.joinable()) { stopping.store(true); worker_thread.join(); }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
