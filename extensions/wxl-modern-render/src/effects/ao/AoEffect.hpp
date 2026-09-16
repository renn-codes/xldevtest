// wxl-modern-graphic: horizon-based ambient occlusion (GTAO/HBAO) over the world depth, all quality tiers.
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

#pragma once

#include "gpu/Effect.hpp"

namespace wxl::scripts::render_modern
{
    /**
     * @brief Ambient occlusion from the world depth (INTZ). Reconstructs view-space position + normal from
     *        depth, estimates horizon-based occlusion (GTAO/HBAO), denoises it depth-aware, and multiplies it
     *        over the scene. Every quality tier runs the same horizon algorithm and shares one look; Low/Medium/
     *        High differ only in horizon slices/steps (cost vs quality). Needs ctx.depthSrv; passthrough when
     *        depth is absent.
     */
    class AoEffect : public IEffect
    {
    public:
        const char* Name() const override { return "SSAO"; }
        bool Init(Framework& gpu, DXGI_FORMAT rtvFmt) override;
        void SetQuality(Quality q) override;
        bool IsAntiAliasing() const override { return false; }
        bool NeedsDepth() const override { return true; }
        int QualityLevels() const override { return 3; }   // Low/Medium/High, all horizon-based (GTAO)
        void DrawTuning() override;
        void Render(Framework& gpu, const FrameContext& ctx) override;

    private:
        void ensureAo(Framework& gpu, uint32_t w, uint32_t h);

        ID3D12RootSignature* m_aoRootSig = nullptr;
        ID3D12PipelineState* m_gtaoPso   = nullptr;   // GTAO (horizon) compute, all tiers
        ID3D12RootSignature* m_blurRootSig = nullptr;
        ID3D12PipelineState* m_blurPso     = nullptr;
        ID3D12RootSignature* m_compRootSig = nullptr;
        ID3D12PipelineState* m_compPso     = nullptr;

        ID3D12Resource* m_aoTex  = nullptr;           // raw AO at HALF resolution (R32_FLOAT, UAV)
        ID3D12Resource* m_aoTexB = nullptr;           // denoised + upsampled AO at full resolution
        D3D12_RESOURCE_STATES m_aoState  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES m_aoStateB = D3D12_RESOURCE_STATE_COMMON;
        uint32_t m_w = 0, m_h = 0;                    // full (scene) resolution
        uint32_t m_aoHalfW = 0, m_aoHalfH = 0;        // half resolution the raw AO is computed at

        // Shared look across all tiers (set by SetQuality); live-tunable in DrawTuning.
        float m_intensity = 0.45f;
        float m_radius    = 0.5f;   // occlusion radius in world units
        float m_power     = 1.30f;  // AO contrast
        float m_blurPx    = 2.0f;   // denoise kernel spread
        float m_blurSharp = 10.0f;  // edge preservation in the denoise

        // Distance fade: AO is full strength up to m_fadeStart (view-space depth, world units) and lerps to
        // none (AO = 1) by m_fadeEnd. Disabled when m_fadeEnd <= m_fadeStart. On by default (0 -> 50) so AO
        // stays a near-field effect and does not occlude distant terrain.
        float m_fadeStart = 0.0f;
        float m_fadeEnd   = 50.0f;

        // Horizon cost/quality, set per tier by SetQuality (Low 2x4, Medium 4x8, High 8x16): slices and steps
        // per slice. Thickness is a thin-occluder blend (0 = opaque occluders, 1 = horizons see past a sample).
        uint32_t m_gtaoSlices = 4;
        uint32_t m_gtaoSteps  = 8;
        float    m_gtaoThickness = 0.02f;
    };
}
