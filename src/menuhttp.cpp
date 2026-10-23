#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>

#include <GarrysMod/Lua/Interface.h>

#include "log.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using GarrysMod::Lua::ILuaBase;
namespace Type = GarrysMod::Lua::Type;

namespace {
constexpr int kMissingRef = -1;
constexpr size_t kMaxJobs = 128;
constexpr size_t kMaxBody = 32u * 1024u * 1024u;
constexpr int kWorkers = 4;
// Steam HTTP default observed in Garry's Mod; callers may override via headers.
constexpr char kGmodUserAgent[] = "Valve/Steam HTTP Client 1.0 (4000)";
constexpr const char* kHookId = "menuhttp.winhttp.callbacks";
std::atomic<std::uint64_t> g_nextRequestId{1};

struct Request {
    std::string url;
    std::string method = "GET";
    std::map<std::string, std::string> parameters;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string contentType = "text/plain; charset=utf-8";
    int timeout = 60;
    bool hasBody = false;
};

struct Response {
    bool ok = false;
    unsigned long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;
#if MENUHTTP_WITH_LOGGER
    std::string rawHeaders;
#endif
};

struct Job {
    std::uint64_t id = 0;
    ULONGLONG submittedAt = 0;
    std::string caller;
    std::string api;
    Request request;
    int successRef = kMissingRef;
    int failureRef = kMissingRef;
    bool convenience = false;
};

struct Completion {
    std::shared_ptr<Job> job;
    Response response;
};

using InternetHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
InternetHandle Handle(HINTERNET h) { return InternetHandle(h, &WinHttpCloseHandle); }

std::wstring ToWide(const std::string& text) {
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(static_cast<size_t>(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), result.data(), n)) return {};
    return result;
}

std::string ToUTF8(const std::wstring& text) {
    if (text.empty() || text.size() > static_cast<size_t>(INT_MAX)) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string result(static_cast<size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                             result.data(), n, nullptr, nullptr)) return {};
    return result;
}

std::string WinError(DWORD error) {
    return "WinHTTP error " + std::to_string(error);
}

bool IsSafeHeader(const std::string& name, const std::string& value) {
    if (name.empty() || name.size() > 1024 || value.size() > 16384) return false;
    const auto token = [](unsigned char c) {
        return std::isalnum(c) || c == '!' || c == '#' || c == '$' || c == '%' ||
               c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' ||
               c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
    };
    for (unsigned char c : name) if (!token(c)) return false;
    for (unsigned char c : value) if (c == '\r' || c == '\n' || c == 0) return false;
    return true;
}

std::string Encode(const std::string& source) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(source.size());
    for (unsigned char c : source) {
        if ((std::isalnum(c) && c < 128) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool HasHeader(const std::map<std::string, std::string>& headers, const char* target) {
    for (const auto& [name, ignored] : headers) {
        (void)ignored;
        size_t i = 0;
        while (i < name.size() && target[i] &&
               std::tolower(static_cast<unsigned char>(name[i])) ==
               std::tolower(static_cast<unsigned char>(target[i]))) ++i;
        if (i == name.size() && !target[i]) return true;
    }
    return false;
}

std::string FormData(const std::map<std::string, std::string>& values) {
    std::string out;
    for (const auto& [key, value] : values) {
        if (!out.empty()) out += '&';
        out += Encode(key);
        out += '=';
        out += Encode(value);
    }
    return out;
}

#if MENUHTTP_WITH_LOGGER
std::string RawHeaders(HINTERNET request, DWORD queryFlags) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, queryFlags, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return {};
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, queryFlags, WINHTTP_HEADER_NAME_BY_INDEX,
                              buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
    buffer.resize(wcsnlen_s(buffer.c_str(), buffer.size()));
    return ToUTF8(buffer);
}

#endif

void ReadHeaders(HINTERNET request, std::map<std::string, std::string>& result) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return;
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(),
                             &bytes, WINHTTP_NO_HEADER_INDEX)) return;
    buffer.resize(wcsnlen_s(buffer.c_str(), buffer.size()));
    size_t start = buffer.find(L"\r\n"); // skip HTTP status line
    if (start == std::wstring::npos) return;
    start += 2;
    while (start < buffer.size()) {
        const size_t end = buffer.find(L"\r\n", start);
        if (end == std::wstring::npos || end == start) break;
        const size_t split = buffer.find(L':', start);
        if (split != std::wstring::npos && split < end) {
            size_t begin = split + 1;
            while (begin < end && (buffer[begin] == L' ' || buffer[begin] == L'\t')) ++begin;
            std::string key = ToUTF8(buffer.substr(start, split - start));
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            result[std::move(key)] = ToUTF8(buffer.substr(begin, end - begin));
        }
        start = end + 2;
    }
}

