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

namespace
{
    // Flips the frame vertically and blends it back in toward the bottom of the screen, faking a
    // reflection without any depth or water mask. No notion of "is this pixel water" -- it is a fixed
    // screen-space band, so it will also tint terrain/rock in the same band. That trade-off is the
    // whole point: zero new hooks, one fullscreen pass.
    //
    // c0.x = bandStart   - screen-space v (0 top, 1 bottom) where the blend begins ramping in (default 0.6)
    // c0.y = maxOpacity  - opacity of the flipped sample at the very bottom of the screen (0..1, default 0.35)
    // c0.z = flipBias    - extra vertical offset applied to the flip source, in case the natural
    //                      mirror point doesn't read as "horizon" on your camera pitch (default 0.0)
    const char* kWaterBandHLSL =
        "sampler2D s0      : register(s0);\n" // the real, already-rendered frame
        "float4    c0      : register(c0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR0\n"
        "{\n"
        "    float4 col = tex2D(s0, uv);\n"
        // how far into the band are we, 0 at bandStart, 1 at the bottom of the screen
        "    float  t   = saturate((uv.y - c0.x) / max(1.0 - c0.x, 0.001));\n"
        // flip vertically around bandStart, with optional bias, so the "reflection" reads as
        // continuing the world above the band rather than just mirroring the band itself
        "    float2 fuv = uv;\n"
        "    fuv.y      = saturate(c0.x - (uv.y - c0.x) - c0.z);\n"
        "    float4 ref = tex2D(s0, fuv);\n"
        // ramp opacity with t, capped at maxOpacity, and ease it so the seam at bandStart is soft
        "    float  op  = t * t * c0.y;\n"
        "    col.rgb    = lerp(col.rgb, ref.rgb, op);\n"
        "    return col;\n"
        "}\n";
}
