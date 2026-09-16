// wxl-runtime extension-local bridge to the Hub ABI.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include "common/ExtensionConfig.hpp"
#include "wxl/FrameScriptApi.h"
#include "wxl/NetworkApi.h"
#include "wxl/PluginApi.h"

#include <cstddef>
#include <cstdint>

namespace wxl_runtime
{
    extern const WXL_Api* g_api;
    const WXL_FrameScriptApi* FrameScriptApi();

    template <class Fn>
    inline bool HookAttach(const char* name, uintptr_t target, Fn* detour, Fn** original,
                           int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttach(name, target, reinterpret_cast<void*>(detour),
                                 reinterpret_cast<void**>(original), priority) != 0;
    }

    inline bool ConfigBool(const char* name, bool fallback)
    {
        char value[16] = {};
        return wxl::ext::config::Raw(name, value, sizeof value,
                                     "Extensions\\wxl-runtime\\wxl-runtime.cfg")
            ? wxl::ext::config::Truthy(value, fallback)
            : fallback;
    }

    bool InstallFrameScriptBridge();
    bool InstallNetworkBridge();
}

#define WLOG_TRACE(...) ::wxl_runtime::g_api->Log(WXL_LOG_TRACE, "wxl-runtime", __VA_ARGS__)
#define WLOG_DEBUG(...) ::wxl_runtime::g_api->Log(WXL_LOG_DEBUG, "wxl-runtime", __VA_ARGS__)
#define WLOG_INFO(...)  ::wxl_runtime::g_api->Log(WXL_LOG_INFO,  "wxl-runtime", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_runtime::g_api->Log(WXL_LOG_WARN,  "wxl-runtime", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_runtime::g_api->Log(WXL_LOG_ERROR, "wxl-runtime", __VA_ARGS__)
