// wxl-render-modern grass: directional wind sway + blades parting around the player.
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

#include "grass/GrassSettings.hpp"

#include "core/Hook.hpp"
#include "core/Logger.hpp"
#include "events/EventScript.hpp"
#include "game/camera/Camera.hpp"
#include "game/gx/Gx.hpp"
#include "game/world/World.hpp"
#include "offsets/game/GroundEffect.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// windows.h aliases GetObject to GetObjectA, which collides with the world binding.
#undef GetObject

// Animates the engine's ground-effect (grass) vertices on the GPU, from the engine's own seams.
//
// The engine uploads the grass vertex constants c0..c22 from a static block once per visible
// chunk (kChunkConstantUpload); registers c14..c20 are unused by the live shader permutations.
// A detour on that upload writes the motion constants into the free block region once per frame
// (the engine then ships them with its own upload) and flags that the next draw is a grass draw,
// which lets the capture identify the engine grass vertex shader deterministically: the shader
// bound at that draw IS the grass shader. A replacement is assembled once from its disassembly
// with a motion block injected, and every later bind of the engine shader is substituted.
//
// Two motions are composed in the world XY plane, both anchored at the blade base by a height
// cone: a directional two-wave wind field (settings row ActiveWind) and a repulsion that parts
// blades around the player (settings row ActivePhysics).
namespace wxl::scripts::render_modern::grass
{
    namespace ev    = wxl::events;
    namespace cam   = wxl::game::camera;
    namespace world = wxl::game::world;
    namespace gxoff = wxl::game::gx::off;
    namespace geoff = wxl::offsets::game::groundeffect;

