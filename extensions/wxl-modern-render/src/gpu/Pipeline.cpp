// wxl-modern-graphic: the post-process pipeline. Runs the effect chain at native resolution and resolves to
// the native backbuffer; supersampling is a render-size source the fill downsamples on the way in.
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

#include "Pipeline.hpp"

#include "effects/ao/AoEffect.hpp"
#include "effects/aa/FxaaEffect.hpp"
#include "effects/aa/SmaaEffect.hpp"
#include "effects/aa/Cmaa2Effect.hpp"
#include "common/Log.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>

#include <atomic>
#include <thread>
#include <mutex>

namespace wxl::scripts::render_modern
{
    // The chain, in render order. Ambient occlusion runs first so its darkening is itself anti-aliased by a
    // later pass; the anti-aliasing methods run last. They are mutually exclusive (the overlay keeps one
    // enabled at a time), but SSAO is orthogonal and stacks with whichever AA, with or without supersampling.
    // Every effect ships disabled: an empty run is a 1:1 passthrough.
    Pipeline::Pipeline()
    {
        auto add = [this](std::unique_ptr<IEffect> e) { e->SetEnabled(false); m_effects.emplace_back(std::move(e)); };
        add(std::make_unique<AoEffect>());      // SSAO (depth-using); stacks with any AA and with supersampling
        add(std::make_unique<FxaaEffect>());    // default AA
        add(std::make_unique<SmaaEffect>());
        add(std::make_unique<Cmaa2Effect>());
    }

    // Resolve pass: sample the final scene and write it to the backbuffer. With a render-size scene and the
    // linear sampler this is the supersampling downsample; at 1:1 it is a plain copy.
    static const char* k_blitPS =
        "Texture2D    scene : register(t0);\n"
        "SamplerState smp   : register(s0);\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  return float4(scene.Sample(smp, uv).rgb, 1);\n"
        "}\n";

    Pipeline& Pipeline::Get()
    {
        static Pipeline instance;
        return instance;
    }

    bool Pipeline::HasEnabledEffect() const
    {
        for (const auto& e : m_effects)
            if (e->Enabled()) return true;
        return false;
    }

    void Pipeline::PrepareForReset()
    {
        // Frame() returns borrowed On12 resources with this queue's completion fence. A reset may arrive
        // before the following BeginFrame performs its normal wait, so retire that final frame while the
        // engine-owned color/depth resources are still alive.
        if (m_gpuReady)
            m_gpu.WaitForGpu();
    }

    bool Pipeline::ensureInit(DXGI_FORMAT fmt)
    {
        if (m_init) return true;

        // Each effect initializes once; one that fails is dropped, not fatal (the rest, and the passthrough,
        // survive). The effect set is fixed in the constructor, so this runs a single time.
        for (auto it = m_effects.begin(); it != m_effects.end(); )
        {
            if (!(*it)->Init(m_gpu, fmt))
            {
                WLOG_ERROR("wxl-modern-graphic: effect init failed, dropped: %s", (*it)->Name());
                it = m_effects.erase(it);
            }
            else ++it;
        }

        // The fill PSO writes into the single-sample scene targets, so it is always 1-sample. The separate
        // output-to-backbuffer PSO (m_blitPsoBB) must instead match the backbuffer's sample count (1, or N under
        // engine MSAA) and is built / rebuilt by ensureBlitBB.
        if (!m_gpu.CreateTextureFx(k_blitPS, fmt, 1, &m_blitRootSig, &m_blitPso))
        {
            WLOG_ERROR("wxl-modern-graphic: fill pass init failed");
            return false;
        }

        m_init = true;
        WLOG_INFO("wxl-modern-graphic: pipeline init OK (%d effects)", (int)m_effects.size());
        return true;
    }

    bool Pipeline::ensureBlitBB(DXGI_FORMAT fmt, uint32_t samples)
    {
        if (m_blitPsoBB && m_blitBBSamples == samples && m_blitBBFmt == fmt) return true;
        // The sample count changes when the engine's MSAA setting is toggled (a device reset). Rebuild the
        // output PSO to match; the GPU may still reference the old one, so drain before releasing it.
        if (m_blitPsoBB) { m_gpu.WaitForGpu(); m_blitPsoBB->Release(); m_blitPsoBB = nullptr; }

        ID3DBlob* ps = m_gpu.Compile(k_blitPS, "main", "ps_5_0");
        if (!ps) { WLOG_ERROR("wxl-modern-graphic: blit PS compile failed"); return false; }
        m_blitPsoBB = m_gpu.CreateFullscreenPSO(m_blitRootSig, ps, fmt, samples);
        ps->Release();
        if (!m_blitPsoBB) { WLOG_ERROR("wxl-modern-graphic: backbuffer blit PSO (samples=%u) failed", samples); return false; }
        m_blitBBSamples = samples; m_blitBBFmt = fmt;
        WLOG_INFO("wxl-modern-graphic: backbuffer blit PSO ready (samples=%u)", samples);
        return true;
    }

