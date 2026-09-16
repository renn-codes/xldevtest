#include "ExtensionApi.hpp"

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info{
        sizeof(WXL_PluginInfo), WXL_API_VERSION, "wxl-runtime", 1, WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;
    wxl_runtime::g_api = api;

    api->PublishInterface("wxl.framescript", WXL_FRAME_SCRIPT_API_VERSION,
                          const_cast<WXL_FrameScriptApi*>(wxl_runtime::FrameScriptApi()));
    if (!wxl_runtime::InstallFrameScriptBridge()) return 0;
    if (!wxl_runtime::InstallNetworkBridge()) return 0;

    api->Log(WXL_LOG_INFO, "wxl-runtime", "shared runtime services published");
    return 1;
}
