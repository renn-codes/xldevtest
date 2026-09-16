// wxl-modern-render: this frame's readable-depth request.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

namespace wxl::scripts::render_modern
{
    // Nothing in the core reads this yet: OnWorldRenderEnd's depthSource is always null until a
    // future hook redirects the world's depth-stencil to a sampleable (INTZ) surface. This just
    // records the request so that hook, once it exists, knows whether to pay for one -- SSAO is the
    // only current subscriber, and it must already treat a null depth as "run without it" (see
    // AoEffect::Render), so setting this today changes nothing observable.
    inline bool g_readableDepthNeeded = false;

    inline void SetReadableDepthNeeded(bool needed) { g_readableDepthNeeded = needed; }
    inline bool ReadableDepthNeeded() { return g_readableDepthNeeded; }
}
