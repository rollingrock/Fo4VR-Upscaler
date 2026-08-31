#pragma once

// One source string compiled by the shipping Intermediates module. The source motion
// texture is a private exact per-eye copy; the destination is the private texture handed
// to reconstruction backends. Finite nonzero payloads pass through unchanged. Exact-zero
// and non-finite holes are completed from standard-Z DS1 plus the engine's current and
// previous unjittered matrices when that guarded camera snapshot is valid; otherwise they
// remain zero. The engine-owned double-wide motion and depth resources are read-only.

namespace Upscaler::MotionSanitizerShader
{
	inline constexpr char kSource[] = R"(
Texture2D<float2> sourceMotion : register(t0);
Texture2D<float> sourceDepth : register(t1);

cbuffer MotionCompletion : register(b0)
{
    row_major float4x4 currentClipToPreviousClip;
    float2 inverseDimensions;
    uint2 depthBase;
    uint completionEnabled;
    float3 completionPadding;
};

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}

float2 PSMain(float4 position : SV_Position) : SV_Target0
{
    float2 value = sourceMotion.Load(int3(uint2(position.xy), 0));
    if (all(isfinite(value)) && any(value != float2(0.0, 0.0))) {
        return value;
    }
    if (completionEnabled == 0) {
        return float2(0.0, 0.0);
    }

    float2 currentUv = position.xy * inverseDimensions;
    float2 currentNdc = float2(currentUv.x * 2.0 - 1.0, 1.0 - currentUv.y * 2.0);
    float depth = sourceDepth.Load(int3(uint2(position.xy) + depthBase, 0));
    float4 previousClip = mul(float4(currentNdc, depth, 1.0), currentClipToPreviousClip);
    if (!all(isfinite(previousClip)) || abs(previousClip.w) <= 1.0e-6) {
        return float2(0.0, 0.0);
    }

    float2 previousNdc = previousClip.xy / previousClip.w;
    float2 previousUv = float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5);
    float2 completed = previousUv - currentUv;
    return all(isfinite(completed)) && all(abs(completed) <= 4.0) ? completed : float2(0.0, 0.0);
}
)";
}