Response Execute(const Job& job) {
    const Request& input = job.request;
    Response output;
    std::string url = input.url;
    std::string body = input.body;
    auto headers = input.headers;
    if (!input.hasBody) {
        const std::string params = FormData(input.parameters);
        if ((input.method == "GET" || input.method == "HEAD") && !input.parameters.empty()) {
            url += (url.find('?') == std::string::npos ? "?" : "&");
            url += params;
        } else if (input.method == "POST" && !input.parameters.empty()) {
            body = params;
            if (!HasHeader(headers, "Content-Type"))
                headers.emplace("Content-Type", "application/x-www-form-urlencoded");
        }
    }
    if (input.hasBody && !HasHeader(headers, "Content-Type"))
        headers.emplace("Content-Type", input.contentType);

    // Never send the custom DLL identifier as User-Agent. Respect any caller-
    // supplied User-Agent (case-insensitive), otherwise match the commonly
    // observed Steam HTTP/GMod default. WinHTTP itself must not choose one.
    const bool customUserAgent = HasHeader(headers, "User-Agent");
    if (!customUserAgent) headers.emplace("User-Agent", kGmodUserAgent);
    MenuHTTP_Log("HTTP[%llu] User-Agent source=%s",
        static_cast<unsigned long long>(job.id),
        customUserAgent ? "Lua headers" : "GMod/Steam fallback");
    MenuHTTP_LogData(job.id, "request.url.effective", url);
#if MENUHTTP_WITH_LOGGER
    for (const auto& [name, value] : headers)
        MenuHTTP_LogData(job.id, "request.header.effective", name + ": " + value);
#endif
    MenuHTTP_LogData(job.id, "request.body.effective", body);
    if (body.size() > kMaxBody) { output.error = "request body too large"; return output; }

    const auto urlWide = ToWide(url);
    if (urlWide.empty()) { output.error = "invalid url"; return output; }
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(urlWide.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        !parts.dwHostNameLength) {
        output.error = "invalid url";
        return output;
    }
    const std::wstring hostname(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path = parts.dwUrlPathLength
        ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    const std::wstring method = ToWide(input.method);
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    // No WinHTTP session User-Agent: the header is set explicitly below,
    // so the effective request header is always the caller's value or the
    // Steam/GMod fallback, never an identifier for this DLL.
    auto session = Handle(WinHttpOpen(nullptr,
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { output.error = WinError(GetLastError()); return output; }
    auto connection = Handle(WinHttpConnect(session.get(), hostname.c_str(), parts.nPort, 0));
    if (!connection) { output.error = WinError(GetLastError()); return output; }
    auto request = Handle(WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(),
                                              nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) { output.error = WinError(GetLastError()); return output; }
    const int timeoutMs = input.timeout * 1000;
    if (!WinHttpSetTimeouts(request.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs)) {
        output.error = WinError(GetLastError()); return output;
    }
    for (const auto& [key, value] : headers) {
        if (!IsSafeHeader(key, value)) { output.error = "invalid request header"; return output; }
        const std::wstring header = ToWide(key + ": " + value);
        if (header.empty() || !WinHttpAddRequestHeaders(request.get(), header.c_str(),
                static_cast<DWORD>(header.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            output.error = WinError(GetLastError()); return output;
        }
    }
    void* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : static_cast<void*>(body.data());
    const DWORD length = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            data, length, length, 0)) {
        output.error = WinError(GetLastError()); return output;
    }
    // WinHTTP may add its own headers (Host, Content-Length, etc.). Record
    // the transport's request headers as well as the caller-supplied table.
    MenuHTTP_LogData(job.id, "request.headers.wire",
        RawHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF | WINHTTP_QUERY_FLAG_REQUEST_HEADERS));
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        output.error = WinError(GetLastError()); return output;
    }
    DWORD statusSize = sizeof(output.status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &output.status,
                             &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        output.error = WinError(GetLastError()); return output;
    }
#if MENUHTTP_WITH_LOGGER
    output.rawHeaders = RawHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF);
#endif
    ReadHeaders(request.get(), output.headers);
    std::array<char, 65536> chunk{};
    while (true) {
        DWORD n = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &n)) {
            output.error = WinError(GetLastError()); return output;
        }
        if (n == 0) break;
        if (output.body.size() + n > kMaxBody) {
            output.error = "response body too large";
            return output;
        }
        output.body.append(chunk.data(), n);
    }
    output.ok = true; // HTTP status >= 400 is still a successful transport.
    return output;
}

