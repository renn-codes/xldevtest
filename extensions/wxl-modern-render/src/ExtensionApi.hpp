// wxl-modern-render extension-local bridge to the Hub ABI.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include "wxl/PluginApi.h"

#include <cstdint>

namespace wxl_modern_render
{
    extern const WXL_Api* g_api;

    template <class Fn>
    inline bool HookAttach(const char* name, uintptr_t target, Fn* detour, Fn** original,
                           int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttach(name, target, reinterpret_cast<void*>(detour),
                                 reinterpret_cast<void**>(original), priority) != 0;
    }

    bool InstallRenderModernModule();
    bool InstallGrassMotion();
    bool InstallOverlayPanel();
}

#define WLOG_TRACE(...) ::wxl_modern_render::g_api->Log(WXL_LOG_TRACE, "wxl-modern-render", __VA_ARGS__)
#define WLOG_DEBUG(...) ::wxl_modern_render::g_api->Log(WXL_LOG_DEBUG, "wxl-modern-render", __VA_ARGS__)
#define WLOG_INFO(...)  ::wxl_modern_render::g_api->Log(WXL_LOG_INFO,  "wxl-modern-render", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_modern_render::g_api->Log(WXL_LOG_WARN,  "wxl-modern-render", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_modern_render::g_api->Log(WXL_LOG_ERROR, "wxl-modern-render", __VA_ARGS__)
