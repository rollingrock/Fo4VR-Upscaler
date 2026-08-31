#pragma once

// One source string compiled by the shipping Intermediates module. The source motion
// texture is a private exact per-eye copy; the destination is the private texture handed
// to reconstruction backends. Invalid half-float payloads are replaced without touching
// the engine-owned double-wide target.

namespace Upscaler::MotionSanitizerShader
{
	inline constexpr char kSource[] = R"(
Texture2D<float2> sourceMotion : register(t0);

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}

float2 PSMain(float4 position : SV_Position) : SV_Target0
{
    float2 value = sourceMotion.Load(int3(uint2(position.xy), 0));
    return all(isfinite(value)) ? value : float2(0.0, 0.0);
}
)";
}
