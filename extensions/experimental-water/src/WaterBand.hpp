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

#pragma once

#include "wxl/EventScript.hpp"
#include "game/Gx.hpp"

// Fakes a water reflection with zero depth/mask information: copies the already-rendered scene,
// flips it vertically, and blends it back in over a fixed band near the bottom of the screen.
// Runs on OnWorldRenderEnd -- the world->UI boundary -- instead of OnEndScene, so the scene is
// captured BEFORE the UI pass draws. Health bars, minimap, chat etc. are composited on top
// afterward, untouched. The trade-off versus a real masked reflection is unchanged: it tints
// whatever is in that screen band, water or not. Tune bandStart/maxOpacity to fit how your camera
// typically frames water in practice.
namespace wxl::scripts::waterband
{
    class WaterBand final : public wxl::ext::EventScript
    {
    public:
        WaterBand();  // binds the event handler

        // Tunable at runtime.
        float bandStart   = 0.5f;   // screen-space v where the blend starts ramping in (0 top, 1 bottom)
        float maxOpacity  = 0.45f;  // opacity of the flipped sample at the very bottom of the screen
        float flipBias    = 0.0f;   // nudge the mirror point if the horizon doesn't line up

    private:
        void OnWorldRenderEnd(const events::WorldRenderEndArgs& a);

        bool EnsureResources(game::gx::Device9 dev); // compile shader + create scratch RT, once
        void BandPass(game::gx::Device9 dev);         // copy scene + draw band quad

        game::gx::RenderTarget scratch_{};  // pre-UI scene copy we sample from
        void*                  bandPS_ = nullptr;
    };
}
