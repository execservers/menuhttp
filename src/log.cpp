#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <string>

namespace {
std::mutex g_logMutex;

bool EqualsIgnoreCase(const std::wstring& value, const wchar_t* literal) {
    std::wstring other(literal);
    if (value.size() != other.size()) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (std::towlower(value[i]) != std::towlower(other[i])) return false;
    }
    return true;
}

std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring BaseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return path.substr(slash == std::wstring::npos ? 0 : slash + 1);
}

std::wstring LogPath() {
    wchar_t executable[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, 32768);
    if (size == 0 || size >= 32768) return {};
    std::wstring directory = Parent(std::wstring(executable, size));
    for (int i = 0; i < 2; ++i) {
        const auto name = BaseName(directory);
        if (EqualsIgnoreCase(name, L"bin") || EqualsIgnoreCase(name, L"win64") ||
            EqualsIgnoreCase(name, L"win32")) {
            directory = Parent(directory);
        } else {
            break;
        }
    }
    return directory.empty() ? std::wstring{} : directory + L"\\menuhttp.log";
}

HANDLE OpenLog() {
    const std::wstring path = LogPath();
    return path.empty() ? INVALID_HANDLE_VALUE :
        CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void WriteAll(HANDLE file, const char* data, size_t length) {
    while (length) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(length, 65536));
        DWORD written = 0;
        if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0) return;
        data += written;
        length -= written;
    }
}

std::string Prefix() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char buffer[160]{};
    snprintf(buffer, sizeof(buffer),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu tid=%lu] ",
        static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()));
    return buffer;
}

// Emit data in bounded chunks; do not allocate a second, potentially huge,
// response body. No field contents are masked or discarded.
void WriteEscaped(HANDLE file, const std::string& data) {
    constexpr char hex[] = "0123456789ABCDEF";
    char buffer[8192];
    size_t used = 0;
    auto append = [&](char c) {
        if (used == sizeof(buffer)) { WriteAll(file, buffer, used); used = 0; }
        buffer[used++] = c;
    };
    for (unsigned char c : data) {
        if (c == '\\') { append('\\'); append('\\'); }
        else if (c == '\n') { append('\\'); append('n'); }
        else if (c == '\r') { append('\\'); append('r'); }
        else if (c == '\t') { append('\\'); append('t'); }
        else if (c < 32 || c == 127) {
            append('\\'); append('x'); append(hex[c >> 4]); append(hex[c & 0x0f]);
        } else append(static_cast<char>(c));
    }
    if (used) WriteAll(file, buffer, used);
}
} // namespace

void MenuHTTP_Log(const char* format, ...) {
    char message[4096]{};
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    const std::string line = Prefix() + message + "\r\n";
    std::lock_guard<std::mutex> guard(g_logMutex);
    const HANDLE file = OpenLog();
    if (file != INVALID_HANDLE_VALUE) {
        WriteAll(file, line.data(), line.size());
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    OutputDebugStringA(line.c_str());
}

void MenuHTTP_LogData(std::uint64_t id, const char* label, const std::string& data) {
    const std::string header = Prefix() + "HTTP[" + std::to_string(id) + "] " + label +
        " bytes=" + std::to_string(data.size()) + " value=";
    std::lock_guard<std::mutex> guard(g_logMutex);
    const HANDLE file = OpenLog();
    if (file == INVALID_HANDLE_VALUE) return;
    WriteAll(file, header.data(), header.size());
    WriteEscaped(file, data);
    WriteAll(file, "\r\n", 2);
    FlushFileBuffers(file);
    CloseHandle(file);
}
