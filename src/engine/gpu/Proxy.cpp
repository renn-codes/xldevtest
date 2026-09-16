// The d3d9.dll proxy: pass Direct3DCreate9(Ex) through to the system d3d9 (or, when available, a
// D3D9On12-backed factory sharing a D3D12 device with the core) and load WarcraftXL.dll.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// The client LoadLibrary's "d3d9.dll" from its own folder first, so this proxy loads ahead of the system
// one. Ordinarily it does exactly two things: forward the factory-create exports to the real system d3d9,
// and load WarcraftXL.dll into the process. All rendering runs on the client's native D3D9 device.
//
// When D3D9On12 is available (Windows 10+, its own d3d9on12.dll present) this proxy instead creates a
// D3D12 device and hands both device creation calls a D3D9On12-backed factory built on it, so the
// client's own D3D9 device is queryable for IDirect3DDevice9On12 -- what a D3D12-based extension (e.g.
// wxl-modern-render) needs to run its own D3D12 work against the SAME device and present through the
// client's normal D3D9 swap chain, unmodified. Building the D3D12 device/queue and probing for
// d3d9on12.dll happen through the exact same public, versioned Microsoft API surface Microsoft's own
// D3D9On12 sample and browser engines (e.g. ANGLE) use for this same interop -- nothing here reaches into
// the WoW client's own internals. Every step degrades to the plain passthrough on any failure (missing
// d3d9on12.dll, no D3D12-capable adapter, device/queue creation failure), so a system without D3D9On12
// support renders exactly as it did before this file existed.
//
// d3d9on12.dll is loaded dynamically (LoadLibrary + GetProcAddress, like the system d3d9.dll below)
// rather than linked statically: a hard load-time dependency on it would make THIS DLL -- the client's
// injection point -- refuse to load entirely on a system that lacks it, which is a far worse failure
// than simply not getting the D3D12 bridge. d3d12.dll itself IS linked statically: it has shipped with
// every Windows 10 release since RTM, which is this project's stated minimum target.

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>

#include "common/Log.hpp"

#include <cstdarg>
#include <cstdlib>

namespace
{
    using Create9Fn   = IDirect3D9* (WINAPI*)(UINT);
    using Create9ExFn = HRESULT     (WINAPI*)(UINT, IDirect3D9Ex**);
    Create9Fn   g_realCreate9   = nullptr;
    Create9ExFn g_realCreate9Ex = nullptr;

    using Create9On12Fn   = IDirect3D9* (WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);
    using Create9On12ExFn = HRESULT     (WINAPI*)(UINT, D3D9ON12_ARGS*, UINT, IDirect3D9Ex**);
    Create9On12Fn   g_create9On12   = nullptr;
    Create9On12ExFn g_create9On12Ex = nullptr;

    // The shared D3D12 device + queue handed to D3D9On12, and to any extension's own D3D12 work on the
    // client's device (see an extension's own src/gpu/Proxy.hpp). Null until EnsureD3D12Bridge succeeds,
    // and permanently null if it never does -- everything here degrades to the plain passthrough.
    ID3D12Device*       g_d3d12Device    = nullptr;
    ID3D12CommandQueue* g_d3d12Queue     = nullptr;
    ID3D12InfoQueue*    g_d3d12InfoQueue = nullptr;   // only set when WXL_D3D12_DEBUG=1
    float               g_ssaaFactor     = 1.0f;

    /**
     * @brief Writes one Info line to the proxy's own log sink, opening it on first use.
     *
     * The proxy is a distinct module from WarcraftXL.dll and owns a separate log instance. Lines are
     * sparse boot/crash diagnostics and are flushed immediately -- the DLL has no orderly close on exit.
     * @param fmt  printf-style format string followed by its arguments.
     */
    void Log(const char* fmt, ...)
    {
        ::wxl::log::Open("Logs\\d3d9proxy.log");   // idempotent
        if (!::wxl::log::Enabled(::wxl::log::Level::Info)) return;
        va_list ap;
        va_start(ap, fmt);
        ::wxl::log::WriteV(::wxl::log::Level::Info, fmt, ap);
        va_end(ap);
        ::wxl::log::Flush();
    }

