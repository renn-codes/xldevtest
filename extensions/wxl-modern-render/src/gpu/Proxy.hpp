// wxl-modern-render: accessors for the shared D3D12 device the d3d9 proxy owns.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <d3d12.h>

// The proxy (d3d9.dll) creates the D3D12 device once, up front, so it can hand the client's own D3D9
// device to D3D9On12 backed by it -- every extension that runs D3D12 work reuses this same device
// rather than each creating (and fighting the driver over) its own. These four are plain DLL exports
// off the proxy, resolved at link time against its import library (see module.cmake): a different
// DLL boundary than the WXL_Api table, which only ever connects an extension to WarcraftXL.dll.
extern "C"
{
    /// The shared D3D12 device, or null when the proxy's D3D9On12 bridge isn't up (system d3d9on12.dll
    /// missing, device creation failed, or the client simply hasn't triggered a device creation yet).
    /// Never release it: the proxy owns its lifetime for the process.
    __declspec(dllimport) ID3D12Device* __cdecl WxlD3D12Device();

    /// Flushes the D3D12 debug layer's queued validation messages to the proxy's log, if the debug
    /// layer is active (opt-in via WXL_D3D12_DEBUG=1; off by default, since it costs real frame time).
    /// A no-op otherwise.
    __declspec(dllimport) void __cdecl WxlD3D12DrainDebug();

    /// The current render-scale factor applied to the world pass before the post-process chain runs
    /// (1.0 = native, >1.0 = supersampled, <1.0 = upscaled). Defaults to 1.0.
    __declspec(dllimport) float __cdecl WxlGetSsaaFactor();

    /// Sets the render-scale factor for the next frame, clamped to [0.5, 2.0].
    __declspec(dllimport) void __cdecl WxlSetSsaaFactor(float factor);
}
