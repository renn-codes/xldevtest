// wxl-glue-zoom — v180 PRODUCTION: scroll-wheel head zoom on the character-create screen
// Per-race calibrated table (20 race/gender pairs, visually tuned at max zoom):
//   maxZoom = FOV zoom endpoint, scale = framing correction. No formula for known
//   races (exact table); formula fallback for unlisted models.
// Runtime:
//   wheel -> t (smoothed, frame-rate independent)
//   zoom  = 1 + (maxZoom-1)*t            (cam+0x114 FOV, VERIFIED)
//   scale = (1+(Z*S-1)t)/(1+(Z-1)t)      (perceived zoom*scale linear in t)
//   placement = rotBase * scale          (single writer at DIP, feet-anchored)
// Only active on the character-create screen (frame global [0xB6B184]).

#include <wxl/PluginApi.h>
#include "engine/events/Event.hpp"
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <windows.h>
#include <intrin.h>

static const WXL_Api* g_api = nullptr;
static const char*    kTag  = "GlueZoom";

static constexpr uintptr_t kDrawTriBatch = 0x008203B0;
static constexpr uintptr_t kSetupWProj   = 0x004BECF0;
static constexpr uintptr_t kGetSphere    = 0x0082C2C0;
static constexpr uintptr_t kDeviceGlobal = 0x00C5DF88;
static constexpr uintptr_t kD3DDevOff    = 0x397C;
static constexpr uintptr_t kCharCreateFrm = 0x00B6B184;   // charcreate frame global
static constexpr uintptr_t kCharSelectFrm = 0x00B6B1FC;   // charselect frame global (VERIFIED)
static constexpr uint32_t  kDipSlot      = 82;            // IDirect3DDevice9::DrawIndexedPrimitive

enum { kOffCtxInstance = 0x60 };
enum { kOffInstModel = 0x2C };
enum { kOffModelHeader = 0x150 };
enum { kOffModelSkin = 0x170 };
enum { kOffInstPlacement = 0xB4 };
enum { kOffHdrAttachCount = 0xF0, kOffHdrAttachPtr = 0xF4, kAttachStride = 0x28 };
enum { kOffCamFov = 0x114 };
enum { kWheelNotch = 120 };

using FnDrawTri = void(__fastcall*)(void* ctx, void* edx);
using FnSetup   = void(__thiscall*)(void* cam, void* vb, int flag);
using FnSphere  = void(__thiscall*)(void* inst, float out[4]);
using FnDIP     = long(__stdcall*)(void* dev, int pt, int bv, unsigned mi, unsigned nv, unsigned si, unsigned pc);

static FnDrawTri  g_origDraw  = nullptr;
static FnSetup    g_origSetup = nullptr;
static FnSphere   g_getSphere = nullptr;
static FnDIP      g_origDIP   = nullptr;
static void*      g_hookedDev = nullptr;

static void* s_inst = nullptr;

static float s_t = 0.0f;
static float s_tGoal = 0.0f;     // wheel target; s_t eases toward it (smooth glide)
static float s_zoom = 1.0f;