    /**
     * @brief Loads the real d3d9 from the system directory, falling back to a local d3d9_real.dll.
     *
     * A loaded module is keyed by full path, so the system d3d9.dll is a distinct module from this proxy
     * despite the shared base name.
     * @return Handle to the real d3d9 module, or null on failure.
     */
    HMODULE LoadRealD3D9()
    {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n != 0 && n < MAX_PATH - 16)
        {
            lstrcatA(path, "\\d3d9.dll");
            if (HMODULE r = LoadLibraryA(path)) return r;
        }
        return LoadLibraryA("d3d9_real.dll");
    }

    /** @brief Lazily loads the real d3d9 and resolves its create entry points on first use. */
    void EnsureReal()
    {
        if (g_realCreate9 || g_realCreate9Ex) return;
        HMODULE r = LoadRealD3D9();
        if (!r) { Log("d3d9proxy: FAILED to load the system d3d9.dll"); return; }
        g_realCreate9   = reinterpret_cast<Create9Fn>(GetProcAddress(r, "Direct3DCreate9"));
        g_realCreate9Ex = reinterpret_cast<Create9ExFn>(GetProcAddress(r, "Direct3DCreate9Ex"));
        Log("d3d9proxy: system d3d9 loaded (9=%p Ex=%p)", g_realCreate9, g_realCreate9Ex);
    }

    /**
     * @brief Loads WarcraftXL.dll once, from the first Direct3DCreate9* call.
     *
     * Deliberately NOT done in DllMain: calling LoadLibrary under the loader lock can deadlock against
     * WarcraftXL.dll's own attach work. The engine creates its factory long before the runtime is needed,
     * so first-create is early enough and runs outside the loader lock.
     */
    void EnsureRuntimeLoaded()
    {
        static bool attempted = false;
        if (attempted) return;
        attempted = true;
        if (!LoadLibraryA("WarcraftXL.dll"))
            Log("d3d9proxy: WarcraftXL.dll not loaded (win32=%lu)", GetLastError());
    }

    /**
     * @brief Lazily loads d3d9on12.dll and resolves its two create entry points, once.
     * @return true if at least one entry point resolved.
     */
    bool EnsureD3D9On12Loaded()
    {
        static bool attempted = false;
        if (attempted) return g_create9On12 || g_create9On12Ex;
        attempted = true;

        HMODULE m = LoadLibraryA("d3d9on12.dll");
        if (!m) { Log("d3d9proxy: d3d9on12.dll not available"); return false; }
        g_create9On12   = reinterpret_cast<Create9On12Fn>(GetProcAddress(m, "Direct3DCreate9On12"));
        g_create9On12Ex = reinterpret_cast<Create9On12ExFn>(GetProcAddress(m, "Direct3DCreate9On12Ex"));
        Log("d3d9proxy: d3d9on12.dll loaded (9On12=%p 9On12Ex=%p)", g_create9On12, g_create9On12Ex);
        return g_create9On12 || g_create9On12Ex;
    }

    /**
     * @brief Creates the shared D3D12 device and a dedicated DIRECT queue for D3D9On12, once.
     *
     * The debug layer is opt-in (WXL_D3D12_DEBUG=1): it costs real frame time, so it stays off unless a
     * developer chasing a D3D12 issue asks for it.
     * @return true once a device is up; cached, so later calls are free.
     */
    bool EnsureD3D12Bridge()
    {
        static bool attempted = false;
        static bool ok = false;
        if (attempted) return ok;
        attempted = true;

        if (GetEnvironmentVariableA("WXL_D3D12_DEBUG", nullptr, 0) > 0)
        {
            ID3D12Debug* debug = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)&debug)) && debug)
            {
                debug->EnableDebugLayer();
                debug->Release();
                Log("d3d9proxy: D3D12 debug layer enabled (WXL_D3D12_DEBUG=1)");
            }
        }

        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                     __uuidof(ID3D12Device), (void**)&g_d3d12Device)))
        {
            Log("d3d9proxy: D3D12CreateDevice failed");
            return false;
        }

        // Only present when the debug layer above was actually enabled; harmless to not have one.
        g_d3d12Device->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&g_d3d12InfoQueue);

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(g_d3d12Device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_d3d12Queue)))
        {
            Log("d3d9proxy: CreateCommandQueue failed");
            if (g_d3d12InfoQueue) { g_d3d12InfoQueue->Release(); g_d3d12InfoQueue = nullptr; }
            g_d3d12Device->Release();
            g_d3d12Device = nullptr;
            return false;
        }

        Log("d3d9proxy: D3D12 device + queue ready (device=%p queue=%p)",
            (void*)g_d3d12Device, (void*)g_d3d12Queue);
        ok = true;
        return true;
    }

    /** @brief D3D9ON12_ARGS pointing D3D9On12 at the shared device + queue above. */
    D3D9ON12_ARGS MakeOn12Args()
    {
        D3D9ON12_ARGS args{};
        args.Enable9On12 = TRUE;
        args.pD3D12Device = g_d3d12Device;
        args.ppD3D12Queues[0] = g_d3d12Queue;
        args.NumQueues = 1;
        return args;
    }
}

