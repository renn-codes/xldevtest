// wxl-water-band: cheap full-frame "reflection" via flipped screen-space banding.
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

#include "WaterBand.hpp"
#include "Hlsl.hpp"

#include <d3d9.h>

namespace wxl::scripts::waterband
{
    namespace gx = wxl::game::gx;
    namespace ev = wxl::events;

    // D3DFMT_X8R8G8B8 -- no alpha needed for the scratch copy.
    constexpr uint32_t kFmtX8R8G8B8 = 22;

    WaterBand::WaterBand()
    {
        on<&WaterBand::OnWorldRenderEnd>(ev::Event::OnWorldRenderEnd);
    }

    bool WaterBand::EnsureResources(gx::Device9 dev)
    {
        if (!bandPS_)
            bandPS_ = gx::CompilePixelShader(dev, kWaterBandHLSL, "ps_2_0");

        if (!scratch_.surface)
            gx::EnsureBackbufferTarget(dev, scratch_, kFmtX8R8G8B8);

        return bandPS_ && scratch_.surface;
    }

    void WaterBand::BandPass(gx::Device9 dev)
    {
        // --- copy the current (pre-UI) render target into the scratch RT via StretchRect ---
        // At OnWorldRenderEnd the world pass has just finished and UI has not drawn yet, so whatever
        // is bound as render target 0 right now is the scene we want -- not GetBackBuffer, since on
        // some configurations the active target at this point isn't the swap-chain back buffer yet.
        void* curRT = nullptr;
        dev.GetRenderTarget(0, &curRT);
        if (!curRT) return;

        auto* src = static_cast<IDirect3DSurface9*>(curRT);
        auto* dst = static_cast<IDirect3DSurface9*>(scratch_.surface);
        static_cast<IDirect3DDevice9*>(dev.raw())->StretchRect(src, nullptr, dst, nullptr, D3DTEXF_NONE);
        gx::Release(curRT);

        // --- state save ---
        const unsigned sZE = dev.GetRenderState(gx::rs::kZEnable);
        const unsigned sAB = dev.GetRenderState(gx::rs::kAlphaBlend);
        const unsigned sCU = dev.GetRenderState(gx::rs::kCullMode);
        void* oldPS = nullptr; dev.GetPixelShader(&oldPS);

        // --- draw the band quad ---
        dev.SetRenderState(gx::rs::kZEnable,    0);
        dev.SetRenderState(gx::rs::kAlphaBlend, 0);
        dev.SetRenderState(gx::rs::kCullMode,   gx::cull::kNone);

        dev.SetTexture(0, scratch_.texture);
        dev.SetPixelShader(bandPS_);

        // c0: (bandStart, maxOpacity, flipBias, 0)
        const float consts[4] = { bandStart, maxOpacity, flipBias, 0.0f };
        dev.SetPixelShaderConstantF(0, consts, 1);

        gx::DrawFullscreenQuad(dev);

        // --- state restore ---
        dev.SetPixelShader(oldPS);
        dev.SetTexture(0, nullptr);
        dev.SetRenderState(gx::rs::kZEnable,    sZE);
        dev.SetRenderState(gx::rs::kAlphaBlend, sAB);
        dev.SetRenderState(gx::rs::kCullMode,   sCU);
        gx::Release(oldPS);
    }

    void WaterBand::OnWorldRenderEnd(const ev::WorldRenderEndArgs& a)
    {
        gx::Device9 dev(a.device);
        if (!dev) return;
        if (!EnsureResources(dev)) return;
        BandPass(dev);
    }
}

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "WaterBand",
        1,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;

    // Ahead of the constructor, which is where the handler binds.
    wxl::ext::EventScript::Bind(api);
    static wxl::scripts::waterband::WaterBand waterBand;

    api->Log(WXL_LOG_INFO, "WaterBand", "cheap full-frame reflection band active");
    return 1;
}
