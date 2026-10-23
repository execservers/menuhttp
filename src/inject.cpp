// LoadLibraryW bootstrap for both x86 and x64 Garry's Mod.
// The DLL registers Lua globals without require; hot unloading is unsupported.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <GarrysMod/Lua/LuaInterface.h>
#include <GarrysMod/Lua/LuaShared.h>

#include "log.h"

#include <atomic>
#include <exception>

// Implemented in menuhttp.cpp. Called only from the selected game's GUI thread.
bool MenuHTTP_Install(GarrysMod::Lua::ILuaBase* lua);

namespace {
std::atomic<HHOOK> g_hook{nullptr};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};
std::atomic<const char*> g_stage{"bootstrap"};

using Factory = void* (__cdecl*)(const char*, int*);

// Record the exact native call that faulted, then permanently stop probing.
// This is a last-resort crash diagnostic, not an ABI compatibility workaround.
LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS* pointers) {
    const auto* record = pointers ? pointers->ExceptionRecord : nullptr;
    MenuHTTP_Log("EXCEPTION stage=%s code=0x%08lX address=%p; auto-install disabled",
                 g_stage.load(std::memory_order_relaxed),
                 record ? static_cast<unsigned long>(record->ExceptionCode) : 0ul,
                 record ? record->ExceptionAddress : nullptr);
    g_failed.store(true, std::memory_order_release);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool ProbeAndInstall() {
    static bool loggedShared = false;
    static bool loggedFactory = false;
    static bool loggedMenu = false;
    g_stage.store("GetModuleHandle(lua_shared.dll)", std::memory_order_relaxed);
    HMODULE module = GetModuleHandleW(L"lua_shared.dll");
    if (!module) return false;
    if (!loggedShared) {
        loggedShared = true;
        MenuHTTP_Log("lua_shared.dll loaded");
    }

    g_stage.store("GetProcAddress(CreateInterface)", std::memory_order_relaxed);
    const auto factory = reinterpret_cast<Factory>(GetProcAddress(module, "CreateInterface"));
    if (!factory) {
        if (!loggedFactory) {
            loggedFactory = true;
            MenuHTTP_Log("ERROR: lua_shared.dll has no CreateInterface export");
        }
        return false;
    }
    g_stage.store("CreateInterface(LUASHARED003)", std::memory_order_relaxed);
    auto* shared = static_cast<GarrysMod::Lua::ILuaShared*>(
        factory(GMOD_LUASHARED_INTERFACE, nullptr));
    if (!shared) {
        if (!loggedFactory) {
            loggedFactory = true;
            MenuHTTP_Log("ERROR: LUASHARED003 interface unavailable");
        }
        return false;
    }

    g_stage.store("ILuaShared::GetLuaInterface(MENU)", std::memory_order_relaxed);
    auto* menu = shared->GetLuaInterface(GarrysMod::Lua::State::MENU);
    if (!menu) return false;
    if (!loggedMenu) {
        loggedMenu = true;
        MenuHTTP_Log("MENU ILuaInterface obtained on thread %lu", GetCurrentThreadId());
    }

    // GetLuaInterface(MENU) plus the MENU_DLL global check in MenuHTTP_Install
    // is enough; calling ILuaInterface::IsMenu here added another risky vcall.
    g_stage.store("MenuHTTP_Install / Lua registration", std::memory_order_relaxed);
    try {
        return MenuHTTP_Install(static_cast<GarrysMod::Lua::ILuaBase*>(menu));
    } catch (const std::exception& error) {
        MenuHTTP_Log("C++ exception during MENU install: %s", error.what());
        g_failed.store(true, std::memory_order_release);
        return false;
    } catch (...) {
        MenuHTTP_Log("unknown C++ exception during MENU install");
        g_failed.store(true, std::memory_order_release);
        return false;
    }
}

// Must not contain objects requiring C++ stack unwinding (__try / C2712).
bool GuardedProbeAndInstall() {
    __try {
        return ProbeAndInstall();
    } __except (ExceptionFilter(GetExceptionInformation())) {
        return false;
    }
}

// WH_GETMESSAGE callback runs on the selected window's message thread.
LRESULT CALLBACK GetMessageHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && !g_ready.load(std::memory_order_acquire) &&
        !g_failed.load(std::memory_order_acquire)) {
        static thread_local bool inCallback = false;
        static thread_local ULONGLONG nextAttempt = 0;
        const ULONGLONG now = GetTickCount64();
        if (!inCallback && now >= nextAttempt) {
            inCallback = true;
            nextAttempt = now + 750; // no repeated Lua probing for every GUI message
            if (GuardedProbeAndInstall()) {
                g_ready.store(true, std::memory_order_release);
                MenuHTTP_Log("SUCCESS: MENU globals http/http2/HTTP installed");
            }
            inCallback = false;
        }
    }
    return CallNextHookEx(g_hook.load(std::memory_order_acquire), code, wparam, lparam);
}