/**
 * @brief Proxy entry point for Direct3DCreate9: tries the D3D9On12 bridge, then forwards to the system d3d9.
 * @param sdkVersion  D3D SDK version passed by the caller.
 * @return The native (or D3D9On12-backed) IDirect3D9 factory, or null on failure.
 */
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    EnsureRuntimeLoaded();

    if (EnsureD3D9On12Loaded() && g_create9On12 && EnsureD3D12Bridge())
    {
        D3D9ON12_ARGS args = MakeOn12Args();
        if (IDirect3D9* d9 = g_create9On12(sdkVersion, &args, 1))
        {
            Log("d3d9proxy: D3D9On12 bridge active (Direct3DCreate9)");
            return d9;
        }
        Log("d3d9proxy: Direct3DCreate9On12 failed, falling back to native d3d9");
    }

    EnsureReal();
    return g_realCreate9 ? g_realCreate9(sdkVersion) : nullptr;
}

/**
 * @brief Proxy entry point for Direct3DCreate9Ex: tries the D3D9On12 bridge, then forwards to the system d3d9.
 * @param sdkVersion  D3D SDK version passed by the caller.
 * @param out         receives the native (or D3D9On12-backed) IDirect3D9Ex factory.
 * @return S_OK on success, E_NOINTERFACE when neither path can produce a factory.
 */
extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    EnsureRuntimeLoaded();

    if (out && EnsureD3D9On12Loaded() && g_create9On12Ex && EnsureD3D12Bridge())
    {
        D3D9ON12_ARGS args = MakeOn12Args();
        const HRESULT hr = g_create9On12Ex(sdkVersion, &args, 1, out);
        if (SUCCEEDED(hr) && *out)
        {
            Log("d3d9proxy: D3D9On12 bridge active (Direct3DCreate9Ex)");
            return S_OK;
        }
        Log("d3d9proxy: Direct3DCreate9On12Ex failed (hr=0x%08lX), falling back to native d3d9",
            static_cast<unsigned long>(hr));
    }

    EnsureReal();
    if (g_realCreate9Ex) return g_realCreate9Ex(sdkVersion, out);
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}

/**
 * @brief The shared D3D12 device, for a D3D12-based extension to run its own work on -- or null when the
 *        D3D9On12 bridge isn't up. Never release it: the proxy owns its lifetime for the process.
 */
extern "C" ID3D12Device* __cdecl WxlD3D12Device()
{
    return g_d3d12Device;
}

/**
 * @brief Flushes the D3D12 debug layer's queued validation messages to the proxy's log, if the debug
 *        layer is active (WXL_D3D12_DEBUG=1). A no-op otherwise.
 */
extern "C" void __cdecl WxlD3D12DrainDebug()
{
    if (!g_d3d12InfoQueue) return;

    const UINT64 count = g_d3d12InfoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T len = 0;
        if (FAILED(g_d3d12InfoQueue->GetMessage(i, nullptr, &len)) || !len) continue;
        auto* msg = static_cast<D3D12_MESSAGE*>(malloc(len));
        if (!msg) continue;
        if (SUCCEEDED(g_d3d12InfoQueue->GetMessage(i, msg, &len)))
            Log("d3d9proxy: [D3D12 sev=%d] %s", static_cast<int>(msg->Severity), msg->pDescription);
        free(msg);
    }
    g_d3d12InfoQueue->ClearStoredMessages();
}

/** @brief The render-scale factor applied to the world pass (1.0 = native). Defaults to 1.0. */
extern "C" float __cdecl WxlGetSsaaFactor()
{
    return g_ssaaFactor;
}

/** @brief Sets the render-scale factor for the next frame, clamped to [0.5, 2.0]. */
extern "C" void __cdecl WxlSetSsaaFactor(float factor)
{
    if (factor < 0.5f) factor = 0.5f;
    if (factor > 2.0f) factor = 2.0f;
    g_ssaaFactor = factor;
}

/**
 * @brief Process-attach entry point. Intentionally does no work.
 *
 * Loading the real d3d9 and WarcraftXL.dll happens lazily in Direct3DCreate9(Ex): a LoadLibrary issued
 * here would run under the loader lock and can deadlock against the loaded DLL's attach.
 * @param reason  DLL notification reason.
 * @return TRUE.
 */
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    (void)reason;
    return TRUE;
}