class Engine {
public:
    void Start() {
        for (int i = 0; i < kWorkers; ++i) workers_.emplace_back([this] { Work(); });
    }
    bool Submit(std::shared_ptr<Job> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outstanding_ >= kMaxJobs) return false;
            ++outstanding_;
            queued_.push_back(std::move(job));
        }
        ready_.notify_one();
        return true;
    }
    std::deque<Completion> Take(size_t max) {
        std::deque<Completion> batch;
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < max && !done_.empty(); ++i) {
            batch.push_back(std::move(done_.front()));
            done_.pop_front();
            --outstanding_;
        }
        return batch;
    }
    static void Free(ILuaBase* lua, Job& job) {
        if (job.successRef != kMissingRef) lua->ReferenceFree(job.successRef);
        if (job.failureRef != kMissingRef) lua->ReferenceFree(job.failureRef);
        job.successRef = job.failureRef = kMissingRef;
    }
private:
    void Work() {
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return !queued_.empty(); });
                job = std::move(queued_.front());
                queued_.pop_front();
            }
            Response response;
            try { response = Execute(*job); }
            catch (...) { response.error = "internal HTTP worker error"; }
#if MENUHTTP_WITH_LOGGER
            const auto elapsed = GetTickCount64() - job->submittedAt;
            MenuHTTP_Log("HTTP[%llu] RESPONSE api=%s code=%lu success=%d elapsed_ms=%llu source=%s",
                         static_cast<unsigned long long>(job->id), job->api.c_str(),
                         response.status, response.ok ? 1 : 0,
                         static_cast<unsigned long long>(elapsed), job->caller.c_str());
            MenuHTTP_LogData(job->id, "response.headers.raw", response.rawHeaders);
            for (const auto& [key, value] : response.headers) {
                MenuHTTP_LogData(job->id, "response.header.name", key);
                MenuHTTP_LogData(job->id, "response.header.value", value);
            }
            MenuHTTP_LogData(job->id, "response.body", response.body);
            if (!response.ok) MenuHTTP_LogData(job->id, "response.error", response.error);
#endif
            {
                std::lock_guard<std::mutex> lock(mutex_);
                done_.push_back({std::move(job), std::move(response)});
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::shared_ptr<Job>> queued_;
    std::deque<Completion> done_;
    std::vector<std::thread> workers_;
    size_t outstanding_ = 0;
};

// Deliberately process-lifetime: std::thread destructors must not run during
// DLL detach (Windows loader lock). The OS reclaims the worker resources on exit.
Engine* g_engine = nullptr;

std::string StringAt(ILuaBase* lua, int index) {
    unsigned int length = 0;
    const char* data = lua->GetString(index, &length);
    return data ? std::string(data, length) : std::string{};
}

bool TableString(ILuaBase* lua, int index, const char* key, std::string& out) {
    lua->GetField(index, key);
    bool found = lua->GetType(-1) == Type::String;
    if (found) out = StringAt(lua, -1);
    lua->Pop();
    return found;
}

// GMod parameters commonly contain numbers and booleans as values.
bool ScalarString(ILuaBase* lua, int index, std::string& out) {
    const int type = lua->GetType(index);
    if (type == Type::String) { out = StringAt(lua, index); return true; }
    if (type == Type::Bool) { out = lua->GetBool(index) ? "true" : "false"; return true; }
    if (type == Type::Number) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(15) << lua->GetNumber(index);
        out = stream.str();
        return true;
    }
    return false;
}

