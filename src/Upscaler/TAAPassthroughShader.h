#pragma once

// One source string shared by the shipping bounded seam and the standalone hardware
// harness. Keeping it here makes the readback test compile the exact shader the game uses.

namespace Upscaler::TAAPassthroughShader
{
	inline constexpr char kSource[] = R"(
Texture2D<float3> sourceColor : register(t0);

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

PixelOutputs PSMain(float4 position : SV_Position)
{
    float4 value = float4(sourceColor.Load(int3(uint2(position.xy), 0)), 1.0);
    PixelOutputs output;
    output.history = value;
    output.submitted = value;
    return output;
}
)";
}
