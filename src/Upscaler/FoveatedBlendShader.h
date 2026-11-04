#pragma once

// Foveated Phase 2a's seam: one source string shared by the shipping TemporalAA seam and
// the standalone harness, so the harness compiles the exact shader the game runs.
//
// It runs after the game's own TemporalAA has drawn the whole eye into the parity history
// and RT4, scissored to one eye's DLAA box at a time, with hardware alpha blending onto
// both of those still-bound targets: out = w * DLAA + (1 - w) * TemporalAA. w is a
// smoothstep from the box's edge to `feather` pixels inside it, 1 beyond that, so the box's
// interior is DLAA exactly and its border fades into the engine's image. The history gets
// the raw DLAA term and RT4 the sharpened one, with the same weight - the history holds
// exactly the blend that was shown, before sharpening, which is what keeps the engine's
// next TemporalAA frame from ghosting at the seam. Only colour is written; both targets'
// alpha is left as the engine wrote it.
//
// The sharpen is the seam's RCAS, the same kernel as SharpenShader.h (AMD FidelityFX FSR
// 1.0 RCAS, MIT, re-expressed in our own HLSL; see that header for the notice and for what
// was taken), so a foveated box is sharpened exactly as a whole-eye DLAA frame is. The
// smoothstep feather follows the shape of alandtse/open-shaders' SubrectBlendCS.hlsl
// (GPL-3.0-or-later, read for shape at 7184ce4); nothing is copied.
//
// tint, for the debug key, mixes the box green and its feather amber in RT4 only.

#include <d3d11.h>

#include <cstdint>

namespace Upscaler::FoveatedBlendShader
{
	// out = w * DLAA + (1 - w) * the engine's TemporalAA on both targets, colour only; the
	// destination's alpha is kept. Shared with the harness, which draws with exactly this.
	[[nodiscard]] inline D3D11_BLEND_DESC BlendDesc() noexcept
	{
		D3D11_BLEND_DESC desc{};
		desc.IndependentBlendEnable = FALSE;
		desc.RenderTarget[0].BlendEnable = TRUE;
		desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		return desc;
	}

	// One eye per draw, scissored to its box.
	[[nodiscard]] inline D3D11_RASTERIZER_DESC RasterizerDesc() noexcept
	{
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.DepthClipEnable = TRUE;
		desc.ScissorEnable = TRUE;
		return desc;
	}

	inline constexpr char kSource[] = R"(
Texture2D<float4> sourceColor : register(t0);

cbuffer BlendConstants : register(b0)
{
    float4 box;
    float feather;
    float sharpenScale;
    uint tint;
    float blendPadding;
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

float3 LoadTap(int2 coord, int2 maxCoord)
{
    return saturate(sourceColor.Load(int3(clamp(coord, int2(0, 0), maxCoord), 0)).rgb);
}

float LumaTimesTwo(float3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

float3 Rcas(int2 p, float3 e, int2 maxCoord)
{
    const float3 b = LoadTap(p + int2(0, -1), maxCoord);
    const float3 d = LoadTap(p + int2(-1, 0), maxCoord);
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
    return (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
}

PixelOutputs PSMain(float4 position : SV_Position)
{
    uint width;
    uint height;
    sourceColor.GetDimensions(width, height);
    const int2 maxCoord = int2(int(width) - 1, int(height) - 1);
    const int2 p = int2(position.xy);
    const float4 center = sourceColor.Load(int3(p, 0));
    float3 shown = sharpenScale > 0.0 ? Rcas(p, saturate(center.rgb), maxCoord) : center.rgb;

    const float2 q = position.xy;
    const float edge = min(min(q.x - box.x, box.z - q.x), min(q.y - box.y, box.w - q.y));
    const float w = feather > 0.0 ? smoothstep(0.0, feather, edge) : 1.0;
    if (tint != 0) {
        shown = lerp(shown, w >= 0.999 ? float3(0.25, 1.0, 0.25) : float3(1.0, 0.65, 0.1), 0.3);
    }

    PixelOutputs output;
    output.history = float4(center.rgb, w);
    output.submitted = float4(shown, w);
    return output;
}
)";

	// Register b0: the box (x0, y0, x1, y1) in target pixels, the feather in pixels, the
	// RCAS linear scale (0 = the passthrough), the tint flag.
	struct Constants
	{
		float box[4]{};
		float feather = 0.0f;
		float sharpenScale = 0.0f;
		std::uint32_t tint = 0;
		float padding = 0.0f;
	};
	static_assert(sizeof(Constants) == 32, "b0 is two 16-byte registers");
}
