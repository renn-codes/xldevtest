// wxl-modern-render: entry point. Binds the Hub ABI, then brings up the render module, grass motion,
// and the dev-overlay panel -- each independently, so one failing does not take the others down.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"

#include "wxl/EventScript.hpp"

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info{
        sizeof(WXL_PluginInfo), WXL_API_VERSION, "wxl-modern-render", 1, WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;
    wxl_modern_render::g_api = api;

    // Ahead of any EventScript subclass's constructor, which is where the handlers bind.
    wxl::ext::EventScript::Bind(api);

    bool ok = wxl_modern_render::InstallRenderModernModule();
    ok &= wxl_modern_render::InstallGrassMotion();
    ok &= wxl_modern_render::InstallOverlayPanel();

    api->Log(WXL_LOG_INFO, "wxl-modern-render", "modern render pipeline loaded");
    return ok ? 1 : 0;
}