    // D3DAssemble / D3DDisassemble are exported by d3dcompiler_47.dll; D3DAssemble has no SDK
    // header declaration, so both are loaded by name.
    typedef HRESULT (WINAPI* PFN_D3DAssemble)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
        ID3DInclude*, UINT, ID3DBlob**, ID3DBlob**);
    typedef HRESULT (WINAPI* PFN_D3DDisassemble)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

    namespace
    {
        // Active settings rows (see GrassSettings.hpp for the future custom-DBC plan).
        WindSettings    g_wind    = { true, 45.0f, 6.0f, 0.12f, 18.0f, 0.05f, 6.5f, 35.0f, 0.35f, 0.6f, 0.015f, 0.3f };
        PhysicsSettings g_physics = { true, 2.0f, 0.5f, 0.0f, 0.5f, 0.3f };

        bool GrassMotionEnabled()
        {
            static int enabled = []() -> int {
                const char* env = std::getenv("WXL_GRASS_MOTION");
                if (env && (*env == '0' || *env == 'n' || *env == 'N')) return 0;
#pragma warning(suppress: 4996)
                FILE* flag = std::fopen("WarcraftXL_grass_motion.disable", "rb");
                if (!flag) return 1;
                std::fclose(flag);
                return 0;
            }();
            return enabled != 0;
        }

        IDirect3DVertexShader9* g_engineVS = nullptr; // the engine grass shader, once identified
        IDirect3DVertexShader9* g_ourVS    = nullptr; // the assembled replacement
        IDirect3DVertexShader9* g_failedVS = nullptr; // capture attempt failed; do not retry it
        bool g_bindPending = false;                   // a grass chunk upload ran; next draw is grass
        bool g_frameDirty  = true;                    // write the block constants once per frame
        bool g_playerValid = false;                   // player position read this frame

        using SetVertexShaderFn = long(__stdcall*)(void*, void*);
        using DIPFn             = long(__stdcall*)(void*, int, int, unsigned, unsigned, unsigned, unsigned);
        SetVertexShaderFn g_origSetVS = nullptr;
        DIPFn             g_origDIP   = nullptr;
        geoff::ChunkConstantUploadFn g_origChunkUpload = nullptr;

        constexpr float kEps    = 1.0e-3f; // guards 1/dist at the repulsion centre
        constexpr float kJitter = 1.7f;    // per-blade phase spread, radians
        constexpr float kTwoPi  = 6.2831853f;

        // Injected after the world position lands in view-space r0, before the projection. The
        // grass modelview 3x3 (c0/c1/c2) is the pure view rotation R (batches are unrotated), so
        // worldPos = R^T * r0 + cameraWorld. The world-plane offset is the sum of the player
        // repulsion and two travelling sine waves (primary + cross swell), rotated back into view
        // space. Both motions anchor the blade base:
        //  - repulsion weight = a height cone above the player's ground Z (repulsion only acts
        //    inside the radius, where that ground reference is valid);
        //  - wind weight = 1 - uv.y (the blade texture v runs 1 at the base to 0 at the tip), so
        //    it is exact per blade on any terrain, times an optional view-distance fade and a
        //    per-blade amplitude variance.
        // The grass shader uses r0/r1 only, so r2..r5 are free scratch. c13.y = 1.0 comes from
        // the engine shader's own def and survives in the copy the replacement is built from.
        // Constants live in the engine-free registers c14..c21:
        // c14 = cam.xyz, phase2 ; c15 = 1/coneHeight, 1/R, minHeight, eps ; c16 = player.xyz, lean ;
        // c17 = edge-center, center, R, phase1 ; c18 = A1.xy, k1.xy ; c19 = A2.xy, k2.xy ;
        // c20 = hashX, hashY, jitter, distanceFade ; c21 = ampVarBase, ampVarSpan, -anchor, 1/(1-anchor).
        const char* kMotionInject =
            "    dp3 r2.x, c0, r0\n"            // worldPos.x = R^T row0 . viewPos
            "    dp3 r2.y, c1, r0\n"            // worldPos.y
            "    add r2.xy, r2, c14\n"          // + cam.xy -> worldPos.xy
            "    mov r4.xy, r2\n"               // save worldPos.xy for the wind phases
            "    dp3 r3.z, c2, r0\n"            // worldPos.z = R^T row2 . viewPos
            "    add r3.z, r3.z, c14.z\n"       // + cam.z -> worldPos.z
            "    add r3.z, r3.z, -c16.z\n"      // height above the player's ground
            "    add r3.z, r3.z, -c15.z\n"      // - minHeight
            "    mul_sat r4.z, r3.z, c15.x\n"   // repulsion cone weight
            "    add r2.xy, r2, -c16\n"         // d = worldPos.xy - player.xy
            "    mul r2.z, r2.x, r2.x\n"
            "    mad r2.z, r2.y, r2.y, r2.z\n"  // dist^2
            "    add r2.z, r2.z, c15.w\n"       // + eps
            "    rsq r2.w, r2.z\n"              // invDist
            "    mul r3.x, r2.z, r2.w\n"        // dist
            "    mul_sat r3.y, r3.x, c15.y\n"   // t = saturate(dist/R)
            "    mad r3.y, c17.x, r3.y, c17.y\n"// strength = center + (edge-center)*t
            "    slt r3.z, r3.x, c17.z\n"       // inside = dist < R
            "    mul r3.y, r3.y, r3.z\n"        // strength *= inside
            "    mul r3.y, r3.y, r4.z\n"        // * cone weight
            "    mul r3.y, r3.y, r2.w\n"        // * invDist (d * this = unit dir * magnitude)
            "    mul r2.xy, r2, r3.y\n"         // repulsion offset (world)
            "    add r5.z, c13.y, -v3.y\n"      // wind weight = 1 - uv.y (0 at the blade base)
            "    add r5.z, r5.z, c21.z\n"       // - anchor: the base band never moves
            "    mul_sat r5.z, r5.z, c21.w\n"   // renormalize over the moving span, clamped
            "    mul r5.z, r5.z, r5.z\n"        // squared: bending profile, stiff base to loose tip
            "    mov r5.w, c13.y\n"             // 1 (an instruction reads at most one c# register)
            "    mad r5.w, r0.z, c20.w, r5.w\n" // 1 + viewZ * distanceFade
            "    rcp r5.w, r5.w\n"
            "    mul r5.z, r5.z, r5.w\n"        // wind weight *= view-distance fade
            "    mul r3.x, r4.x, c20.x\n"       // per-blade hash from worldPos
            "    mad r3.x, r4.y, c20.y, r3.x\n"
            "    frc r3.x, r3.x\n"              // rand in [0,1)
            "    mad r2.z, r3.x, c21.y, c21.x\n"// amplitude scale = base + span*rand
            "    mul r5.z, r5.z, r2.z\n"        // wind weight *= amplitude variance
            "    mul r3.x, r3.x, c20.z\n"       // jitter angle
            "    mul r3.y, r4.x, c18.z\n"
            "    mad r3.y, r4.y, c18.w, r3.y\n" // worldPos . k1
            "    add r3.y, r3.y, r3.x\n"        // + jitter
            "    add r3.y, r3.y, c17.w\n"       // + phase1 -> angle1
            "    mul r3.z, r4.x, c19.z\n"
            "    mad r3.z, r4.y, c19.w, r3.z\n" // worldPos . k2
            "    add r3.z, r3.z, r3.x\n"        // + jitter
            "    add r3.z, r3.z, c14.w\n"       // + phase2 -> angle2
            "    sincos r4.xy, r3.y\n"          // r4.y = sin(angle1) (worldPos.xy consumed)
            "    add r4.w, r4.y, c16.w\n"       // + lean (constant downwind bias)
            "    sincos r5.xy, r3.z\n"          // r5.y = sin(angle2); r5.zw untouched
            "    mul r3.zw, c18.xxxy, r4.wwww\n"     // A1 * (sin1 + lean)
            "    mad r3.zw, c19.xxxy, r5.yyyy, r3\n" // + A2 * sin2
            "    mul r3.zw, r3, r5.zzzz\n"           // * wind weight
            "    add r2.xy, r2, r3.zwzw\n"      // total world offset = repulsion + wind
            "    mul r3.xyz, c0, r2.x\n"        // rotate back into view: R * offset
            "    mad r3.xyz, c1, r2.y, r3.xyz\n"
            "    add r0.xyz, r0, r3\n";         // viewPos += view-space offset

        HMODULE Compiler()
        {
            HMODULE d = GetModuleHandleA("d3dcompiler_47.dll");
            return d ? d : LoadLibraryA("d3dcompiler_47.dll");
        }

        /**
         * @brief Disassembles a vertex shader to text.
         * @param vs  the shader to disassemble.
         * @return the disassembly blob (caller releases), or null on failure.
         */
        ID3DBlob* Disassemble(IDirect3DVertexShader9* vs)
        {
            auto disasm = reinterpret_cast<PFN_D3DDisassemble>(GetProcAddress(Compiler(), "D3DDisassemble"));
            if (!disasm) return nullptr;

            UINT size = 0;
            vs->GetFunction(nullptr, &size);
            if (!size) return nullptr;
            void* code = malloc(size);
            if (!code) return nullptr;
            vs->GetFunction(code, &size);

            ID3DBlob* text = nullptr;
            HRESULT hr = disasm(code, size, 0, nullptr, &text);
            free(code);
            return SUCCEEDED(hr) ? text : nullptr;
        }

        /**
         * @brief Assembles the replacement: the engine grass disassembly plus the motion block.
         * @param device    the live device to create the shader on.
         * @param engineVS  the engine grass shader identified at draw time.
         * @return true when the replacement is ready.
         */
        bool BuildReplacement(IDirect3DDevice9* device, IDirect3DVertexShader9* engineVS)
        {
            ID3DBlob* text = Disassemble(engineVS);
            if (!text) return false;

            std::string src(static_cast<const char*>(text->GetBufferPointer()));
            text->Release();

            size_t at = src.find("add r0, r0, c3");
            if (at == std::string::npos) { WLOG_WARN("grass: injection anchor missing"); return false; }
            at = src.find('\n', at);
            if (at == std::string::npos) return false;
            src.insert(at + 1, kMotionInject);

            auto assemble = reinterpret_cast<PFN_D3DAssemble>(GetProcAddress(Compiler(), "D3DAssemble"));
            if (!assemble) { WLOG_WARN("grass: D3DAssemble unavailable"); return false; }

            ID3DBlob* code = nullptr;
            ID3DBlob* error = nullptr;
            HRESULT hr = assemble(src.c_str(), src.size(), "grassMotion", nullptr, nullptr, 0, &code, &error);
            if (FAILED(hr) || !code)
            {
                WLOG_WARN("grass: assemble failed: %s",
                    error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
                if (error) error->Release();
                return false;
            }
            if (error) error->Release();

            IDirect3DVertexShader9* built = nullptr;
            hr = device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &built);
            code->Release();
            if (FAILED(hr) || !built) { WLOG_WARN("grass: CreateVertexShader failed"); return false; }

            if (g_ourVS) g_ourVS->Release();
            g_ourVS = built;
            g_engineVS = engineVS;
            WLOG_INFO("grass: motion shader ready (engine=%p ours=%p)", engineVS, g_ourVS);
            return true;
        }

        /**
         * @brief Reads the player world position.
         * @param out  receives x, y, z.
         * @return true when a player object is resident.
         */
        bool ReadPlayer(float out[3])
        {
            unsigned long long guid = world::ActivePlayerGuid();
            if (!guid) return false;
            void* unit = world::ResolveObject(guid, world::kTypeMaskPlayer);
            if (!unit) return false;
            world::UnitPosition(unit, out);
            return true;
        }

        /**
         * @brief Writes the motion constants into the engine's constant block (c14..c20 region).
         *
         * The engine memsets the block once per grass pass and uploads it per chunk, so one write
         * per frame (at the first chunk upload, after the memset) reaches every grass draw with
         * no extra device calls.
         */
        void WriteFrameConstants()
        {
            float camPos[3];
            cam::Position(camPos);

            float player[3];
            g_playerValid = ReadPlayer(player);
            if (!g_playerValid) return;

            const WindSettings&    w = g_wind;
            const PhysicsSettings& p = g_physics;

            // Wave vectors and per-frame phases; the waves travel along their heading at w.speed.
            // The clock wraps at 1000s to keep the phase precise.
            const float t   = (GetTickCount() % 1000000u) * 0.001f;
            const float a1  = w.directionDeg * (kTwoPi / 360.0f);
            const float a2  = (w.directionDeg + w.crossAngleDeg) * (kTwoPi / 360.0f);
            const float k1  = kTwoPi / (w.wavelength      > 0.1f ? w.wavelength      : 0.1f);
            const float k2  = kTwoPi / (w.crossWavelength > 0.1f ? w.crossWavelength : 0.1f);
            const float d1[2] = { cosf(a1), sinf(a1) };
            const float d2[2] = { cosf(a2), sinf(a2) };
            const float amp1  = w.enabled ? w.amplitude      : 0.0f;
            const float amp2  = w.enabled ? w.crossAmplitude : 0.0f;
            const float lean  = w.enabled ? w.lean           : 0.0f;
            // The cross swell travels at a different speed so the interference pattern evolves.
            const float phase1 = -fmodf(k1 * w.speed * t, kTwoPi);
            const float phase2 = -fmodf(k2 * w.speed * 1.37f * t, kTwoPi);

            const float center = p.enabled ? p.forceCenter : 0.0f;
            const float edge   = p.enabled ? p.forceEdge   : 0.0f;
            const float coneH  = p.coneHeight > 0.01f ? p.coneHeight : 0.01f;
            const float radius = p.radius     > 0.01f ? p.radius     : 0.01f;

            // Per-blade amplitude spread: variance v maps a [0,1) hash to [1 - v/2, 1 + v/2].
            float var = w.variance;
            if (var < 0.0f) var = 0.0f;
            if (var > 1.0f) var = 1.0f;

            // Base anchor: the [0, anchor] band of the blade texture stays still; the weight is
            // renormalized over the remaining span.
            float anchor = w.anchor;
            if (anchor < 0.0f) anchor = 0.0f;
            if (anchor > 0.9f) anchor = 0.9f;

            const float c[8][4] = {
                { camPos[0], camPos[1], camPos[2], phase2 },                    // c14
                { 1.0f / coneH, 1.0f / radius, p.minHeight, kEps },             // c15
                { player[0], player[1], player[2], lean },                      // c16
                { edge - center, center, radius, phase1 },                      // c17
                { d1[0] * amp1, d1[1] * amp1, d1[0] * k1, d1[1] * k1 },         // c18
                { d2[0] * amp2, d2[1] * amp2, d2[0] * k2, d2[1] * k2 },         // c19
                { 0.737f, 1.311f, kJitter, w.distanceFade > 0.0f ? w.distanceFade : 0.0f }, // c20
                { 1.0f - var * 0.5f, var, -anchor, 1.0f / (1.0f - anchor) },    // c21
            };
            float* block = reinterpret_cast<float*>(geoff::kVsConstantBlock) + geoff::kVsFirstFreeReg * 4;
            memcpy(block, c, sizeof(c));
        }

        /**
         * @brief Detours the per-chunk grass constant upload: refreshes the motion constants once
         *        per frame before the engine ships the block, and flags the next draw as grass.
         * @param mtx    the chunk translation matrix.
         * @param group  the shader permutation group.
         */
        void __cdecl hkChunkUpload(const float* mtx, int group)
        {
            if (g_frameDirty)
            {
                g_frameDirty = false;
                WriteFrameConstants();
            }
            g_bindPending = true;
            g_origChunkUpload(mtx, group);
        }

        /**
         * @brief Detours SetVertexShader: substitutes the identified grass shader at bind time.
         * @param dev     the live device.
         * @param shader  the shader being bound.
         * @return the SetVertexShader result.
         */
        long __stdcall hkSetVertexShader(void* dev, void* shader)
        {
            if (shader && shader == g_engineVS && g_ourVS &&
                g_playerValid && (g_wind.enabled || g_physics.enabled))
                return g_origSetVS(dev, g_ourVS);
            return g_origSetVS(dev, shader);
        }

        // Grass blade vertex size; gates the capture to grass geometry (a chunk group can upload
        // constants without drawing any blade, letting an unrelated draw arrive first).
        constexpr UINT kGrassVertexStride = 36;

        /**
         * @brief Detours DrawIndexedPrimitive: on the grass-stride draw following a grass chunk
         *        upload, the bound vertex shader IS the grass shader; captures it when it is not
         *        ours yet.
         * @param dev  the live device; remaining parameters are the native draw arguments.
         * @return the DrawIndexedPrimitive result.
         */
        long __stdcall hkDIP(void* dev, int pt, int bv, unsigned mi, unsigned nv, unsigned si, unsigned pc)
        {
            if (g_bindPending)
            {
                IDirect3DDevice9* device = static_cast<IDirect3DDevice9*>(dev);
                IDirect3DVertexBuffer9* vb = nullptr;
                UINT vbOffset = 0, stride = 0;
                if (SUCCEEDED(device->GetStreamSource(0, &vb, &vbOffset, &stride)))
                {
                    if (vb) vb->Release();
                    if (stride == kGrassVertexStride)
                    {
                        g_bindPending = false;
                        IDirect3DVertexShader9* vs = nullptr;
                        if (SUCCEEDED(device->GetVertexShader(&vs)) && vs)
                        {
                            // A new engine shader here means first capture or a shadow-variant switch.
                            if (vs != g_ourVS && vs != g_engineVS && vs != g_failedVS)
                                g_failedVS = BuildReplacement(device, vs) ? nullptr : vs;
                            vs->Release();
                        }
                    }
                }
            }
            return g_origDIP(dev, pt, bv, mi, nv, si, pc);
        }

        /**
         * @brief Swaps one device vtable slot to a detour, keeping the previous entry as the chain.
         * @param vtbl  the device vtable.
         * @param idx   the slot index.
         * @param hook  the detour to install.
         * @param orig  receives the previous entry.
         * @return true when the slot now points at the detour.
         */
        bool SwapSlot(void** vtbl, unsigned idx, void* hook, void** orig)
        {
            if (vtbl[idx] == hook) return true;
            DWORD prot = 0;
            if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) return false;
            *orig = vtbl[idx];
            vtbl[idx] = hook;
            VirtualProtect(&vtbl[idx], sizeof(void*), prot, &prot);
            return true;
        }

        /**
         * @brief Installs the engine detour and the device vtable detours (idempotent).
         * @param dev  the live device.
         */
        void EnsureHooks(void* dev)
        {
            if (!GrassMotionEnabled()) return;

            if (!g_origChunkUpload)
            {
                if (wxl::core::hook::Install("GrassChunkUpload", geoff::kChunkConstantUpload,
                                             reinterpret_cast<void*>(&hkChunkUpload),
                                             reinterpret_cast<void**>(&g_origChunkUpload)))
                    wxl::core::hook::EnableAll();
            }

            if (!dev) return;
            void** vtbl = *reinterpret_cast<void***>(dev);
            if (vtbl[gxoff::vt::kSetVertexShader] == reinterpret_cast<void*>(&hkSetVertexShader)) return;

            bool ok = SwapSlot(vtbl, gxoff::vt::kSetVertexShader,
                               reinterpret_cast<void*>(&hkSetVertexShader),
                               reinterpret_cast<void**>(&g_origSetVS));
            ok &= SwapSlot(vtbl, gxoff::vt::kDrawIndexedPrimitive,
                           reinterpret_cast<void*>(&hkDIP),
                           reinterpret_cast<void**>(&g_origDIP));
            if (ok) WLOG_INFO("grass: device hooks installed");
        }

        /** @brief Drops the capture so the next grass draws re-identify the engine shader. */
        void ResetCapture()
        {
            if (g_ourVS) { g_ourVS->Release(); g_ourVS = nullptr; }
            g_engineVS = nullptr;
            g_failedVS = nullptr;
            g_bindPending = false;
            g_playerValid = false;
        }
    }

    const WindSettings&    ActiveWind()    { return g_wind; }
    const PhysicsSettings& ActivePhysics() { return g_physics; }

    /** @brief Keeps the hooks installed and re-arms the per-frame constant refresh. */
    class GrassMotion : public ev::EventScript
    {
    public:
        GrassMotion()
        {
            on<&GrassMotion::OnEndScene>(ev::Event::OnEndScene);
            on<&GrassMotion::OnDeviceReset>(ev::Event::OnDeviceReset);
            WLOG_INFO("wxl-render-modern: grass motion %s", GrassMotionEnabled() ? "loaded" : "disabled");
        }

    private:
        /**
         * @brief Installs the hooks once the device exists and re-arms the constant refresh.
         * @param a  end-scene args carrying the live device.
         */
        void OnEndScene(const ev::EndSceneArgs& a)
        {
            EnsureHooks(a.device);
            g_frameDirty = true;
        }

        /** @brief Drops the captured shaders after a device reset; they are re-identified live. */
        void OnDeviceReset(const ev::DeviceResetArgs&)
        {
            ResetCapture();
        }
    };

    // File-scope instance self-registers its handlers at DLL load via the EventScript ctor.
    GrassMotion g_grassMotion;
}
