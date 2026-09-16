// wxl-render-modern grass: wind and unit-physics settings rows.
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

// Grass motion settings. Each struct is one settings row; the active rows are compile-time
// defaults for now and will later be served per zone from two custom DBCs (one for wind,
// one for physics). Consumers read the active rows once per frame, so a future data-driven
// swap needs no code change on the consumer side.
namespace wxl::scripts::render_modern::grass
{
    /** @brief Wind row: a directional two-wave sway field over the grass. */
    struct WindSettings
    {
        bool  enabled;         // master switch for the sway
        float directionDeg;    // wind heading in the world XY plane, degrees
        float speed;           // wave travel speed, yards per second
        float amplitude;       // primary wave sway at the blade tip, yards
        float wavelength;      // primary wave length, yards
        float crossAmplitude;  // secondary cross-swell sway, yards
        float crossWavelength; // secondary wave length, yards
        float crossAngleDeg;   // secondary wave heading offset from the primary, degrees
        float lean;            // constant downwind lean, fraction of the primary amplitude
        float variance;        // per-blade amplitude spread, 0 (uniform) .. 1 (0.5x to 1.5x)
        float distanceFade;    // per-yard view-distance sway attenuation, 0 disables
        float anchor;          // fraction of the blade (from the base) that never moves, 0 .. 0.9
    };

    /** @brief Physics row: blades part around a moving unit. */
    struct PhysicsSettings
    {
        bool  enabled;     // master switch for the parting
        float radius;      // influence radius around the unit, yards
        float forceCenter; // lean strength at the unit
        float forceEdge;   // lean strength at the radius edge
        float coneHeight;  // height above ground where the lean weight reaches 1, yards
        float minHeight;   // blades shorter than this stay still, yards
    };

    /** @brief Returns the wind row currently applied. */
    const WindSettings& ActiveWind();

    /** @brief Returns the physics row currently applied. */
    const PhysicsSettings& ActivePhysics();
}