// ---- calibrated per-race table (user ground truth, max-zoom visual calibration) ----
struct RaceCal { const char* name; float headH; float maxZoom; float scale; };
static const RaceCal kRaceCal[] = {
    { "humanmale_hd.m2",     1.8967f, 4.050f, 0.868f },
    { "humanfemale_hd.m2",   1.7917f, 4.343f, 0.899f },
    { "dwarfmale_hd.m2",     1.4360f, 3.424f, 1.175f },
    { "dwarffemale_hd.m2",   1.3794f, 3.240f, 1.167f },
    { "gnomemale_hd.m2",     0.9921f, 3.105f, 1.548f },
    { "gnomefemale_hd.m2",   0.9267f, 3.240f, 1.557f },
    { "nightelfmale_hd.m2",  2.2970f, 5.423f, 0.689f },
    { "nightelffemale_hd.m2", 2.1387f, 5.364f, 0.737f },
    { "orcmale_hd.m2",       2.1453f, 4.206f, 0.723f },
    { "orcfemale_hd.m2",     1.9092f, 5.396f, 0.834f },
    { "scourgemale_hd.m2",   1.9130f, 4.801f, 0.950f },
    { "scourgefemale_hd.m2", 1.7847f, 4.802f, 0.956f },
    { "taurenmale_hd.m2",    2.5169f, 4.200f, 0.818f },
    { "taurenfemale_hd.m2",  2.4491f, 5.282f, 0.770f },
    { "trollmale_hd.m2",     2.7010f, 4.584f, 0.708f },
    { "trollfemale_hd.m2",   2.2615f, 5.341f, 0.705f },
    { "bloodelfmale_hd.m2",  1.9542f, 4.681f, 0.846f },
    { "bloodelffemale_hd.m2", 1.8157f, 4.766f, 0.908f },
    { "draeneimale_hd.m2",   2.3272f, 4.595f, 0.678f },
    { "draeneifemale_hd.m2",  2.2129f, 5.029f, 0.703f },
};
static constexpr int kRaceCalCount = sizeof(kRaceCal) / sizeof(kRaceCal[0]);

// formula fallback for unlisted models (old calibrated fit)
static constexpr float kRefHead = 1.81f;
static constexpr float kRefZ    = 3.0f;
static constexpr float kScaleCap = 3.0f;
static constexpr float kScaleMin = 0.20f;

static uint32_t s_nativeNameHash = 0;
static float s_nativeRadius = 0.0f;

static float s_raceMaxZoom = 3.0f;
static float s_raceScale   = 1.0f;

static float s_rotBase[16];
static float s_lastWritten[16];

// active only on the character-create screen.
//
// v181 used [frame + 0x2A0] (frame's model field) as the screen signal — WRONG: the
// field persists after leaving the screen, so the gate stayed open in the world and
// the module scaled the world player's placement -> invisible/flicker.
// v183 used [frame + 0x2A4] (frame's camera handle) — ALSO persists in world on this
// client (the "freed on screen leave" note does not hold for the charcreate frame).
//
// v184 DEFINITIVE gate — content-based, immune to every stale global:
//   A world unit's placement matrix carries its world coordinates in the TRANSLATION
//   row (pl[12..14] != 0). A glue character sits at IDENTITY placement (translation
//   ~0) — VERIFIED in the v96-117 era ("placement IDENTITY on charselect/charcreate").
//   So "translation ~ 0 AND attachment 29" can only be true for the glue character.
//   Even if every frame/camera global is stale, a world player can NEVER pass it.
//   The FOV hook additionally restricts SetupWorldProjection to exactly the charcreate
//   camera; and a placement write is refused unless the current translation is ~0.

static bool HasAttachment29(void* model);

// the charcreate camera handle at [frame + 0x2A4] — the ACTIVE camera signal.
// Both glue screens (charselect [0xB6B1FC], charcreate [0xB6B184]) show character M2s
// at identity placement, so the placement gate alone cannot tell them apart. The
// cameras are DIFFERENT handles per frame (VERIFIED Camera.yaml), so we carry which
// camera is active from SetupWorldProjection: the FOV hook sees the camera each call
// and sets s_onCharCreate = (cam == charcreate camera). World/charselect cameras
// clear it, so placement writes only happen on the charcreate screen.
static void* CharCreateCamera()
{
    __try {
        void* frm = *(void**)kCharCreateFrm;
        if (!frm) return nullptr;
        return *(void**)((char*)frm + 0x2A4);
    } __except(1) { return nullptr; }
}

static bool s_onCharCreate = false;   // updated every SetupWorldProjection call

// placement is (approximately) identity -> a glue character, not a world unit
static bool IsGluePlacement(const float* pl)
{
    return pl[12] > -0.01f && pl[12] < 0.01f
        && pl[13] > -0.01f && pl[13] < 0.01f
        && pl[14] > -0.01f && pl[14] < 0.01f;
}