struct MainWindow {
    DWORD thread = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM arg) {
    auto* result = reinterpret_cast<MainWindow*>(arg);
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || tid == 0 || !IsWindowVisible(hwnd) ||
        GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    wchar_t klass[128]{};
    GetClassNameW(hwnd, klass, 128);
    if (!result->window || lstrcmpiW(klass, L"Valve001") == 0) {
        result->thread = tid;
        result->window = hwnd;
    }
    return lstrcmpiW(klass, L"Valve001") != 0;
}

DWORD WINAPI Bootstrap(void*) {
    MenuHTTP_Log("DLL loaded; pid=%lu arch=%s; no Lua require",
                 GetCurrentProcessId(), sizeof(void*) == 8 ? "x64" : "x86");

    // Keep mapped for all Lua C closures and workers until process termination.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Bootstrap), &pinned)) {
        MenuHTTP_Log("FATAL: pin DLL failed, Win32 error=%lu", GetLastError());
        g_failed.store(true, std::memory_order_release);
        return 1;
    }
    MenuHTTP_Log("DLL pinned; searching for game window");

    bool reportedWindow = false;
    bool reportedHookFailure = false;
    bool reportedWindowTimeout = false;
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        if (g_ready.load(std::memory_order_acquire) ||
            g_failed.load(std::memory_order_acquire)) {
            const HHOOK old = g_hook.exchange(nullptr, std::memory_order_acq_rel);
            if (old) UnhookWindowsHookEx(old);
            const bool success = g_ready.load(std::memory_order_acquire);
            MenuHTTP_Log("bootstrap finished: %s", success ? "installed" : "disabled after exception");
            return success ? 0 : 1;
        }

        MainWindow window{};
        EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window.window && !reportedWindow) {
            wchar_t klass[128]{};
            GetClassNameW(window.window, klass, 128);
            MenuHTTP_Log("selected game window: hwnd=%p thread=%lu class=%ls",
                         window.window, window.thread, klass);
            reportedWindow = true;
        }
        if (!reportedWindowTimeout && !window.window && GetTickCount64() - started > 15000) {
            reportedWindowTimeout = true;
            MenuHTTP_Log("waiting: no visible main game window after 15 seconds");
        }
        HHOOK hook = g_hook.load(std::memory_order_acquire);
        if (!hook && window.window && window.thread != GetCurrentThreadId()) {
            hook = SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, nullptr, window.thread);
            if (hook) {
                g_hook.store(hook, std::memory_order_release);
                MenuHTTP_Log("WH_GETMESSAGE hook installed; waiting for MENU STATE");
            } else if (!reportedHookFailure) {
                reportedHookFailure = true;
                MenuHTTP_Log("ERROR: SetWindowsHookExW failed: Win32 error=%lu", GetLastError());
            }
        }
        if (hook && window.window) {
            PostMessageW(window.window, WM_NULL, 0, 0);
        }
        Sleep(500);
    }
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Avoid file I/O and C++/Lua calls under the loader lock.
        HANDLE thread = CreateThread(nullptr, 0, &Bootstrap, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

// -1 = disabled after fault, 0 = initializing, 1 = registered.
extern "C" __declspec(dllexport) int __cdecl MenuHTTP_Status() {
    if (g_failed.load(std::memory_order_acquire)) return -1;
    return g_ready.load(std::memory_order_acquire) ? 1 : 0;
}
