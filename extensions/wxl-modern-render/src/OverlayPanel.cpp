// wxl-render-modern: the dev-overlay panel exposing the post-process effects (enable + quality tier).
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

#include "ExtensionApi.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/Proxy.hpp"

#include "imgui.h"

// Registers a "Graphics" panel with the dev overlay. The panel is generic over the effect chain: each effect
// exposes an enable toggle and a quality tier, so effects added later appear here automatically. Anti-aliasing
// methods are mutually exclusive; SSAO and supersampling are independent and stack with everything.
namespace wxl::scripts::render_modern
{
    namespace
    {
        const char* const k_qualityNames[] = { "Low", "Medium", "High", "Ultra" };

        void __cdecl DrawGraphicsPanel(void* /*user*/)
        {
            const auto& effects = Pipeline::Get().Effects();
            if (effects.empty())
            {
                ImGui::TextDisabled("(no effects registered)");
                return;
            }

            // Engine MSAA: all effects run under it now. PPAA composites over the resolved MSAA frame; SSAO and
            // Render Scale render the world single-sample into our offscreen (the post-process becomes its
            // anti-aliasing) and draw the result back onto the MSAA backbuffer.
            if (Pipeline::Get().MsaaActive())
            {
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Engine MSAA active (effects supported)");
                ImGui::Spacing();
            }

            for (const auto& e : effects)
            {
                ImGui::PushID(e.get());

                bool on = e->Enabled();
                if (ImGui::Checkbox(e->Name(), &on))
                {
                    e->SetEnabled(on);
                    // Anti-aliasing methods are mutually exclusive: enabling one disables the other AA methods.
                    // SSAO is not anti-aliasing, so it is left alone and stacks with whichever AA is on.
                    if (on && e->IsAntiAliasing())
                        for (const auto& other : effects)
                            if (other.get() != e.get() && other->IsAntiAliasing())
                                other->SetEnabled(false);
                }

                int q = static_cast<int>(e->GetQuality());
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f);
                // Each effect shows its first QualityLevels() names from k_qualityNames -- all are Low/Medium/High
                // now (the Ultra tier was retired when SSAO became GTAO at every tier; the name is kept for any
                // future 4-tier effect).
                if (ImGui::Combo("##quality", &q, k_qualityNames, e->QualityLevels()))
                    e->SetQuality(static_cast<Quality>(q));

                // Effect-specific live tuning controls (SSAO sliders, ...), shown while the effect is enabled.
                if (on)
                    e->DrawTuning();

                ImGui::PopID();
            }

            // Render scale (formerly SSAA): the world renders at native * scale and the pipeline scales the
            // result onto the backbuffer -- a supersampling downsample above 100%, a bilinear upscale below it.
            // It is independent of the effects above (each runs at the scaled resolution), is compatible with the
            // engine's native MSAA, and applies live on the next frame. 100% = off.
            ImGui::Spacing();
            ImGui::TextUnformatted("Render Scale");
            int pct = (int)(WxlGetSsaaFactor() * 100.0f + 0.5f);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("##renderscale", &pct, 50, 200, "%d%%"))
            {
                if (pct < 50)  pct = 50;
                if (pct > 200) pct = 200;
                WxlSetSsaaFactor((float)pct / 100.0f);
            }
        }
    }
}

bool wxl_modern_render::InstallOverlayPanel()
{
    g_api->UiAddPanel("Graphics", &wxl::scripts::render_modern::DrawGraphicsPanel, nullptr);
    return true;
}
