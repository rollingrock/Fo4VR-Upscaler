#pragma once

// The sharpened variant of the TemporalAA-seam shader, one source string shared by the
// shipping seam and the standalone hardware harness so the harness compiles the exact
// shader the game runs. Same shape as TAAPassthroughShader.h: VSMain, PSMain, two MRTs.
//
// The history output is the raw load, exactly what the passthrough writes: on a native
// fallback frame the engine's own TemporalAA blends from that history and applies its own
// sharpen to its output, so a sharpened history would sharpen twice there (INFERRED from
// the engine's TemporalAA shape, not measured). Only RT4, the submitted target, receives
// the sharpened value.
//
// The kernel is AMD FidelityFX FSR 1.0 RCAS (robust contrast adaptive sharpening),
// re-expressed in our own HLSL from `ffx_fsr1.h` (`FsrRcasCon`, `FsrRcasF`,
// `FSR_RCAS_LIMIT`) in the FidelityFX SDK, which is MIT:
//
//   Copyright (c) 2023 Advanced Micro Devices, Inc.
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this
//   software and associated documentation files (the "Software"), to deal in the Software
//   without restriction, including without limitation the rights to use, copy, modify,
//   merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to the following
//   conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies
//   or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
//   INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//   PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
//   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
//   OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// What was taken is the algorithm and its structure: the five-tap cross, luma times two,
// the min and max of the ring, the two clip limiters with the lower-limiter multiplier,
// the lobe clamped to [-(0.25 - 1/16), 0] and scaled by the linear sharpness, and the
// resolve with 1 / (4 * lobe + 1). The stops-to-linear mapping exp2(-stops) is
// FsrRcasCon's. Denoise is off, which the reference recommends. Nothing is copied
// verbatim: the reference is written against its own portability macros and load
// callbacks, and this file carries the D3D11 seam's bindings instead. Two choices are
// ours: the taps are clamped to the texture edge (the reference leaves loads to the
// caller; an out-of-range Load returns zero - REPORTED, D3D11 - which would sharpen the
// border row against black), and the five taps are saturated to [0, 1] before the
// kernel, because RCAS solves its limiters against a [0, 1] peak and RT4 is UNORM so it
// clamps there anyway. The clamp is to the double-wide's edge, not the eye's, so each
// eye's inner column reads one column of the other eye across the seam. INFERRED
// harmless: that column is the nasal edge under the lens, and the engine's own TemporalAA
// is taken to read its neighbourhood across the same seam. If a headset session ever
// shows it, sharpenPadding.x can carry the eye width for a per-eye clamp without growing
// the constant buffer.

#include <cmath>
#include <cstdint>

namespace Upscaler::SharpenShader
{
	inline constexpr char kSource[] = R"(
Texture2D<float4> sourceColor : register(t0);

cbuffer SharpenConstants : register(b0)
{
    float sharpenScale;
    float3 sharpenPadding;
};

struct PixelOutputs
{
    float4 history : SV_Target0;
    float4 submitted : SV_Target1;
};

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}

// Clamped to the texture edge, not the eye's: the header note covers the seam column.
float3 LoadTap(int2 coord, int2 maxCoord)
{
    return saturate(sourceColor.Load(int3(clamp(coord, int2(0, 0), maxCoord), 0)).rgb);
}

float LumaTimesTwo(float3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

PixelOutputs PSMain(float4 position : SV_Position)
{
    uint width;
    uint height;
    sourceColor.GetDimensions(width, height);
    const int2 maxCoord = int2(int(width) - 1, int(height) - 1);
    const int2 p = int2(position.xy);
    const float4 center = sourceColor.Load(int3(p, 0));

    const float3 b = LoadTap(p + int2(0, -1), maxCoord);
    const float3 d = LoadTap(p + int2(-1, 0), maxCoord);
    const float3 e = saturate(center.rgb);
    const float3 f = LoadTap(p + int2(1, 0), maxCoord);
    const float3 h = LoadTap(p + int2(0, 1), maxCoord);

    const float bL = LumaTimesTwo(b);
    const float dL = LumaTimesTwo(d);
    const float eL = LumaTimesTwo(e);
    const float fL = LumaTimesTwo(f);
    const float hL = LumaTimesTwo(h);

    const float3 mn4 = min(min(b, d), min(f, h));
    const float3 mx4 = max(max(b, d), max(f, h));
    const float2 peakC = float2(1.0, -4.0);
    const float lowerLimiterMultiplier = saturate(eL / min(min(bL, dL), min(fL, hL)));
    const float3 hitMin = mn4 * (1.0 / (4.0 * mx4)) * lowerLimiterMultiplier;
    const float3 hitMax = (peakC.x - mx4) * (1.0 / (4.0 * mn4 + peakC.y));
    const float3 lobeRgb = max(-hitMin, hitMax);
    const float limit = 0.25 - (1.0 / 16.0);
    const float lobe = max(-limit, min(max(max(lobeRgb.r, lobeRgb.g), lobeRgb.b), 0.0)) * sharpenScale;
    const float rcpL = 1.0 / (4.0 * lobe + 1.0);
    const float3 sharpened = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;

    PixelOutputs output;
    output.history = center;
    output.submitted = float4(sharpened, center.a);
    return output;
}
)";

	// The 16-byte constant buffer behind register b0.
	struct Constants
	{
		float scale = 1.0f;
		float padding[3]{};
	};
	static_assert(sizeof(Constants) == 16, "b0 is one 16-byte register");

	// The setting is 0.0..1.0; anything else is treated as off.
	[[nodiscard]] constexpr float Sanitize(float a_sharpness) noexcept
	{
		return a_sharpness > 0.0f && a_sharpness <= 1.0f ? a_sharpness : 0.0f;
	}

	// 0 keeps the passthrough shader: the off path is bit-identical to no pass by
	// construction rather than RCAS at two stops.
	[[nodiscard]] constexpr bool UsesRcas(float a_sharpness) noexcept
	{
		return Sanitize(a_sharpness) > 0.0f;
	}

	// RCAS stops from the setting: 1.0 is the reference maximum, 0.5 one stop down.
	[[nodiscard]] constexpr float Stops(float a_sharpness) noexcept
	{
		return 2.0f * (1.0f - Sanitize(a_sharpness));
	}

	// exp2(-stops), the linear lobe scale FsrRcasCon derives from its stops argument.
	[[nodiscard]] inline float LinearScale(float a_sharpness) noexcept
	{
		return std::exp2(-Stops(a_sharpness));
	}
}