    bool Pipeline::ensureMsaaResolve(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
    {
        if (m_msaaResolve && m_msaaResolveW == w && m_msaaResolveH == h && m_msaaResolveFmt == fmt) return true;
        if (m_msaaResolve) { m_gpu.WaitForGpu(); m_msaaResolve->Release(); m_msaaResolve = nullptr; }
        m_msaaResolve = m_gpu.CreateTex2D(w, h, fmt, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        if (!m_msaaResolve) { WLOG_ERROR("wxl-modern-graphic: MSAA resolve tex %ux%u failed", w, h); return false; }
        m_msaaResolveW = w; m_msaaResolveH = h; m_msaaResolveFmt = fmt;
        return true;
    }

    void Pipeline::ensureTargets(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
    {
        if (m_scene[0] && m_w == w && m_h == h && m_fmt == fmt) return;

        // Recreating (a resolution / supersample-factor change): the previous frame's command list may still
        // reference the old scene targets, and this runs before BeginFrame's own wait, so wait for the GPU
        // before freeing them. Skipped on first creation (nothing in flight, nothing to free).
        if (m_scene[0]) m_gpu.WaitForGpu();

        for (int i = 0; i < 2; ++i)
            if (m_scene[i]) { m_scene[i]->Release(); m_scene[i] = nullptr; }

        for (int i = 0; i < 2; ++i)
        {
            m_scene[i] = m_gpu.CreateTex2D(w, h, fmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            m_sceneState[i] = D3D12_RESOURCE_STATE_COMMON;
        }

        m_w = w; m_h = h; m_fmt = fmt;
    }

    void Pipeline::Frame(IDirect3DDevice9On12* on12, ID3D12Device* device,
                         IDirect3DResource9* backbuffer, IDirect3DResource9* superSample,
                         IDirect3DResource9* depth, const float* proj, const float* view)
    {
        // Bring up the framework and its dedicated queue before unwrapping: On12 needs the exact queue the
        // work runs on for the unwrap/return fences. The format-dependent setup happens later in ensureInit.
        if (!m_gpuReady)
        {
            if (!m_gpu.Init(device)) return;
            m_gpuReady = true;
        }
        ID3D12CommandQueue* queue = m_gpu.Queue();

        // Always unwrap the native backbuffer: it is the final output (and, with no supersampling, also the
        // scene input).
        ID3D12Resource* bb12 = nullptr;
        HRESULT uhr = on12->UnwrapUnderlyingResource(backbuffer, queue, __uuidof(ID3D12Resource), (void**)&bb12);
        if (FAILED(uhr) || !bb12)
        {
            static bool once = false;
            if (!once) { once = true; WLOG_ERROR("wxl-modern-graphic: unwrap backbuffer failed hr=0x%08X", (unsigned)uhr); }
            return;
        }

        D3D12_RESOURCE_DESC bd = bb12->GetDesc();
        const uint32_t    nativeW = (uint32_t)bd.Width, nativeH = bd.Height;
        const uint32_t    samples = bd.SampleDesc.Count;
        const DXGI_FORMAT fmt     = bd.Format;
        m_lastSamples = samples;

        // Engine MSAA: the world is in a multisampled backbuffer a shader cannot sample, and the core does not
        // arm the supersampling/depth redirect under MSAA (so superSample is null and depth is null here). The
        // chain still runs: the MSAA backbuffer is resolved into a single-sample scene input below, the effects
        // run single-sample, and the output draw writes back into the MSAA backbuffer through m_blitPsoBB
        // (built for its sample count). Depth-using effects fall back to passthrough (no sampleable MSAA depth).

        // Scene input. With supersampling the world is in a render-size offscreen surface that the scene fill
        // downsamples; otherwise the backbuffer itself is the input. The scene targets are always NATIVE size:
        // the supersampling resolve happens on the way IN (the fill samples the larger source into the native
        // scene), so the effects run at native resolution and the final output is a 1:1 copy.
        ID3D12Resource* ss12 = nullptr;
        ID3D12Resource* sceneInput = bb12;
        if (superSample && SUCCEEDED(on12->UnwrapUnderlyingResource(superSample, queue, __uuidof(ID3D12Resource), (void**)&ss12)) && ss12)
            sceneInput = ss12;

        const uint32_t sceneW = nativeW, sceneH = nativeH;

        {
            static int diag = 0;
            if (diag < 3) { diag++; WLOG_INFO("wxl-modern-graphic: frame fmt=%d native=%ux%u ssaa=%d",
                                              (int)fmt, nativeW, nativeH, ss12 ? 1 : 0); }
        }

        // On any post-unwrap early exit, hand the borrowed surfaces back to On12 before releasing them: an
        // unwrap with no matching return leaves the D3D9 surface stuck lent-out and desyncs the engine's later
        // D3D9 use of it. BeginFrame has not run yet here, so the return carries no fence.
        auto abort = [&]() {
            if (ss12) { on12->ReturnUnderlyingResource(superSample, 0, nullptr, nullptr); ss12->Release(); }
            on12->ReturnUnderlyingResource(backbuffer, 0, nullptr, nullptr);
            bb12->Release();
        };
        if (!ensureInit(fmt)) { abort(); return; }
        ensureTargets(sceneW, sceneH, fmt);
        if (!m_scene[0] || !m_scene[1]) { abort(); return; }
        if (!ensureBlitBB(fmt, samples)) { abort(); return; }   // output PSO matched to the backbuffer sample count

        // Engine MSAA with no supersample source (PPAA only): the world is in the multisampled backbuffer, so
        // resolve it into a single-sample scene input. When a supersample source IS present (render scale / SSAO
        // under MSAA hand the world over already single-sampled in g_ssaaColor), that is the input instead and
        // the backbuffer is the output target only.
        const bool resolveBB = (samples > 1 && !ss12);
        if (resolveBB)
        {
            if (!ensureMsaaResolve(nativeW, nativeH, fmt)) { abort(); return; }
            sceneInput = m_msaaResolve;
        }

        ID3D12GraphicsCommandList* cmd = m_gpu.BeginFrame();

        auto sceneTo = [&](int i, D3D12_RESOURCE_STATES s) {
            if (m_sceneState[i] != s) { m_gpu.Barrier(m_scene[i], m_sceneState[i], s); m_sceneState[i] = s; }
        };

        // MSAA (PPAA only): resolve the engine's multisampled backbuffer into the single-sample scene input. bb12
        // returns to COMMON afterward so the output path moves it to RENDER_TARGET exactly like the 1-sample case.
        if (resolveBB)
        {
            m_gpu.Barrier(bb12,          D3D12_RESOURCE_STATE_COMMON,        D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            m_gpu.Barrier(m_msaaResolve, D3D12_RESOURCE_STATE_COMMON,        D3D12_RESOURCE_STATE_RESOLVE_DEST);
            cmd->ResolveSubresource(m_msaaResolve, 0, bb12, 0, fmt);
            m_gpu.Barrier(m_msaaResolve, D3D12_RESOURCE_STATE_RESOLVE_DEST,  D3D12_RESOURCE_STATE_COMMON);
            m_gpu.Barrier(bb12,          D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }

        // Sampleable depth (single-sample only) + the stencil view (water tagged in bit 0). A multisampled
        // frame passes no depth, so depth-using effects fall back to passthrough.
        ID3D12Resource* depth12 = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv = {};
        D3D12_GPU_DESCRIPTOR_HANDLE waterMask = {};
        if (depth && SUCCEEDED(on12->UnwrapUnderlyingResource(depth, queue, __uuidof(ID3D12Resource), (void**)&depth12)) && depth12)
        {
            m_gpu.Barrier(depth12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            uint32_t ds = m_gpu.AllocDescriptors(1);
            m_gpu.SetSRV(ds, depth12, DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
            depthSrv = m_gpu.GpuHandle(ds);
            uint32_t st = m_gpu.AllocDescriptors(1);
            m_gpu.SetSRV(st, depth12, DXGI_FORMAT_X24_TYPELESS_G8_UINT, 1);
            waterMask = m_gpu.GpuHandle(st);
        }

        // Chain source. With supersampling the render-size world surface must first be DOWNSAMPLED into a
        // native scene target (the fill: a sampled blit at the native viewport box-filters the larger source).
        // Without it the input IS the native backbuffer, so the first effect reads it directly and the
        // redundant 1:1 fill copy is skipped. bb12 moves to PIXEL_SHADER_RESOURCE (bbState) here and the final
        // resolve moves it to RENDER_TARGET; a supersample surface is restored to COMMON before it is returned.
        const bool needFill = (sceneInput != bb12);   // supersample present: downsample into scene[0] first
        D3D12_RESOURCE_STATES bbState = D3D12_RESOURCE_STATE_COMMON;
        if (sceneInput == bb12) bbState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_gpu.Barrier(sceneInput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        ID3D12Resource* chainSrc = sceneInput;   // resource the next effect reads
        int cur = -1;                            // scene target holding the current image (-1 = still the input)
        if (needFill)
        {
            sceneTo(0, D3D12_RESOURCE_STATE_RENDER_TARGET);
            uint32_t inSlot = m_gpu.AllocDescriptors(1);
            m_gpu.SetSRV(inSlot, sceneInput, fmt);
            m_gpu.DrawFullscreen(cmd, m_blitRootSig, m_blitPso, m_gpu.GpuHandle(inSlot), m_gpu.Rtv(m_scene[0], fmt), sceneW, sceneH);
            chainSrc = m_scene[0];
            cur = 0;
        }

        // Effect chain ping-pong over the two scene targets. Each enabled effect reads the current image and
        // writes the next; the first reads chainSrc (the backbuffer when there was no fill, else scene[0]).
        for (auto& e : m_effects)
        {
            if (!e->Enabled()) continue;
            int dst = (cur < 0) ? 0 : (1 - cur);

            if (cur >= 0) sceneTo(cur, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            sceneTo(dst, D3D12_RESOURCE_STATE_RENDER_TARGET);

            uint32_t slot = m_gpu.AllocDescriptors(1);
            m_gpu.SetSRV(slot, chainSrc, fmt);

            FrameContext ctx;
            ctx.cmd       = cmd;
            ctx.srcTex    = chainSrc;
            ctx.srcSrv    = m_gpu.GpuHandle(slot);
            ctx.depthSrv  = depthSrv;
            ctx.waterMask = waterMask;
            ctx.dstRtv    = m_gpu.Rtv(m_scene[dst], fmt);
            ctx.w = sceneW; ctx.h = sceneH; ctx.fmt = fmt;
            ctx.proj = proj;
            ctx.view = view;

            e->Render(m_gpu, ctx);
            cur = dst;
            chainSrc = m_scene[cur];
        }

        // Output: draw the final native scene into the backbuffer (1:1). cur >= 0 here -- either the fill ran
        // (supersample) or at least one effect ran (the no-effect + no-supersample case returned earlier). The
        // cur < 0 guard only fires on a supersample surface that failed to unwrap with nothing enabled: the
        // backbuffer already holds the image, so just normalize its state for the return.
        if (cur < 0)
        {
            if (bbState != D3D12_RESOURCE_STATE_COMMON)
                m_gpu.Barrier(bb12, bbState, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            sceneTo(cur, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_gpu.Barrier(bb12, bbState, D3D12_RESOURCE_STATE_RENDER_TARGET);

            uint32_t outSlot = m_gpu.AllocDescriptors(1);
            m_gpu.SetSRV(outSlot, m_scene[cur], fmt);
            // m_blitPsoBB is built for the backbuffer's sample count (N under engine MSAA), so this draw lands
            // on the multisampled backbuffer; the present then resolves it.
            m_gpu.DrawFullscreen(cmd, m_blitRootSig, m_blitPsoBB, m_gpu.GpuHandle(outSlot), m_gpu.Rtv(bb12, fmt), nativeW, nativeH);

            m_gpu.Barrier(bb12, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        }

        // Restore the borrowed / persistent surfaces to COMMON before handing the backbuffer back to On12. The
        // MSAA resolve target moved to PIXEL_SHADER_RESOURCE as the fill input above; reset it for next frame.
        if (resolveBB && m_msaaResolve)
            m_gpu.Barrier(m_msaaResolve, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (ss12)
            m_gpu.Barrier(ss12, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (depth12)
            m_gpu.Barrier(depth12, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

        ID3D12Fence* fence = nullptr;
        uint64_t val = 0;
        m_gpu.EndFrame(&fence, &val);

        // Hand every borrowed surface back to On12 with the completion fence, so the engine's later D3D9 use
        // (UI draw, present) waits for our writes.
        on12->ReturnUnderlyingResource(backbuffer, 1, &val, &fence);
        if (ss12)
        {
            on12->ReturnUnderlyingResource(superSample, 1, &val, &fence);
            ss12->Release();
        }
        if (depth12)
        {
            on12->ReturnUnderlyingResource(depth, 1, &val, &fence);
            depth12->Release();
        }
        bb12->Release();
    }
}
