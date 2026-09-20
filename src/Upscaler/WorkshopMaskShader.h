#pragma once

// Full-allocation workshop-mask remap used before the native ISVatsTarget pass.
//
// RT17 keeps its full-size allocation while dynamic resolution rasterizes the two
// eyes into a smaller packed rectangle. The native effect samples that texture in
// full-output coordinates. This pass expands the valid rectangle into a private
// full-size RGBA8 texture so the native shader, including its 5x5 mask filter and
// Effects/VScan composition, remains untouched.
//
// Mapping is per eye. Four explicit Load calls implement linear interpolation and
// every tap is clamped to its own source-eye rectangle; the left eye can therefore
// never blend the first column of the right eye. All four channels are interpolated.
// Source origins need not be zero.
//
// SCALE ONLY. The paired native screen-pass VS writes an unchanged UV and its
// constant writer supplies zero in the position-offset xy fields. The producer's
// deeper projection phase has not been proved equal to the scene phase, so this
// correction deliberately adds no guessed jitter offset.

#include <cstdint>
#include <type_traits>

namespace Upscaler::WorkshopMaskShader
{
	inline constexpr std::uint32_t kMaxTextureDimension = 16384;

	// Shared layout for b0. The source allocation fields make the CPU-side check
	// self-contained; the shader uses the validated rectangle and output extent.
	struct Constants
	{
		std::uint32_t sourceOriginX = 0;
		std::uint32_t sourceOriginY = 0;
		std::uint32_t sourceEyeWidth = 0;
		std::uint32_t sourceHeight = 0;

		std::uint32_t outputEyeWidth = 0;
		std::uint32_t outputHeight = 0;
		std::uint32_t sourceAllocationWidth = 0;
		std::uint32_t sourceAllocationHeight = 0;

		[[nodiscard]] bool operator==(const Constants&) const noexcept = default;
	};

	static_assert(sizeof(Constants) == 32, "b0 is two 16-byte registers");
	static_assert(std::is_standard_layout_v<Constants> && std::is_trivially_copyable_v<Constants>);

	// D3D11 textures are bounded to 16384 texels per axis at feature level 11.
	// Keeping all values inside that bound also makes every uint-to-float conversion
	// finite and exact. Subtraction/division checks avoid overflow in both packed-eye
	// and origin-plus-height calculations.
	[[nodiscard]] constexpr bool Validate(const Constants& a_constants) noexcept
	{
		const auto& c = a_constants;
		if (c.sourceEyeWidth == 0 || c.sourceHeight == 0 ||
			c.outputEyeWidth == 0 || c.outputHeight == 0 ||
			c.sourceAllocationWidth == 0 || c.sourceAllocationHeight == 0) {
			return false;
		}
		if (c.sourceAllocationWidth > kMaxTextureDimension ||
			c.sourceAllocationHeight > kMaxTextureDimension ||
			c.outputHeight > kMaxTextureDimension ||
			c.outputEyeWidth > kMaxTextureDimension / 2) {
			return false;
		}
		if (c.sourceOriginX > c.sourceAllocationWidth ||
			c.sourceOriginY > c.sourceAllocationHeight) {
			return false;
		}
		const auto availableWidth = c.sourceAllocationWidth - c.sourceOriginX;
		const auto availableHeight = c.sourceAllocationHeight - c.sourceOriginY;
		return c.sourceEyeWidth <= availableWidth / 2 &&
			c.sourceHeight <= availableHeight;
	}

	inline constexpr char kSource[] = R"(
Texture2D<float4> sourceMask : register(t0);

cbuffer WorkshopMaskConstants : register(b0)
{
    uint sourceOriginX;
    uint sourceOriginY;
    uint sourceEyeWidth;
    uint sourceHeight;

    uint outputEyeWidth;
    uint outputHeight;
    uint sourceAllocationWidth;
    uint sourceAllocationHeight;
};

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0
{
    const uint2 outputPixel = uint2(position.xy);
    const uint eye = min(outputPixel.x / outputEyeWidth, 1u);
    const uint localX = outputPixel.x - eye * outputEyeWidth;

    // Pixel-center mapping. Subtracting one half converts a source texel center
    // back to the integer coordinate space used by Texture2D.Load.
    const float sourceX = float(sourceOriginX + eye * sourceEyeWidth) +
        (float(localX) + 0.5) * float(sourceEyeWidth) / float(outputEyeWidth) - 0.5;
    const float sourceY = float(sourceOriginY) +
        (float(outputPixel.y) + 0.5) * float(sourceHeight) / float(outputHeight) - 0.5;

    const int minX = int(sourceOriginX + eye * sourceEyeWidth);
    const int maxX = minX + int(sourceEyeWidth) - 1;
    const int minY = int(sourceOriginY);
    const int maxY = minY + int(sourceHeight) - 1;
    const int x0 = int(floor(sourceX));
    const int y0 = int(floor(sourceY));
    const float2 blend = frac(float2(sourceX, sourceY));

    const int loadX0 = clamp(x0, minX, maxX);
    const int loadX1 = clamp(x0 + 1, minX, maxX);
    const int loadY0 = clamp(y0, minY, maxY);
    const int loadY1 = clamp(y0 + 1, minY, maxY);

    const float4 top = lerp(
        sourceMask.Load(int3(loadX0, loadY0, 0)),
        sourceMask.Load(int3(loadX1, loadY0, 0)), blend.x);
    const float4 bottom = lerp(
        sourceMask.Load(int3(loadX0, loadY1, 0)),
        sourceMask.Load(int3(loadX1, loadY1, 0)), blend.x);
    return lerp(top, bottom, blend.y);
}
)";
}
