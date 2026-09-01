#pragma once

// One source string compiled by the shipping Intermediates module. The source motion
// texture is a private exact per-eye copy; the destination is the private texture handed
// to reconstruction backends. Finite nonzero payloads inside four per-eye UV extents pass
// through unchanged. Exact-zero, non-finite and larger payloads are completed from
// standard-Z DS1 plus the engine's current and previous unjittered matrices when that
// guarded camera snapshot is valid; otherwise they remain zero. The engine-owned
// double-wide motion and depth resources are read-only.

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
    uint depthAvailable;
    float2 completionPadding;
};

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}

struct SanitizedInputs
{
    float2 motion : SV_Target0;
    float depth : SV_Target1;
};

SanitizedInputs PSMain(float4 position : SV_Position)
{
    SanitizedInputs output;
    output.depth = depthAvailable != 0
        ? sourceDepth.Load(int3(uint2(position.xy) + depthBase, 0))
        : 0.0;
    float2 value = sourceMotion.Load(int3(uint2(position.xy), 0));
    if (all(isfinite(value)) && any(value != float2(0.0, 0.0)) && all(abs(value) <= 4.0)) {
        output.motion = value;
        return output;
    }
    if (completionEnabled == 0) {
        output.motion = float2(0.0, 0.0);
        return output;
    }

    float2 currentUv = position.xy * inverseDimensions;
    float2 currentNdc = float2(currentUv.x * 2.0 - 1.0, 1.0 - currentUv.y * 2.0);
    float4 previousClip = mul(float4(currentNdc, output.depth, 1.0), currentClipToPreviousClip);
    if (!all(isfinite(previousClip)) || abs(previousClip.w) <= 1.0e-6) {
        output.motion = float2(0.0, 0.0);
        return output;
    }

    float2 previousNdc = previousClip.xy / previousClip.w;
    float2 previousUv = float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5);
    float2 completed = previousUv - currentUv;
    output.motion = all(isfinite(completed)) && all(abs(completed) <= 4.0)
        ? completed
        : float2(0.0, 0.0);
    return output;
}
)";
}