static bool IsCharCreateModel(void* model)
{
    if (!model || !s_onCharCreate) return false;   // must be on the charcreate screen
    return HasAttachment29(model);                 // must be a character M2
}

static uint32_t HashModelName(void* model)
{
    uint32_t h = 2166136261u;
    __try {
        const char* n = *(const char**)((char*)model + 0x140);
        if (n) for (const char* p = n; *p && p - n < 64; ++p) {
            h ^= (uint8_t)*p; h *= 16777619u;
        }
    } __except(1) {}
    return h;
}

static void ApplyScale(float* m, float s)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r*4 + c] *= s;
}

static bool HasAttachment29(void* model)
{
    __try {
        void* hdr = *(void**)((char*)model + kOffModelHeader);
        if (!hdr || *(uint32_t*)hdr != 0x3032444D) return false;
        uint32_t attCnt = *(uint32_t*)((char*)hdr + kOffHdrAttachCount);
        uint32_t* attPtr = *(uint32_t**)((char*)hdr + kOffHdrAttachPtr);
        if (!attPtr || !attCnt || attCnt > 512) return false;
        for (uint32_t i = 0; i < attCnt; ++i)
            if (*(const uint32_t*)((const uint8_t*)attPtr + i*kAttachStride) == 29) return true;
    } __except(1) {}
    return false;
}

static float MeasureHeadHeight(void* model)
{
    __try {
        uint8_t* skin = *(uint8_t**)((char*)model + kOffModelSkin);
        if (!skin) return 0.0f;
        uint32_t cnt = *(uint32_t*)(skin + 0x1C);
        uint8_t* subs = *(uint8_t**)(skin + 0x20);
        if (!subs || cnt > 1024) return 0.0f;
        for (uint32_t i = 0; i < cnt; ++i) {
            uint8_t* s = subs + i * 0x30;
            if (*(uint16_t*)(s + 0x00) == 1702) {
                float* ctr = (float*)(s + 0x14);
                float h = ctr[2];
                return (h > 0.5f && h < 5.f) ? h : 0.0f;
            }
        }
    } __except(1) {}
    return 0.0f;
}

// resolve this race's (maxZoom, scale) from the table; formula fallback
static void ResolveRace(void* model, float headH)
{
    const char* name = "";
    __try {
        const char* n = *(const char**)((char*)model + 0x140);
        if (n) name = n;
    } __except(1) {}
    for (int i = 0; i < kRaceCalCount; ++i) {
        if (name && strncmp(kRaceCal[i].name, name, 32) == 0) {
            s_raceMaxZoom = kRaceCal[i].maxZoom;
            s_raceScale   = kRaceCal[i].scale;
            return;
        }
    }
    // formula fallback
    float nr = s_nativeRadius;
    if (!(nr > 0.05f && nr < 50.f)) nr = 1.128f;
    float f = nr / 1.128f;
    if (f < 0.85f) f = 0.85f;
    if (f > 1.5f) f = 1.5f;
    s_raceMaxZoom = 1.0f + 2.0f * f;
    float x = (headH > 0.5f ? (headH - kRefHead) / kRefHead : 0.0f);
    float z = (s_raceMaxZoom - kRefZ) / kRefZ;
    float ts = 0.9415f - 0.8468f*x + 0.1892f*x*x - 0.7637f*z + 3.6116f*x*z;
    if (ts < kScaleMin) ts = kScaleMin;
    if (ts > kScaleCap) ts = kScaleCap;
    s_raceScale = ts;
}

