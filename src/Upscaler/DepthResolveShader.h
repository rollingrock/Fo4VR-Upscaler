#pragma once

// The one source string of the seam-exit depth-stencil resolve, in the shape the other
// two full-screen passes in this tree use (TAAPassthroughShader.h, SharpenShader.h): a
// VSMain that builds the covering triangle from SV_VertexID with no vertex buffer, and
// a PSMain compiled at ps_5_0.
//
// What it does. On a reduced frame the engine rasterizes both eyes into a rectangle
// packed at the origin of the double-wide depth-stencil - two eyes of reducedEyeWidth by
// reducedHeight at the top left of a fullEyeWidth*2 by fullHeight surface - and every
// later pass that reads that surface reads it at the FULL extent. This pass rewrites the
// whole plane at the full extent from a copy of the packed one, so the consumers that
// follow the seam see a depth buffer whose geometry is where they expect it.
//
// The mapping is per eye and integral: a full-extent texel is divided back into its own
// eye, scaled by that eye's reduced-to-full ratio, and offset to that eye's base inside
// the packed rectangle. Two eyes cannot share one ratio and one origin, which is why the
// eye index is recomputed per texel rather than folded into a single scale.
//
// The four taps are a 2x2 clamped INSIDE the eye's own packed rectangle: a tap that ran
// off the right edge of the left eye would sample the right eye's first column, and a tap
// past the packed height would sample depth the reduced raster never wrote. `min` is
// "nearest" here because this project's depth is standard-Z and not inverted - the
// backend config sets invertedDepth = false at every site that builds one - so the
// smaller value is the closer surface. Taking the nearest rather than a bilinear average
// is deliberate: the consumer is an occlusion test, and a silhouette that averages half
// its background reads as a hole in whatever is drawn against it.
//
// NO JITTER COMPENSATION. The references remove the frame's sub-pixel offset before
// sampling because their consumer is a refraction/AO pass whose output is compared
// against a jittered colour buffer. Ours is the HUD's occlusion test, which asks only
// whether something is nearer than the ribbon; a half-pixel of phase does not change that
// answer, and adding the offset here would need the engine's live jitter at the seam exit
// for a correction smaller than the 2x2 kernel it would sit inside.
//
// Design read from the GPL-3 references alandtse/open-shaders
// `Upscaling::UpscaleDepth` and `DepthRefractionUpscalePS.hlsl`, and the same pass ported
// to flat FO4 in northaxosky/fallout4-community-shaders. What was taken is the SHAPE of
// the fix - copy the depth-stencil, one full-screen triangle back into it with DepthFunc
// ALWAYS and a stencil state that writes zero, a minimum gather for the reduced-to-full
// map. Nothing is copied: the references sample with a linear sampler through their own
// dynamic-resolution UV helpers and discard against the copy's stencil, and this file
// addresses texels directly with Load, clamps per eye against the packed rectangle this
// plugin computed itself, and writes every texel because the stencil residue it also has
// to erase is exactly the region a discard would have kept.

#include <cstdint>

namespace Upscaler::DepthResolveShader
{
	// Matches the cbuffer below. Four uints, so the buffer is one 16-byte constant
	// register and needs no padding member.
	struct Constants
	{
		std::uint32_t fullEyeWidth = 0;
		std::uint32_t fullHeight = 0;
		std::uint32_t reducedEyeWidth = 0;
		std::uint32_t reducedHeight = 0;

		[[nodiscard]] bool operator==(const Constants&) const noexcept = default;
	};

	inline constexpr char kSource[] = R"(
Texture2D<float> packedDepth : register(t0);

cbuffer DepthResolveConstants : register(b0)
{
    uint fullEyeWidth;
    uint fullHeight;
    uint reducedEyeWidth;
    uint reducedHeight;
};

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}

// Unsigned throughout, and not for style: fxc warns X3556 on a signed integer divide and
// this plugin compiles its shaders with warnings as errors. Every quantity here is a
// texel index, so unsigned is also the honest type.
float PSMain(float4 position : SV_Position) : SV_Depth
{
    const uint2 p = uint2(position.xy);
    const uint eye = p.x < fullEyeWidth ? 0u : 1u;
    const uint baseFull = eye * fullEyeWidth;
    const uint baseReduced = eye * reducedEyeWidth;

    // Integer, floored, per eye. The engine's own packed extent is floor(full * ratio),
    // so the inverse taken this way lands on the texel the reduced raster wrote.
    const uint xr = baseReduced + ((p.x - baseFull) * reducedEyeWidth) / fullEyeWidth;
    const uint yr = (p.y * reducedHeight) / fullHeight;

    // Only an upper clamp is needed: xr starts at this eye's base and yr at zero, and the
    // taps only ever add.
    const uint maxX = baseReduced + reducedEyeWidth - 1u;
    const uint maxY = reducedHeight - 1u;

    // Standard-Z, so the nearest of the four is the smallest.
    float nearest = 1.0;
    [unroll] for (uint dy = 0u; dy < 2u; ++dy)
    {
        [unroll] for (uint dx = 0u; dx < 2u; ++dx)
        {
            uint2 tap = uint2(min(xr + dx, maxX), min(yr + dy, maxY));
            nearest = min(nearest, packedDepth.Load(int3(int(tap.x), int(tap.y), 0)));
        }
    }
    return nearest;
}
)";
}