bool ReadPairs(ILuaBase* lua, int index, std::map<std::string, std::string>& out) {
    if (lua->GetType(index) != Type::Table) return false;
    lua->PushNil();
    while (lua->Next(index)) {
        std::string key, value;
        if (lua->GetType(-2) != Type::String || !ScalarString(lua, -1, value)) {
            lua->Pop(2); // value, key
            return false;
        }
        // Restrict total entries to prevent unbounded memory use.
        if (out.size() >= 256) { lua->Pop(2); return false; }
        key = StringAt(lua, -2);
        out[std::move(key)] = std::move(value);
        lua->Pop(); // value; keep key for Next
    }
    return true;
}

bool ReadOptionalPairs(ILuaBase* lua, int index, const char* field,
                       std::map<std::string, std::string>& out) {
    lua->GetField(index, field);
    const int type = lua->GetType(-1);
    bool valid = type == Type::Nil || (type == Type::Table && ReadPairs(lua, lua->Top(), out));
    lua->Pop();
    return valid;
}

bool ReadRequest(ILuaBase* lua, int tableIndex, Request& request) {
    if (lua->GetType(tableIndex) != Type::Table ||
        !TableString(lua, tableIndex, "url", request.url) || request.url.empty()) return false;
    TableString(lua, tableIndex, "method", request.method);
    std::transform(request.method.begin(), request.method.end(), request.method.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    const bool supported = request.method == "GET" || request.method == "POST" ||
       request.method == "PUT" || request.method == "PATCH" || request.method == "DELETE" ||
       request.method == "HEAD" || request.method == "OPTIONS";
    if (!supported || !ReadOptionalPairs(lua, tableIndex, "parameters", request.parameters) ||
        !ReadOptionalPairs(lua, tableIndex, "headers", request.headers)) return false;
    request.hasBody = TableString(lua, tableIndex, "body", request.body);
    TableString(lua, tableIndex, "type", request.contentType);
    lua->GetField(tableIndex, "timeout");
    if (lua->GetType(-1) == Type::Number) {
        double n = lua->GetNumber(-1);
        if (n >= 1 && n <= 300) request.timeout = static_cast<int>(n);
        else { lua->Pop(); return false; }
    }
    lua->Pop();
    return true;
}

int ReferenceFunction(ILuaBase* lua, int index) {
    if (lua->GetType(index) != Type::Function) return kMissingRef;
    lua->Push(index);
    return lua->ReferenceCreate();
}

// Read the Lua call site through debug.getinfo rather than relying on the
// private lua_Debug layout. The first Lua frame above our native function is
// normally level 2; skip intervening C frames and report Lua file:line.
#if MENUHTTP_WITH_LOGGER
std::string Caller(ILuaBase* lua) {
    const int oldTop = lua->Top();
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->GetField(-1, "debug");
    if (lua->GetType(-1) != Type::Table) {
        lua->Pop(lua->Top() - oldTop);
        return "unknown (debug unavailable)";
    }
    lua->GetField(-1, "getinfo");
    if (lua->GetType(-1) != Type::Function) {
        lua->Pop(lua->Top() - oldTop);
        return "unknown (debug.getinfo unavailable)";
    }
    std::string origin = "unknown (no Lua frame)";
    for (int level = 2; level <= 12; ++level) {
        lua->Push(-1);  // retained debug.getinfo function
        lua->PushNumber(level);
        lua->PushString("Sln");
        if (lua->PCall(2, 1, 0) != 0) { lua->Pop(); break; }
        if (lua->GetType(-1) != Type::Table) { lua->Pop(); break; }
        lua->GetField(-1, "short_src");
        const std::string source = StringAt(lua, -1);
        lua->Pop();
        lua->GetField(-1, "currentline");
        const int line = lua->GetType(-1) == Type::Number
            ? static_cast<int>(lua->GetNumber(-1)) : -1;
        lua->Pop();
        lua->GetField(-1, "name");
        const std::string name = StringAt(lua, -1);
        lua->Pop();
        lua->GetField(-1, "what");
        const std::string what = StringAt(lua, -1);
        lua->Pop();
        lua->Pop(); // debug info table
        if (what == "C") continue;
        if (!source.empty()) {
            origin = source + ":" + std::to_string(line);
            if (!name.empty()) origin += " (" + name + ")";
            break;
        }
    }
    lua->Pop(lua->Top() - oldTop);
    return origin;
}

void LogRequest(const Job& job) {
    MenuHTTP_Log("HTTP[%llu] CALL api=%s source=%s method=%s timeout=%d",
                 static_cast<unsigned long long>(job.id), job.api.c_str(),
                 job.caller.c_str(), job.request.method.c_str(), job.request.timeout);
    MenuHTTP_LogData(job.id, "request.source", job.caller);
    MenuHTTP_LogData(job.id, "request.url", job.request.url);
    for (const auto& [name, value] : job.request.headers)
        MenuHTTP_LogData(job.id, "request.header", name + ": " + value);
    for (const auto& [name, value] : job.request.parameters)
        MenuHTTP_LogData(job.id, "request.parameter", name + "=" + value);
    MenuHTTP_LogData(job.id, "request.content_type", job.request.contentType);
    if (job.request.hasBody)
        MenuHTTP_LogData(job.id, "request.body", job.request.body);
}

#else
std::string Caller(ILuaBase*) { return {}; }
void LogRequest(const Job&) {}
#endif

void ReportLuaError(ILuaBase* lua, std::uint64_t id) {
    const std::string error = StringAt(lua, -1);
    MenuHTTP_LogData(id, "lua.callback.error", error);
    lua->Pop();
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->GetField(-1, "ErrorNoHalt");
    if (lua->GetType(-1) == Type::Function) {
        lua->PushString(("[menuhttp] callback: " + error + "\n").c_str());
        if (lua->PCall(1, 0, 0) != 0) lua->Pop();
    } else {
        lua->Pop();
    }
    lua->Pop();
}

void InvokeFailure(ILuaBase* lua, int ref, const std::string& error, std::uint64_t id) {
    if (ref == kMissingRef) return;
    lua->ReferencePush(ref);
    lua->PushString(error.c_str(), static_cast<unsigned int>(error.size()));
    if (lua->PCall(1, 0, 0) != 0) ReportLuaError(lua, id);
}

void PushHeaders(ILuaBase* lua, const std::map<std::string, std::string>& headers) {
    lua->CreateTable();
    for (const auto& [key, value] : headers) {
        lua->PushString(value.c_str(), static_cast<unsigned int>(value.size()));
        lua->SetField(-2, key.c_str());
    }
}

void InvokeSuccess(ILuaBase* lua, int ref, const Completion& done) {
    if (ref == kMissingRef) return;
    lua->ReferencePush(ref);
    const auto& r = done.response;
    if (done.job->convenience) {
        lua->PushString(r.body.c_str(), static_cast<unsigned int>(r.body.size()));
        lua->PushNumber(static_cast<double>(r.body.size()));
        PushHeaders(lua, r.headers);
        lua->PushNumber(r.status);
    } else {
        lua->PushNumber(r.status);
        lua->PushString(r.body.c_str(), static_cast<unsigned int>(r.body.size()));
        PushHeaders(lua, r.headers);
    }
    if (lua->PCall(done.job->convenience ? 4 : 3, 0, 0) != 0) ReportLuaError(lua, done.job->id);
}

void Deliver(ILuaBase* lua) {
    if (!g_engine) return;
    auto results = g_engine->Take(32);
    for (auto& result : results) {
        MenuHTTP_Log("HTTP[%llu] CALLBACK type=%s defined=%d",
                     static_cast<unsigned long long>(result.job->id),
                     result.response.ok ? "success" : "failed",
                     (result.response.ok ? result.job->successRef : result.job->failureRef)
                         != kMissingRef ? 1 : 0);
        if (result.response.ok) InvokeSuccess(lua, result.job->successRef, result);
        else InvokeFailure(lua, result.job->failureRef, result.response.error,
                           result.job->id);
        Engine::Free(lua, *result.job);
    }
}

bool Enqueue(ILuaBase* lua, Request&& request,
             int successIndex, int failureIndex, bool convenience,
             std::uint64_t id, std::string caller, const char* api) {
    auto job = std::make_shared<Job>();
    job->id = id;
    job->submittedAt = GetTickCount64();
    job->caller = std::move(caller);
    job->api = api;
    job->request = std::move(request);
    job->convenience = convenience;
    LogRequest(*job);
    if (!g_engine || job->request.url.empty() || job->request.url.size() > 16384 ||
        job->request.body.size() > kMaxBody) {
        MenuHTTP_Log("HTTP[%llu] REJECT reason=invalid request / limits",
                     static_cast<unsigned long long>(id));
        return false;
    }
    job->successRef = ReferenceFunction(lua, successIndex);
    job->failureRef = ReferenceFunction(lua, failureIndex);
    // The worker may finish before Submit returns. CALL records are flushed
    // before queue insertion; any later RESPONSE record shares the same ID.
    if (g_engine->Submit(job)) return true;
    Engine::Free(lua, *job);
    MenuHTTP_Log("HTTP[%llu] REJECT reason=request queue full",
                 static_cast<unsigned long long>(id));
    return false;
}

#if MENUHTTP_WITH_LOGGER
void LogRejectedCall(std::uint64_t id, const char* api, const std::string& source,
                     const char* reason) {
    MenuHTTP_Log("HTTP[%llu] REJECT api=%s source=%s reason=%s",
                 static_cast<unsigned long long>(id), api, source.c_str(), reason);
}
#else
void LogRejectedCall(std::uint64_t, const char*, const std::string&, const char*) {}
#endif

// The native HTTP API. Unlike http.Fetch/Post, HTTP returns a boolean.
LUA_FUNCTION_STATIC(NativeHTTP) {
    const auto id = g_nextRequestId.fetch_add(1, std::memory_order_relaxed);
    const std::string caller = Caller(LUA);
    Request request;
    if (!ReadRequest(LUA, 1, request)) {
        LogRejectedCall(id, "HTTP/http.Request", caller, "invalid HTTPRequest table");
        LUA->PushBool(false);
        return 1;
    }
    LUA->GetField(1, "success");
    LUA->GetField(1, "failed");
    const bool queued = Enqueue(LUA, std::move(request), -2, -1, false,
                                id, caller, "HTTP/http.Request");
    LUA->Pop(2);
    LUA->PushBool(queued);
    return 1;
}

LUA_FUNCTION_STATIC(NativeFetch) {
    const auto id = g_nextRequestId.fetch_add(1, std::memory_order_relaxed);
    const std::string caller = Caller(LUA);
    if (LUA->GetType(1) != Type::String) {
        LogRejectedCall(id, "http.Fetch/http2.Fetch", caller, "URL must be a string");
        return 0;
    }
    Request request;
    request.url = StringAt(LUA, 1);
    if (LUA->GetType(4) == Type::Table && !ReadPairs(LUA, 4, request.headers)) {
        LogRejectedCall(id, "http.Fetch/http2.Fetch", caller, "invalid headers");
        return 0;
    }
    if (!Enqueue(LUA, std::move(request), 2, 3, true,
                 id, caller, "http.Fetch/http2.Fetch")) {
        const int failed = ReferenceFunction(LUA, 3);
        InvokeFailure(LUA, failed, "HTTP failed", id);
        if (failed != kMissingRef) LUA->ReferenceFree(failed);
    }
    return 0;
}

LUA_FUNCTION_STATIC(NativePost) {
    const auto id = g_nextRequestId.fetch_add(1, std::memory_order_relaxed);
    const std::string caller = Caller(LUA);
    if (LUA->GetType(1) != Type::String || LUA->GetType(2) != Type::Table) {
        LogRejectedCall(id, "http.Post/http2.Post", caller, "invalid URL or parameters");
        return 0;
    }
    Request request;
    request.url = StringAt(LUA, 1);
    request.method = "POST";
    if (!ReadPairs(LUA, 2, request.parameters)) {
        LogRejectedCall(id, "http.Post/http2.Post", caller, "invalid parameters");
        return 0;
    }
    if (LUA->GetType(5) == Type::Table && !ReadPairs(LUA, 5, request.headers)) {
        LogRejectedCall(id, "http.Post/http2.Post", caller, "invalid headers");
        return 0;
    }
    if (!Enqueue(LUA, std::move(request), 3, 4, true,
                 id, caller, "http.Post/http2.Post")) {
        const int failed = ReferenceFunction(LUA, 4);
        InvokeFailure(LUA, failed, "HTTP failed", id);
        if (failed != kMissingRef) LUA->ReferenceFree(failed);
    }
    return 0;
}

LUA_FUNCTION_STATIC(Poll) { Deliver(LUA); return 0; }

bool HookCall(ILuaBase* lua, const char* method) {
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->GetField(-1, "hook");
    if (lua->GetType(-1) != Type::Table) { lua->Pop(2); return false; }
    lua->GetField(-1, method);
    if (lua->GetType(-1) != Type::Function) { lua->Pop(3); return false; }
    lua->PushString("Think");
    lua->PushString(kHookId);
    int args = 2;
    if (method[0] == 'A') { lua->PushCFunction(Poll); ++args; }
    const int error = lua->PCall(args, 0, 0);
    if (error != 0) {
        const std::string reason = StringAt(lua, -1);
        MenuHTTP_Log("MENU hook.%s failed: %s", method, reason.c_str());
        lua->Pop();
    }
    lua->Pop(2); // hook table + globals
    return error == 0;
}

// Registers a C function on a Lua table. All Lua access is on the MENU thread.
void SetFunction(ILuaBase* lua, int tableIndex, const char* field,
                 GarrysMod::Lua::CFunc function) {
    // SetField consumes the pushed value. A relative index (e.g. -1)
    // would refer to the *function*, not the original table, after PushCFunction.
    const int absoluteIndex = tableIndex < 0 ? lua->Top() + tableIndex + 1 : tableIndex;
    lua->PushCFunction(function);
    lua->SetField(absoluteIndex, field);
}

} // namespace