static void ResetForRace(void* inst, void* model)
{
    s_nativeNameHash = HashModelName(model);
    for (int i = 0; i < 16; ++i) s_rotBase[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    for (int i = 0; i < 16; ++i) s_lastWritten[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    s_nativeRadius = 0.0f;
    s_t = 0.0f;
    s_tGoal = 0.0f;
    s_zoom = 1.0f;
    if (g_getSphere) {
        float sph[4] = {0,0,0,0};
        __try { g_getSphere(inst, sph); } __except(1) {}
        s_nativeRadius = sph[3];
    }
}

static bool IsDragging() { return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; }

// scale that keeps the PERCEIVED zoom linear: the eye sees zoom(t)*scale(t). With
// both linear in t the product is quadratic -> for scale-DOWN races (tall: NE,
// Draenei) the scale lags early then surges. Fix: product(t) = 1+(Z*S-1)*t linear,
// so scale(t) = product/zoom = (1+(Z*S-1)t)/(1+(Z-1)t). At t=1 -> S exactly.
static float ScaleAt(float z, float s, float t)
{
    if (t <= 0.f) return 1.0f;
    if (t >= 1.f) return s;
    float zoom = 1.0f + (z - 1.0f) * t;
    if (zoom < 0.01f) zoom = 0.01f;
    float prod = 1.0f + (z * s - 1.0f) * t;
    float sc = prod / zoom;
    if (sc < 0.05f) sc = 0.05f;
    if (sc > 5.0f) sc = 5.0f;
    return sc;
}

static bool EngineChangedMatrix(const float* pl)
{
    for (int i = 0; i < 16; ++i) {
        float d = pl[i] - s_lastWritten[i];
        if (d > 0.0005f || d < -0.0005f) return true;
    }
    return false;
}

static void __fastcall HookedDrawTri(void* ctx, void* edx)
{
    if (ctx)
    {
        void* inst = nullptr;
        __try { inst = *(void**)((char*)ctx + kOffCtxInstance); } __except(1) {}
        if (inst) {
            void* model = nullptr;
            __try { model = *(void**)((char*)inst + kOffInstModel); } __except(1) {}
            if (model && IsCharCreateModel(model)) {
                float* pl = (float*)((char*)inst + kOffInstPlacement);
                if (!IsGluePlacement(pl)) goto drawnotchar;   // world unit: NEVER touch
                s_inst = inst;
                if (HashModelName(model) != s_nativeNameHash) {
                    ResetForRace(inst, model);
                    ResolveRace(model, MeasureHeadHeight(model));
                }
            }
        }
    }
    drawnotchar:
    if (g_origDraw) g_origDraw(ctx, edx);
}

static void __fastcall HookedSetupWProj(void* cam, void* edx, void* vb, int flag)
{
    // which camera is active this render? charcreate camera -> on charcreate.
    s_onCharCreate = (cam != nullptr) && (cam == CharCreateCamera());
    if (s_onCharCreate) {
        static bool s_baseSet = false;
        static float s_baseFov = 0.0f;
        if (!s_baseSet) { __try { s_baseFov = *(float*)((char*)cam + kOffCamFov); s_baseSet = true; } __except(1) {} }
        if (s_baseSet && s_baseFov > 0.01f && s_baseFov < 3.f) {
            s_zoom = 1.0f + (s_raceMaxZoom - 1.0f) * s_t;
            if (s_zoom < 1.0f) s_zoom = 1.0f;
            __try { *(float*)((char*)cam + kOffCamFov) = s_baseFov / s_zoom; } __except(1) {}
        }
    }
    if (g_origSetup) g_origSetup(cam, vb, flag);
}

// DIP hook: the ONLY placement writer. Writes at the GPU seam, after all engine writes
// (single writer -> no DrawTri-vs-engine timing fight, no shake). rotBase recaptured
// only while dragging (engine writes rotation between DrawTri and DIP).
static long __stdcall HookedDIP(void* dev, int pt, int bv, unsigned mi, unsigned nv, unsigned si, unsigned pc)
{
    // triple gate: charcreate camera live AND tracked instance valid AND glue placement
    if (s_inst && s_onCharCreate && CharCreateCamera()) {
        __try {
            void* model = *(void**)((char*)s_inst + kOffInstModel);
            if (!model || !IsCharCreateModel(model)) goto skip;
            float* pl = (float*)((char*)s_inst + kOffInstPlacement);
            if (!IsGluePlacement(pl)) goto skip;   // world unit: NEVER touch

            if (IsDragging() && EngineChangedMatrix(pl)) {
                for (int i = 0; i < 16; ++i) s_rotBase[i] = pl[i];
            }

            for (int i = 0; i < 16; ++i) pl[i] = s_rotBase[i];
            float scale = ScaleAt(s_raceMaxZoom, s_raceScale, s_t);
            ApplyScale(pl, scale);
            for (int i = 0; i < 16; ++i) s_lastWritten[i] = pl[i];
        } __except(1) { s_inst = nullptr; }
    }
    skip:
    return g_origDIP(dev, pt, bv, mi, nv, si, pc);
}

static void EnsureDIPHook()
{
    __try {
        void* gx = *(void**)kDeviceGlobal;
        if (!gx) return;
        void* d3d = *(void**)((char*)gx + kD3DDevOff);
        if (!d3d) return;
        if (g_hookedDev == d3d) return;
        void** vt = *(void***)d3d;
        if (vt[kDipSlot] != reinterpret_cast<void*>(&HookedDIP)) {
            g_origDIP = (FnDIP)vt[kDipSlot];
            vt[kDipSlot] = reinterpret_cast<void*>(&HookedDIP);
        }
        g_hookedDev = d3d;
    } __except(1) {}
}

static void __cdecl OnInput(void* user, const void* args)
{
    if (!CharCreateCamera()) return;
    const auto* a = static_cast<const wxl::events::InputArgs*>(args);
    if (!a || a->message != WM_MOUSEWHEEL) return;
    short d = (short)HIWORD(a->wparam);
    if (!d) return;
    s_tGoal += (float)d / (float)kWheelNotch * 0.10f;
    if (s_tGoal < 0.f) s_tGoal = 0.f;
    if (s_tGoal > 1.f) s_tGoal = 1.f;
}

static void __cdecl OnFrame(void* user, const void* args)
{
    // Not on charcreate (camera handle gone or a different camera is active):
    // make the module fully inert — no placement writes anywhere outside charcreate.
    if (!CharCreateCamera()) {
        s_onCharCreate = false;
        s_inst = nullptr;
        return;
    }
    // ease t toward the wheel goal (frame-rate independent exponential smoothing):
    // a wheel notch lands as a ~0.2s glide, not a jump
    static DWORD lastTick = 0;
    DWORD now = GetTickCount();
    float dt = (lastTick ? (float)(now - lastTick) : 16.0f) / 1000.0f;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.1f) dt = 0.1f;
    lastTick = now;
    const float kZoomSmooth = 9.0f;
    float f = 1.0f - expf(-kZoomSmooth * dt);
    s_t += (s_tGoal - s_t) * f;
    if (s_t < 0.f) s_t = 0.f;
    if (s_t > 1.f) s_t = 1.f;
}

static void Install()
{
    g_api->HookAttach("Z:DrawTri", kDrawTriBatch, reinterpret_cast<void*>(&HookedDrawTri), reinterpret_cast<void**>(&g_origDraw), 0);
    g_api->HookAttach("Z:SetupWProj", kSetupWProj, reinterpret_cast<void*>(&HookedSetupWProj), reinterpret_cast<void**>(&g_origSetup), 0);
    g_getSphere = (FnSphere)kGetSphere;
    EnsureDIPHook();
    g_api->Subscribe(uint32_t(wxl::events::Event::OnInput), OnInput, nullptr);
    g_api->Subscribe(uint32_t(wxl::events::Event::OnFrame), OnFrame, nullptr);
    g_api->Log(WXL_LOG_INFO, kTag, "v180 charcreate head zoom: %d calibrated races", kRaceCalCount);
}

extern "C" __declspec(dllexport)
const WXL_PluginInfo* __cdecl WXL_Query(void) {
    static const WXL_PluginInfo info{ sizeof(WXL_PluginInfo), WXL_API_VERSION, "GlueZoom", 180, WXL_CLIENT_BUILD };
    return &info;
}

extern "C" __declspec(dllexport)
int __cdecl WXL_Load(const WXL_Api* api) {
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;
    g_api = api;
    Install();
    return 1;
}