// Called by inject.cpp after the MENU Lua interface becomes available.
// No Lua require(), gmod13_open or script bootstrap is used or exported.
bool MenuHTTP_Install(ILuaBase* lua) {
    if (!lua || g_engine) return g_engine != nullptr;

    // Only install in MENU. Do not alter the client or server Lua realms.
    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    lua->GetField(-1, "MENU_DLL");
    const bool isMenu = lua->GetType(-1) == Type::Bool && lua->GetBool(-1);
    lua->Pop(2);
    if (!isMenu) {
        static bool loggedNotReady = false;
        if (!loggedNotReady) {
            loggedNotReady = true;
            MenuHTTP_Log("waiting: MENU_DLL global is not true yet");
        }
        return false;
    }

    // Ensure callback delivery is available before installing any global API.
    // No request workers are started if the menu Think hook is unavailable.
    if (!HookCall(lua, "Add")) {
        static bool loggedMissingHook = false;
        if (!loggedMissingHook) {
            loggedMissingHook = true;
            MenuHTTP_Log("waiting: hook.Add unavailable in MENU state");
        }
        return false;
    }

    // Intentionally never deleted: see comment at g_engine declaration.
    // The hook calls Poll on the game's MENU Lua thread.
    g_engine = new Engine();
    g_engine->Start();

    lua->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB); // _G
    SetFunction(lua, -1, "HTTP", NativeHTTP);       // standard global HTTP({...})

    lua->GetField(-1, "http");
    if (lua->GetType(-1) != Type::Table) {
        lua->Pop();
        lua->CreateTable();
        lua->Push(-1);
        lua->SetField(-3, "http");                // _G.http = {}
    }

    // Existing table entries other than our API are left unchanged.
    SetFunction(lua, -1, "Fetch", NativeFetch);
    SetFunction(lua, -1, "Post", NativePost);
    SetFunction(lua, -1, "Request", NativeHTTP);  // table-scoped alias for HTTP

    // _G.http2 points to the exact same Lua table as _G.http.
    // Do not create a new table or copy its functions.
    lua->Push(-1);                                  // duplicate http table reference
    lua->SetField(-3, "http2");                    // _G.http2 = _G.http
    lua->Pop(2);                                   // http + _G
    MenuHTTP_Log("registered _G.HTTP, _G.http and _G.http2 (same table)");
    return true;
}
