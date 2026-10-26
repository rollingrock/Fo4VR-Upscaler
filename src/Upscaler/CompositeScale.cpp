#include "Upscaler/CompositeScale.h"

#include "Upscaler/Addresses.h"
#include "Upscaler/Common.h"
#include "Upscaler/CompositeContracts.h"
#include "Upscaler/D3D11StateIsolation.h"
#include "Upscaler/DynamicResolution.h"
#include "Upscaler/Engine.h"
#include "Upscaler/FaultGuard.h"

#include <Windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace Upscaler::CompositeScale
{
	namespace
	{
		// VERIFIED from the freshly extracted VR witness: per-draw constants are b2,
		// stereo state is b8, and the global camera buffer is b12. Reconstruction uses
		// shader rows 32..47; rows 20..31 have different jobs and are never changed here.
		constexpr UINT kPerDrawSlot = 2;
		constexpr UINT kStereoSlot = 8;
		constexpr UINT kCameraSlot = 12;
		constexpr UINT kCubeArraySlot = 8;
		constexpr std::size_t kFirstReconstructionRow = 32;
		constexpr std::size_t kReconstructionRowCount = 16;
		constexpr std::size_t kBytesPerConstant = 16;
		constexpr std::size_t kMaxBufferBytes = 4096;
		constexpr std::size_t kMaxSamples = 8;
		constexpr std::size_t kSamplesPerRatioClass = 4;
		constexpr std::size_t kMaxPixelShaders = 16;
		constexpr std::size_t kMaxRouteSamples = 32;
		constexpr std::size_t kMaxRegisteredTechniques = 4096;
		constexpr std::size_t kMaxUnknownPostCommits = 64;
		constexpr std::size_t kMaxKnownNonCubeSamples = 32;
		constexpr std::size_t kMaxAdmittedPreliminarySamples = 16;
		constexpr std::size_t kMaxKnownOtherPreliminarySamples = 64;
		constexpr std::size_t kMaxUnknownPreliminarySamples = 64;
		constexpr std::size_t kMaxPixelTableBuckets = 4096;
		constexpr std::size_t kMaxPixelTableNodes = 4096;
		constexpr std::uint32_t kMaxCaptureFailures = 8;
		constexpr std::uintptr_t kAdmittedCallerReturnRva = 0x1d98901;
		constexpr UINT kCameraBufferBytes = 1360;
		constexpr UINT kCameraBufferRows = kCameraBufferBytes / kBytesPerConstant;
		constexpr UINT kExpectedFirstConstant = 0;
		constexpr UINT kExpectedNumConstants = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
		constexpr std::size_t kMaxCorrectionSamples = 4;
		constexpr std::uint32_t kObservedPixelTechnique = 65600;
		constexpr std::size_t kMaxTechniqueObservationSamples = 4;
		constexpr std::size_t kMaxTechnique65600CorrectionSamples = 4;
		constexpr std::uint32_t kTechnique65600ExpectedCorrectedMask = 0x3;
		constexpr std::uint32_t kTechnique65600ExpectedSkippedMask = 0xc;
		constexpr float kTechnique65600MinDepth = 0.01f;
		constexpr std::size_t kObservedResourceCount = 7;
		constexpr UINT kObservedResourceSlots[kObservedResourceCount] = { 0, 3, 4, 5, 7, 8, 11 };
		constexpr std::size_t kObservedCameraRows = 77;

		using D3DCompileFn = HRESULT(WINAPI*)(
			LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
			LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

		[[nodiscard]] constexpr bool IsAdmittedPixelTechnique(std::uint32_t a_pixelTechnique) noexcept
		{
			const auto* const contract = CompositeContracts::Find(a_pixelTechnique);
			return contract && contract->kind == CompositeContracts::Kind::kCube;
		}

		constexpr char kCorrectionShader[] = R"hlsl(
cbuffer Camera : register(b0) { uint4 sourceRows[85]; };
cbuffer Parameters : register(b1) {
    float scaleX;
    float scaleY;
    uint totalRows;
    uint firstReconstructionRow;
    float ratioWidth;
    float ratioHeight;
    float viewportWidth;
    float viewportHeight;
    uint dimensionRow;
    uint deriveViewportScale;
    uint nativeWidth;
    uint nativeHeight;
};
#ifndef PRODUCT_BASIS
#define PRODUCT_BASIS 0
#endif
#if PRODUCT_BASIS
cbuffer PerDraw : register(b2) { float4 perDrawRows[6]; };
#else
cbuffer PerDraw : register(b2) { float4 perDrawRows[1]; };
#endif
cbuffer Stereo : register(b3) { uint4 stereoConstants; };
RWByteAddressBuffer corrected : register(u0);
RWByteAddressBuffer finiteMask : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint rowIndex = dispatchId.x;
    const bool reconstructionRow =
        rowIndex >= firstReconstructionRow && rowIndex < firstReconstructionRow + 16;
    if (rowIndex < totalRows && !reconstructionRow) {
        corrected.Store4(rowIndex * 16, sourceRows[rowIndex]);
    }

    if (rowIndex >= 4) {
        return;
    }
    const uint matrixBase = firstReconstructionRow + rowIndex * 4;
    const uint eye = rowIndex & 1;
#if PRODUCT_BASIS
    const float4 sizeConstants = perDrawRows[5];
    const float2 inverseSize = perDrawRows[0].xy * sizeConstants.xy;
    const bool basisRowMatches = dimensionRow == 5u;
#else
    const float4 sizeConstants = perDrawRows[0];
    const float2 inverseSize = sizeConstants.xy;
    const bool basisRowMatches = dimensionRow == 0u;
#endif
    const float2 nativeSize = float2(nativeWidth, nativeHeight);
    const float2 viewportSize = float2(viewportWidth, viewportHeight);
    const float2 requestedSize = nativeSize * float2(ratioWidth, ratioHeight);
    const float2 flooredSize = float2(2.0f * floor(requestedSize.x * 0.5f), floor(requestedSize.y));
    // The geometry variants normalize SV_Position through b2[0] and then b2[5].
    const bool indexedBasis = basisRowMatches &&
        all(isfinite(perDrawRows[0].xy)) && all(perDrawRows[0].xy > 0.0f) &&
        (dimensionRow == 0u || (all(isfinite(sizeConstants.xy)) && all(sizeConstants.xy > 0.0f))) &&
        all(isfinite(inverseSize)) && all(inverseSize > 0.0f) &&
        all(isfinite(nativeSize)) && all(nativeSize >= 1.0f) &&
        all(abs(inverseSize * nativeSize - 1.0f) <= 0.0001f) &&
        all(isfinite(viewportSize)) && all(viewportSize > 0.0f) &&
        all(viewportSize <= nativeSize) &&
        all((abs(requestedSize - viewportSize) <= 0.05f) ||
            (abs(flooredSize - viewportSize) <= 0.05f));
    const float2 scale = deriveViewportScale != 0 ?
        nativeSize / viewportSize : float2(scaleX, scaleY);
    const float translateX = (1.0 + 2.0 * eye) * (scale.x - 1.0);
    const float translateY = 1.0 - scale.y;
	float4 source[4];
	uint4 sourceBits[4];
	float4 candidate[4];
    const bool stereoBasis =
        stereoConstants.x == asuint(1.0f) &&
        (deriveViewportScale != 0 ? indexedBasis :
            all(isfinite(sizeConstants)) && all(sizeConstants > 0.0f) &&
            abs(sizeConstants.x * sizeConstants.z - 1.0f) <= 0.0001f &&
            abs(sizeConstants.y * sizeConstants.w - 1.0f) <= 0.0001f &&
            abs(sizeConstants.z * ratioWidth - viewportWidth) <= 0.05f &&
            abs(sizeConstants.w * ratioHeight - viewportHeight) <= 0.05f);
    bool finite = stereoBasis;
	[unroll]
    for (uint loadRow = 0; loadRow < 4; ++loadRow) {
        sourceBits[loadRow] = sourceRows[matrixBase + loadRow];
        source[loadRow] = asfloat(sourceBits[loadRow]);
        finite = finite && all(isfinite(source[loadRow]));
        candidate[loadRow] = float4(
            source[loadRow].x * scale.x,
            source[loadRow].y * scale.y,
            source[loadRow].z,
            source[loadRow].w + source[loadRow].x * translateX +
                source[loadRow].y * translateY);
    }
    [unroll]
    for (uint validateRow = 0; validateRow < 4; ++validateRow) {
        finite = finite && all(isfinite(candidate[validateRow]));
    }
    [unroll]
    for (uint storeRow = 0; storeRow < 4; ++storeRow) {
        corrected.Store4(
            (matrixBase + storeRow) * 16,
            finite ? asuint(candidate[storeRow]) : sourceBits[storeRow]);
    }
    if (finite) {
        uint ignored;
        finiteMask.InterlockedOr(0, 1u << rowIndex, ignored);
    }
    if (rowIndex == 0 && stereoBasis) {
        uint ignored;
        finiteMask.InterlockedOr(0, 0x10u, ignored);
    }
}
)hlsl";

		// Counts cover only the exact prepared caller while correction is enabled.
		// g_lock protects publication and reset; no GPU readback is involved.
		struct FamilyCoverage
		{
			std::uint64_t observed = 0;
			std::uint64_t native = 0;
			std::uint64_t reduced = 0;
			std::uint64_t invalidRatio = 0;
			std::uint64_t admitted = 0;
			std::uint64_t finished = 0;
			std::uint64_t privateDraws = 0;
			std::uint64_t restored = 0;
			std::uint64_t restoreFailures = 0;
			std::uint64_t declines[24]{};
			std::uint64_t legacyDeclines[17]{};
		};
		FamilyCoverage g_familyCoverage[CompositeContracts::kRecordCount + 1]{};
		bool g_familyTableMatches = false;

		struct BufferCapture
		{
			std::uintptr_t identity = 0;
			std::uint32_t byteWidth = 0;
			std::uint32_t usage = 0;
			std::uint32_t bindFlags = 0;
			std::uint32_t cpuAccessFlags = 0;
			std::uint32_t firstConstant = 0;
			std::uint32_t numConstants = 0;
			bool rangeKnown = false;
			std::uint32_t dataBytes = 0;
			std::uint8_t data[kMaxBufferBytes]{};
		};

		struct Sample
		{
			std::uint64_t serial = 0;
			std::uint32_t threadId = 0;
			bool reduced = false;
			std::uintptr_t callerReturn = 0;
			std::uintptr_t callerReturnRva = 0;
			std::uintptr_t packet = 0;
			std::uint32_t packetDrawCount = 0;
			std::uintptr_t pass = 0;
			std::uintptr_t shaderState = 0;
			std::uintptr_t engineShader = 0;
			std::uintptr_t engineShaderVtable = 0;
			std::uint32_t technique = 0;
			std::uint32_t vertexTechnique = 0;
			std::uint32_t pixelTechnique = 0;
			std::uintptr_t pixelShaderRecord = 0;
			std::uintptr_t expectedPixelShader = 0;
			std::uintptr_t boundPixelShader = 0;
			bool pixelShaderMatchesRecord = false;
			std::uintptr_t context = 0;
			std::uint32_t contextType = 0;
			bool multithreadInterface = false;
			bool multithreadProtected = false;
			bool registryDeviceMatches = false;
			std::uintptr_t engineCameraBuffer = 0;
			bool cb12MatchesEngine = false;
			std::uint32_t t8ViewDimension = 0;
			float ratioWidth = 0.0f;
			float ratioHeight = 0.0f;
			std::uint32_t viewportCount = 0;
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			BufferCapture cb2{};
			BufferCapture cb8{};
			BufferCapture cb12{};
		};

		struct BoundState
		{
			ID3D11Device* device = nullptr;
			ID3D11PixelShader* pixelShader = nullptr;
			ID3D11ShaderResourceView* t8 = nullptr;
			ID3D11Buffer* cb2 = nullptr;
			ID3D11Buffer* cb8 = nullptr;
			ID3D11Buffer* cb12 = nullptr;
			std::uint32_t t8ViewDimension = 0;
			std::uint32_t contextType = 0;
			std::uint32_t targetWidth = 0;
			std::uint32_t targetHeight = 0;
			std::uint32_t viewportCount = 0;
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			std::uint32_t firstConstant[3]{};
			std::uint32_t numConstants[3]{};
			bool rangeKnown = false;
			std::uint32_t exceptionCode = 0;
			bool multithreadInterface = false;
			bool multithreadProtected = false;
		};

		struct ReadbackResult
		{
			std::int32_t hr = 0;
			std::uint32_t exceptionCode = 0;
			bool copied = false;
		};

		struct DeviceRetainResult
		{
			ID3D11Device* device = nullptr;
			ID3D11PixelShader* pixelShader = nullptr;
			std::uint32_t exceptionCode = 0;
		};

		struct PixelShaderQuery
		{
			ID3D11PixelShader* pixelShader = nullptr;
			std::uint32_t exceptionCode = 0;
		};

		struct RouteSample
		{
			std::uint32_t threadId = 0;
			std::uintptr_t pass = 0;
			std::uintptr_t shaderState = 0;
			std::uintptr_t engineShader = 0;
			std::uint32_t technique = 0;
			std::uintptr_t shape = 0;
			std::uint32_t geometryType = 0;
			bool shapeReadable = false;
			bool hasPassOverride = false;
			std::uintptr_t pixelShaderRecord = 0;
			std::uintptr_t expectedPixelShader = 0;
			float ratioWidth = 0.0f;
			float ratioHeight = 0.0f;
			std::uint64_t count = 0;
		};

		struct RegisteredTechnique
		{
			std::uintptr_t engineShader = 0;
			std::uintptr_t shaderState = 0;
			std::uintptr_t pixelShaderRecord = 0;
			ID3D11PixelShader* pixelShader = nullptr;
			std::uint32_t vertexTechnique = 0;
			std::uint32_t pixelTechnique = 0;
			std::uint64_t enrollments = 0;
		};

		struct UnknownPostCommit
		{
			std::uintptr_t callerReturn = 0;
			std::uintptr_t callerReturnRva = 0;
			std::uintptr_t boundPixelShader = 0;
			std::uint32_t threadId = 0;
			std::uint64_t count = 0;
		};

		struct KnownNonCubeSample
		{
			std::uintptr_t callerReturn = 0;
			std::uintptr_t callerReturnRva = 0;
			std::uintptr_t pixelShaderRecord = 0;
			std::uintptr_t boundPixelShader = 0;
			std::uint32_t vertexTechnique = 0;
			std::uint32_t pixelTechnique = 0;
			std::uint32_t t8ViewDimension = 0;
			std::uint32_t threadId = 0;
			std::uint64_t count = 0;
		};

		enum class PreliminaryPixelShaderKind : std::uint32_t
		{
			kAdmitted = 0,
			kKnownOther = 1,
			kUnknown = 2,
			kNull = 3,
			kException = 4,
		};

		enum class PreliminaryRatioClass : std::uint32_t
		{
			kReduced = 1,
			kNative = 2,
			kInvalid = 3,
		};

		struct PreliminaryPixelShaderSample
		{
			std::uintptr_t observedPixelShader = 0;
			std::uintptr_t pixelShaderRecord = 0;
			std::uint32_t pixelTechnique = 0;
			std::uint32_t threadId = 0;
			std::uint32_t exceptionCode = 0;
			PreliminaryPixelShaderKind kind = PreliminaryPixelShaderKind::kUnknown;
			bool registryKnown = false;
			std::uint64_t firstDrawSerial = 0;
			std::uint64_t lastDrawSerial = 0;
			std::uint64_t count = 0;
			std::uint64_t reducedCount = 0;
			std::uint64_t nativeCount = 0;
			std::uint64_t invalidRatioCount = 0;
			std::uint64_t firstReducedDrawSerial = 0;
			std::uint64_t lastReducedDrawSerial = 0;
		};

		struct CorrectionParameters
		{
			float scaleX = 1.0f;
			float scaleY = 1.0f;
			std::uint32_t totalRows = 0;
			std::uint32_t firstReconstructionRow = 0;
			float ratioWidth = 1.0f;
			float ratioHeight = 1.0f;
			float viewportWidth = 0.0f;
			float viewportHeight = 0.0f;
			std::uint32_t dimensionRow = 0;
			std::uint32_t deriveViewportScale = 0;
			std::uint32_t nativeWidth = 0;
			std::uint32_t nativeHeight = 0;
		};
		static_assert(sizeof(CorrectionParameters) == 48);

		struct CorrectionCapture
		{
			std::uint64_t serial = 0;
			std::uint64_t generation = 0;
			std::uint64_t correctionEpoch = 0;
			std::uint64_t resourceEpoch = 0;
			std::uint32_t threadId = 0;
			std::uint32_t expectedOwnerThreadId = 0;
			std::uintptr_t callerReturn = 0;
			std::uintptr_t callerReturnRva = 0;
			std::uintptr_t packet = 0;
			std::uint32_t packetDrawCount = 0;
			std::uint32_t pixelTechnique = 0;
			std::uintptr_t pixelShaderRecord = 0;
			std::uintptr_t expectedPixelShader = 0;
			std::uintptr_t boundPixelShader = 0;
			std::uintptr_t context = 0;
			std::uintptr_t device = 0;
			std::uintptr_t engineCameraBuffer = 0;
			std::uint32_t contextType = 0;
			bool registryDeviceMatches = false;
			bool ownerIdentityMatches = false;
			bool cb12MatchesEngine = false;
			bool multithreadInterface = false;
			bool multithreadProtected = false;
			bool dimensionCacheMatched = false;
			std::uintptr_t originalCamera = 0;
			std::uintptr_t privateCamera = 0;
			std::uint32_t firstConstant = 0;
			std::uint32_t numConstants = 0;
			float ratioWidth = 0.0f;
			float ratioHeight = 0.0f;
			float derivedRatioWidth = 0.0f;
			float derivedRatioHeight = 0.0f;
			float scaleX = 1.0f;
			float scaleY = 1.0f;
			std::uint32_t correctedMask = 0;
			std::uint32_t skippedMask = 0;
			bool maskKnown = false;
			bool privateBindingVerified = false;
			bool helperCalled = false;
			bool originalBindingVerified = false;
			bool completeReconstruction = false;
			bool expectedPartialReconstruction = false;
			bool stereoBasisValidated = false;
			float nativeWidth = 0.0f;
			float nativeHeight = 0.0f;
			float viewportTopLeftX = 0.0f;
			float viewportTopLeftY = 0.0f;
			float viewportWidth = 0.0f;
			float viewportHeight = 0.0f;
			float viewportMinDepth = 0.0f;
			float viewportMaxDepth = 0.0f;
			float stereoValue = 0.0f;
			BufferCapture cb2{};
			BufferCapture cb8{};
			BufferCapture source{};
			BufferCapture privateCopy{};
		};

		struct ShaderResourceCapture
		{
			std::uint32_t slot = 0;
			bool present = false;
			std::uintptr_t viewIdentity = 0;
			std::uint32_t viewFormat = 0;
			std::uint32_t viewDimension = 0;
			std::uintptr_t resourceIdentity = 0;
			std::uint32_t resourceDimension = 0;
			bool texture2D = false;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::uint32_t mipLevels = 0;
			std::uint32_t arraySize = 0;
			std::uint32_t format = 0;
			std::uint32_t sampleCount = 0;
			std::uint32_t sampleQuality = 0;
			std::uint32_t usage = 0;
			std::uint32_t bindFlags = 0;
			std::uint32_t cpuAccessFlags = 0;
			std::uint32_t miscFlags = 0;
		};

		struct TechniqueObservationBoundState
		{
			ID3D11Device* device = nullptr;
			ID3D11PixelShader* pixelShader = nullptr;
			ID3D11Buffer* cb2 = nullptr;
			ID3D11Buffer* cb8 = nullptr;
			ID3D11Buffer* cb12 = nullptr;
			std::uint32_t contextType = 0;
			std::uint32_t viewportCount = 0;
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			std::uint32_t firstConstant[3]{};
			std::uint32_t numConstants[3]{};
			bool rangeKnown = false;
			std::uint32_t exceptionCode = 0;
			bool multithreadInterface = false;
			bool multithreadProtected = false;
			ShaderResourceCapture resources[kObservedResourceCount]{};
		};

		struct TechniqueObservationCapture
		{
			std::uint64_t serial = 0;
			std::uint64_t correctionGeneration = 0;
			std::uint64_t diagnosticEpoch = 0;
			std::uint32_t threadId = 0;
			std::uint32_t expectedRenderThreadId = 0;
			std::uintptr_t callerReturn = 0;
			std::uintptr_t callerReturnRva = 0;
			std::uintptr_t packet = 0;
			std::uint32_t packetDrawCount = 0;
			std::uint32_t pixelTechnique = 0;
			std::uintptr_t pixelShaderRecord = 0;
			std::uintptr_t expectedPixelShader = 0;
			std::uintptr_t boundPixelShader = 0;
			bool pixelShaderMatchesRecord = false;
			std::uintptr_t context = 0;
			std::uintptr_t device = 0;
			std::uintptr_t engineCameraBuffer = 0;
			std::uint32_t contextType = 0;
			bool registryDeviceMatches = false;
			bool ownerIdentityMatches = false;
			bool cb12MatchesEngine = false;
			bool multithreadInterface = false;
			bool multithreadProtected = false;
			bool ratiosRead = false;
			bool ratiosValid = false;
			float ratioWidth = 0.0f;
			float ratioHeight = 0.0f;
			std::uint32_t viewportCount = 0;
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			bool stereoBasisValidated = false;
			bool cameraRangeCoversShaderRows = false;
			float nativeWidth = 0.0f;
			float nativeHeight = 0.0f;
			float stereoValue = 0.0f;
			BufferCapture cb2{};
			BufferCapture cb8{};
			BufferCapture cb12{};
			ShaderResourceCapture resources[kObservedResourceCount]{};
		};

		enum class TechniqueObservationFailure : std::uint32_t
		{
			kNone = 0,
			kProtectionUnavailable = 1,
			kWrongRenderThread = 2,
			kOwnerIdentityMismatch = 3,
			kEnterFailed = 4,
			kBoundStateFailed = 5,
			kShaderIdentityMismatch = 6,
			kBindingsMissing = 7,
			kEpisodeIdentityMismatch = 8,
			kReadbackFailed = 9,
			kLeaveFailed = 10,
			kStaleArm = 11,
			kRatiosInvalid = 12,
		};

		enum class Technique65600CorrectionDecline : std::uint32_t
		{
			kNone = 0,
			kProtectionUnavailable = 1,
			kWrongRenderThread = 2,
			kOwnerIdentityMismatch = 3,
			kBindingsMissing = 4,
			kShaderIdentityMismatch = 5,
			kBufferContract = 6,
			kViewportContract = 7,
			kNativePassThrough = 8,
			kDimensionReadFailed = 9,
			kDimensionCacheStale = 10,
			kPrivatePreparationFailed = 11,
			kPrivateBindFailed = 12,
			kOriginalRestoreFailed = 13,
			kStateUncertain = 14,
			kStaleEpisode = 15,
			kEvidenceReadFailed = 16,
		};

		struct TechniqueObservationThreadToken
		{
			ID3D11Multithread* multithread = nullptr;
			std::uint32_t ownerThreadId = 0;
			std::uintptr_t contextIdentity = 0;
			std::uintptr_t deviceIdentity = 0;
			bool entered = false;
		};

		struct Technique65600DimensionCache
		{
			std::uint64_t correctionEpoch = 0;
			std::uint64_t generation = 0;
			std::uint64_t resourceEpoch = 0;
			std::uintptr_t context = 0;
			std::uintptr_t device = 0;
			float width = 0.0f;
			float height = 0.0f;
			bool attempted = false;
			bool valid = false;
		};

		struct PrivatePreparationResult
		{
			std::int32_t hr = S_OK;
			std::uint32_t exceptionCode = 0;
			bool began = false;
			bool dispatched = false;
			bool ended = false;
			bool stateUncertain = false;
		};

		struct Technique65600DimensionResult
		{
			std::int32_t hr = S_OK;
			std::uint32_t exceptionCode = 0;
			float width = 0.0f;
			float height = 0.0f;
			std::uint64_t resourceEpoch = 0;
			Technique65600CorrectionDecline decline =
				Technique65600CorrectionDecline::kNone;
			bool cacheIdentityMatched = false;
			bool valid = false;
		};

		std::mutex g_lock;
		// Serializes publication/replacement of the retained ID3D11Multithread epoch.
		// It is never acquired while g_lock is held and is released before any g_lock use.
		std::mutex g_protectionEpochLock;
		Sample g_samples[kMaxSamples]{};
		RouteSample g_routeSamples[kMaxRouteSamples]{};
		RegisteredTechnique g_registeredTechniques[kMaxRegisteredTechniques]{};
		UnknownPostCommit g_unknownPostCommits[kMaxUnknownPostCommits]{};
		KnownNonCubeSample g_knownNonCubeSamples[kMaxKnownNonCubeSamples]{};
		PreliminaryPixelShaderSample g_admittedPreliminarySamples[kMaxAdmittedPreliminarySamples]{};
		PreliminaryPixelShaderSample g_knownOtherPreliminarySamples[kMaxKnownOtherPreliminarySamples]{};
		PreliminaryPixelShaderSample g_unknownPreliminarySamples[kMaxUnknownPreliminarySamples]{};
		std::size_t g_sampleCount = 0;
		std::size_t g_routeSampleCount = 0;
		std::size_t g_registeredTechniqueCount = 0;
		std::size_t g_unknownPostCommitCount = 0;
		std::size_t g_knownNonCubeSampleCount = 0;
		std::size_t g_admittedPreliminarySampleCount = 0;
		std::size_t g_knownOtherPreliminarySampleCount = 0;
		std::size_t g_unknownPreliminarySampleCount = 0;
		std::uint32_t g_routeOverflow = 0;
		std::uint32_t g_registryOverflow = 0;
		std::uint32_t g_unknownPostCommitOverflow = 0;
		std::uint32_t g_knownNonCubeOverflow = 0;
		std::uint64_t g_knownNonCubeMatches = 0;
		std::uint64_t g_preliminaryDrawSerial = 0;
		std::uint64_t g_preliminaryAdmittedMatches = 0;
		std::uint64_t g_preliminaryAdmittedReduced = 0;
		std::uint64_t g_preliminaryAdmittedNative = 0;
		std::uint64_t g_preliminaryAdmittedInvalidRatio = 0;
		std::uint64_t g_preliminaryAdmittedOverflow = 0;
		std::uint64_t g_rejectedKnownOtherReduced = 0;
		std::uint64_t g_rejectedUnknownReduced = 0;
		std::uint64_t g_rejectedNullReduced = 0;
		std::uint64_t g_rejectedExceptionsReduced = 0;
		std::uint64_t g_rejectedNativeRatio = 0;
		std::uint64_t g_rejectedInvalidRatio = 0;
		std::uint64_t g_rejectedCensusOverflow = 0;
		std::uint64_t g_knownOtherPreliminaryOverflow = 0;
		std::uint64_t g_unknownPreliminaryOverflow = 0;
		std::uint64_t g_rejectedOverflowByKind[4]{};
		std::uint32_t g_tableSnapshotCapacity = 0;
		std::uint32_t g_tableSnapshotRecords = 0;
		std::uint32_t g_tableSnapshotFailures = 0;
		bool g_tableSnapshotComplete = false;
		ID3D11Device* g_registryDevice = nullptr;
		std::atomic<bool> g_tableSnapshotAttempted{ false };
		std::atomic<bool> g_tableSnapshotInProgress{ false };
		std::size_t g_nativeSamples = 0;
		std::size_t g_reducedSamples = 0;
		std::uintptr_t g_pixelShaders[kMaxPixelShaders]{};
		std::size_t g_pixelShaderCount = 0;
		std::uint32_t g_captureFailures = 0;
		bool g_nativeCompleteLogged = false;
		bool g_reducedCompleteLogged = false;
		bool g_allSamplesValid = true;

		std::atomic<bool> g_enabled{ false };
		// Settings publishes this flag; each render draw takes one snapshot.
		std::atomic<bool> g_diagnosticsEnabled{ false };
		std::atomic<std::uint64_t> g_attempts{ 0 };
		std::atomic<std::uint64_t> g_captures{ 0 };
		std::atomic<std::uint64_t> g_restores{ 0 };
		std::atomic<std::uint64_t> g_declines{ 0 };
		std::atomic<std::uint32_t> g_lastDecline{ 0 };
		std::atomic<std::uint32_t> g_exceptionCode{ 0 };
		std::atomic<std::int32_t> g_lastHr{ 0 };
		std::atomic<float> g_lastRatioWidth{ 0.0f };
		std::atomic<float> g_lastRatioHeight{ 0.0f };
		std::atomic<std::uint32_t> g_bufferBytes{ 0 };
		std::atomic<bool> g_ready{ false };
		std::atomic<std::uint64_t> g_drawRoutes{ 0 };
		std::atomic<std::uint64_t> g_preparedPacketDrawRoutes{ 0 };
		std::atomic<std::uint64_t> g_wrongCallerRoutes{ 0 };
		std::atomic<std::uint64_t> g_compositeDraws{ 0 };
		std::atomic<std::uint32_t> g_compositeShaders{ 0 };

		D3D11StateIsolation::Context g_correctionIsolation;
		HMODULE g_compilerModule = nullptr;
		D3DCompileFn g_compile = nullptr;
		ID3D11Device* g_correctionDevice = nullptr;
		ID3D11DeviceContext* g_correctionContext = nullptr;
		ID3D11DeviceContext1* g_correctionContext1 = nullptr;
		ID3D11ComputeShader* g_correctionShaders[2]{};
		ID3D11Buffer* g_correctionParameters = nullptr;
		ID3D11Buffer* g_correctedRaw = nullptr;
		ID3D11UnorderedAccessView* g_correctedRawUav = nullptr;
		ID3D11Buffer* g_finiteMask = nullptr;
		ID3D11UnorderedAccessView* g_finiteMaskUav = nullptr;
		ID3D11Buffer* g_privateCamera = nullptr;
		ID3D11Multithread* g_multithread = nullptr;
		ID3D11Device* g_protectionDevice = nullptr;
		ID3D11DeviceContext* g_protectionContext = nullptr;
		std::uint32_t g_ownerThreadId = 0;
		std::uint32_t g_correctionBufferBytes = 0;
		std::atomic<bool> g_multithreadPriorProtected{ false };
		std::atomic<bool> g_multithreadEnabledByModule{ false };
		std::atomic<bool> g_multithreadProtected{ false };
		bool g_correctionResourcesReady = false;
		CorrectionCapture g_correctionCaptures[kMaxCorrectionSamples]{};
		std::size_t g_correctionCaptureCount = 0;
		std::atomic<std::uint64_t> g_generation{ 0 };
		std::atomic<bool> g_releaseRequested{ false };
		std::atomic<bool> g_terminalStateUncertain{ false };
		std::atomic<std::uint64_t> g_correctionCandidates{ 0 };
		std::atomic<std::uint64_t> g_correctionAttempts{ 0 };
		std::atomic<std::uint64_t> g_correctedDraws{ 0 };
		std::atomic<std::uint64_t> g_correctionPassThroughs{ 0 };
		std::atomic<std::uint64_t> g_privateBindVerifications{ 0 };
		std::atomic<std::uint64_t> g_originalRestoreVerifications{ 0 };
		std::atomic<std::uint64_t> g_originalRestoreFailures{ 0 };
		std::atomic<std::uint64_t> g_resourceBuilds{ 0 };
		std::atomic<std::uint64_t> g_resourceReleases{ 0 };
		std::atomic<std::uint64_t> g_resourceEpoch{ 0 };
		std::atomic<std::uint64_t> g_protectionAttempts{ 0 };
		std::atomic<std::uint64_t> g_protectionEnables{ 0 };
		std::atomic<std::uint64_t> g_protectionEnters{ 0 };
		std::atomic<std::uint64_t> g_protectionLeaves{ 0 };
		std::atomic<std::uint32_t> g_lastCapturedCorrectedMask{ 0 };
		std::atomic<std::uint32_t> g_lastCapturedSkippedMask{ 0 };
		std::atomic<bool> g_lastCapturedMaskKnown{ false };
		std::atomic<bool> g_maskKnownForAllCorrectedDraws{ true };
		std::atomic<bool> g_completeReconstruction{ true };
		TechniqueObservationCapture g_techniqueObservationCaptures[kMaxTechniqueObservationSamples]{};
		std::size_t g_techniqueObservationCaptureCount = 0;
		std::uint32_t g_techniqueObservationThreadId = 0;
		std::uintptr_t g_techniqueObservationContext = 0;
		std::uintptr_t g_techniqueObservationDevice = 0;
		std::atomic<std::uint64_t> g_techniqueObservationEpoch{ 0 };
		std::atomic<std::uint64_t> g_techniqueObservationEligibleDraws{ 0 };
		std::atomic<std::uint64_t> g_techniqueObservationEligibilityDeclines{ 0 };
		std::atomic<std::uint64_t> g_techniqueObservationAttempts{ 0 };
		std::atomic<std::uint64_t> g_techniqueObservationSuccesses{ 0 };
		std::atomic<std::uint64_t> g_techniqueObservationFailures{ 0 };
		std::atomic<std::uint32_t> g_techniqueObservationLastFailure{ 0 };
		std::atomic<std::int32_t> g_techniqueObservationLastHr{ S_OK };
		std::atomic<std::uint32_t> g_techniqueObservationExceptionCode{ 0 };
		std::atomic<bool> g_technique65600CorrectionEnabled{ false };
		std::atomic<std::uint64_t> g_technique65600CorrectionEpoch{ 0 };
		CorrectionCapture g_technique65600CorrectionCaptures[kMaxTechnique65600CorrectionSamples]{};
		std::size_t g_technique65600CorrectionCaptureCount = 0;
		std::uint32_t g_technique65600CorrectionCaptureReservations = 0;
		Technique65600DimensionCache g_technique65600DimensionCache{};
		std::atomic<std::uint64_t> g_technique65600CorrectionCandidates{ 0 };
		std::atomic<std::uint64_t> g_technique65600CorrectionAttempts{ 0 };
		std::atomic<std::uint64_t> g_technique65600PrivateDraws{ 0 };
		std::atomic<std::uint64_t> g_technique65600CorrectedDraws{ 0 };
		std::atomic<std::uint64_t> g_technique65600CorrectionPassThroughs{ 0 };
		std::atomic<std::uint64_t> g_technique65600CorrectionDeclines{ 0 };
		std::atomic<std::uint32_t> g_technique65600CorrectionLastDecline{ 0 };
		std::atomic<std::int32_t> g_technique65600CorrectionLastHr{ S_OK };
		std::atomic<std::uint32_t> g_technique65600CorrectionExceptionCode{ 0 };
		std::atomic<std::uint64_t> g_technique65600PrivateBindVerifications{ 0 };
		std::atomic<std::uint64_t> g_technique65600OriginalRestoreVerifications{ 0 };
		std::atomic<std::uint64_t> g_technique65600OriginalRestoreFailures{ 0 };
		std::atomic<std::uint32_t> g_technique65600DimensionReadAttempts{ 0 };
		std::atomic<std::uint32_t> g_technique65600DimensionReadFailures{ 0 };
		std::atomic<std::uint32_t> g_technique65600CorrectionCaptureAttempts{ 0 };
		std::atomic<std::uint32_t> g_technique65600LastCorrectedMask{ 0 };
		std::atomic<std::uint32_t> g_technique65600LastSkippedMask{ 0 };
		std::atomic<bool> g_technique65600LastMaskKnown{ false };
		std::atomic<bool> g_technique65600ExpectedPartialReconstruction{ true };
		std::atomic<bool> g_technique65600MaskKnownForAllPrivateDraws{ true };

		void ResetTechnique65600CorrectionLocked() noexcept
		{
			g_technique65600CorrectionEpoch.fetch_add(1, std::memory_order_acq_rel);
			std::memset(
				g_technique65600CorrectionCaptures, 0,
				sizeof(g_technique65600CorrectionCaptures));
			g_technique65600CorrectionCaptureCount = 0;
			g_technique65600CorrectionCaptureReservations = 0;
			g_technique65600DimensionCache = {};
			g_technique65600CorrectionCandidates.store(0, std::memory_order_relaxed);
			g_technique65600CorrectionAttempts.store(0, std::memory_order_relaxed);
			g_technique65600PrivateDraws.store(0, std::memory_order_relaxed);
			g_technique65600CorrectedDraws.store(0, std::memory_order_relaxed);
			g_technique65600CorrectionPassThroughs.store(0, std::memory_order_relaxed);
			g_technique65600CorrectionDeclines.store(0, std::memory_order_relaxed);
			g_technique65600CorrectionLastDecline.store(0, std::memory_order_relaxed);
			g_technique65600CorrectionLastHr.store(S_OK, std::memory_order_relaxed);
			g_technique65600CorrectionExceptionCode.store(0, std::memory_order_relaxed);
			g_technique65600PrivateBindVerifications.store(0, std::memory_order_relaxed);
			g_technique65600OriginalRestoreVerifications.store(0, std::memory_order_relaxed);
			g_technique65600OriginalRestoreFailures.store(0, std::memory_order_relaxed);
			g_technique65600DimensionReadAttempts.store(0, std::memory_order_relaxed);
			g_technique65600DimensionReadFailures.store(0, std::memory_order_relaxed);
			g_technique65600CorrectionCaptureAttempts.store(0, std::memory_order_relaxed);
			g_technique65600LastCorrectedMask.store(0, std::memory_order_relaxed);
			g_technique65600LastSkippedMask.store(0, std::memory_order_relaxed);
			g_technique65600LastMaskKnown.store(false, std::memory_order_relaxed);
			g_technique65600ExpectedPartialReconstruction.store(true, std::memory_order_relaxed);
			g_technique65600MaskKnownForAllPrivateDraws.store(true, std::memory_order_relaxed);
		}

		void ResetTechniqueObservationLocked() noexcept
		{
			g_techniqueObservationEpoch.fetch_add(1, std::memory_order_acq_rel);
			std::memset(g_techniqueObservationCaptures, 0, sizeof(g_techniqueObservationCaptures));
			g_techniqueObservationCaptureCount = 0;
			g_techniqueObservationThreadId = 0;
			g_techniqueObservationContext = 0;
			g_techniqueObservationDevice = 0;
			g_techniqueObservationEligibleDraws.store(0, std::memory_order_relaxed);
			g_techniqueObservationEligibilityDeclines.store(0, std::memory_order_relaxed);
			g_techniqueObservationAttempts.store(0, std::memory_order_relaxed);
			g_techniqueObservationSuccesses.store(0, std::memory_order_relaxed);
			g_techniqueObservationFailures.store(0, std::memory_order_relaxed);
			g_techniqueObservationLastFailure.store(0, std::memory_order_relaxed);
			g_techniqueObservationLastHr.store(S_OK, std::memory_order_relaxed);
			g_techniqueObservationExceptionCode.store(0, std::memory_order_relaxed);
		}

		void ResetLocked() noexcept
		{
			std::memset(g_familyCoverage, 0, sizeof(g_familyCoverage));
			g_familyTableMatches = false;
			ResetTechnique65600CorrectionLocked();
			ResetTechniqueObservationLocked();
			std::memset(g_samples, 0, sizeof(g_samples));
			std::memset(g_routeSamples, 0, sizeof(g_routeSamples));
			std::memset(g_unknownPostCommits, 0, sizeof(g_unknownPostCommits));
			std::memset(g_knownNonCubeSamples, 0, sizeof(g_knownNonCubeSamples));
			std::memset(g_admittedPreliminarySamples, 0, sizeof(g_admittedPreliminarySamples));
			std::memset(g_knownOtherPreliminarySamples, 0, sizeof(g_knownOtherPreliminarySamples));
			std::memset(g_unknownPreliminarySamples, 0, sizeof(g_unknownPreliminarySamples));
			std::memset(g_pixelShaders, 0, sizeof(g_pixelShaders));
			g_sampleCount = 0;
			g_routeSampleCount = 0;
			g_unknownPostCommitCount = 0;
			g_knownNonCubeSampleCount = 0;
			g_admittedPreliminarySampleCount = 0;
			g_knownOtherPreliminarySampleCount = 0;
			g_unknownPreliminarySampleCount = 0;
			g_routeOverflow = 0;
			g_unknownPostCommitOverflow = 0;
			g_knownNonCubeOverflow = 0;
			g_knownNonCubeMatches = 0;
			g_preliminaryDrawSerial = 0;
			g_preliminaryAdmittedMatches = 0;
			g_preliminaryAdmittedReduced = 0;
			g_preliminaryAdmittedNative = 0;
			g_preliminaryAdmittedInvalidRatio = 0;
			g_preliminaryAdmittedOverflow = 0;
			g_rejectedKnownOtherReduced = 0;
			g_rejectedUnknownReduced = 0;
			g_rejectedNullReduced = 0;
			g_rejectedExceptionsReduced = 0;
			g_rejectedNativeRatio = 0;
			g_rejectedInvalidRatio = 0;
			g_rejectedCensusOverflow = 0;
			g_knownOtherPreliminaryOverflow = 0;
			g_unknownPreliminaryOverflow = 0;
			std::memset(g_rejectedOverflowByKind, 0, sizeof(g_rejectedOverflowByKind));
			g_tableSnapshotCapacity = 0;
			g_tableSnapshotRecords = 0;
			g_tableSnapshotFailures = 0;
			g_tableSnapshotComplete = false;
			g_tableSnapshotAttempted.store(false, std::memory_order_relaxed);
			g_nativeSamples = 0;
			g_reducedSamples = 0;
			g_pixelShaderCount = 0;
			g_captureFailures = 0;
			g_nativeCompleteLogged = false;
			g_reducedCompleteLogged = false;
			g_allSamplesValid = true;
			g_attempts.store(0, std::memory_order_relaxed);
			g_captures.store(0, std::memory_order_relaxed);
			g_restores.store(0, std::memory_order_relaxed);
			g_declines.store(0, std::memory_order_relaxed);
			g_lastDecline.store(0, std::memory_order_relaxed);
			g_exceptionCode.store(0, std::memory_order_relaxed);
			g_lastHr.store(0, std::memory_order_relaxed);
			g_lastRatioWidth.store(0.0f, std::memory_order_relaxed);
			g_lastRatioHeight.store(0.0f, std::memory_order_relaxed);
			g_bufferBytes.store(0, std::memory_order_relaxed);
			g_ready.store(false, std::memory_order_relaxed);
			g_drawRoutes.store(0, std::memory_order_relaxed);
			g_preparedPacketDrawRoutes.store(0, std::memory_order_relaxed);
			g_wrongCallerRoutes.store(0, std::memory_order_relaxed);
			g_compositeDraws.store(0, std::memory_order_relaxed);
			g_compositeShaders.store(0, std::memory_order_relaxed);
			std::memset(g_correctionCaptures, 0, sizeof(g_correctionCaptures));
			g_correctionCaptureCount = 0;
			g_correctionCandidates.store(0, std::memory_order_relaxed);
			g_correctionAttempts.store(0, std::memory_order_relaxed);
			g_correctedDraws.store(0, std::memory_order_relaxed);
			g_correctionPassThroughs.store(0, std::memory_order_relaxed);
			g_privateBindVerifications.store(0, std::memory_order_relaxed);
			g_originalRestoreVerifications.store(0, std::memory_order_relaxed);
			g_originalRestoreFailures.store(0, std::memory_order_relaxed);
			g_lastCapturedCorrectedMask.store(0, std::memory_order_relaxed);
			g_lastCapturedSkippedMask.store(0, std::memory_order_relaxed);
			g_lastCapturedMaskKnown.store(false, std::memory_order_relaxed);
			g_maskKnownForAllCorrectedDraws.store(true, std::memory_order_relaxed);
			g_completeReconstruction.store(true, std::memory_order_relaxed);
		}

		void ClearRegistryLocked() noexcept
		{
			for (std::size_t i = 0; i < g_registeredTechniqueCount; ++i) {
				SafeRelease(g_registeredTechniques[i].pixelShader);
			}
			std::memset(g_registeredTechniques, 0, sizeof(g_registeredTechniques));
			g_registeredTechniqueCount = 0;
			g_registryOverflow = 0;
			SafeRelease(g_registryDevice);
		}

		void RecordDecline(Decline a_reason, std::int32_t a_hr = 0, std::uint32_t a_exception = 0) noexcept
		{
			g_declines.fetch_add(1, std::memory_order_relaxed);
			g_lastDecline.store(static_cast<std::uint32_t>(a_reason), std::memory_order_relaxed);
			g_lastHr.store(a_hr, std::memory_order_relaxed);
			if (a_exception != 0) {
				g_exceptionCode.store(a_exception, std::memory_order_relaxed);
			}
		}

		void RecordFamilyDecline(
			PreparedDrawToken& a_token,
			Decline a_reason,
			std::int32_t a_hr = 0,
			std::uint32_t a_exception = 0) noexcept
		{
			a_token.coverageDecline = static_cast<std::uint32_t>(a_reason);
			RecordDecline(a_reason, a_hr, a_exception);
		}

		void RecordCaptureFailure(
			Decline a_reason,
			std::int32_t a_hr = 0,
			std::uint32_t a_exception = 0) noexcept
		{
			++g_captureFailures;
			RecordDecline(a_reason, a_hr, a_exception);
			if (g_captureFailures == 1 || g_captureFailures == kMaxCaptureFailures) {
				logger::warn(
					FMT_STRING("composite observation capture failure {}/{}: decline {}, HRESULT {:08X}, exception {:08X}"),
					g_captureFailures,
					kMaxCaptureFailures,
					static_cast<std::uint32_t>(a_reason),
					static_cast<std::uint32_t>(a_hr),
					a_exception);
			}
		}

		[[nodiscard]] std::uintptr_t EngineCameraBuffer() noexcept
		{
			const auto state = Engine::GraphicsState();
			void* buffer = nullptr;
			if (state == 0 ||
				!Engine::ReadPtr(state + Addr::kStateGlobalCameraConstantBuffer, buffer)) {
				return 0;
			}
			return reinterpret_cast<std::uintptr_t>(buffer);
		}

		void ReleaseBoundState(BoundState& a_state) noexcept
		{
			SafeRelease(a_state.cb12);
			SafeRelease(a_state.cb8);
			SafeRelease(a_state.cb2);
			SafeRelease(a_state.t8);
			SafeRelease(a_state.pixelShader);
			SafeRelease(a_state.device);
		}

		void ReleaseTechniqueObservationBoundState(TechniqueObservationBoundState& a_state) noexcept
		{
			SafeRelease(a_state.cb12);
			SafeRelease(a_state.cb8);
			SafeRelease(a_state.cb2);
			SafeRelease(a_state.pixelShader);
			SafeRelease(a_state.device);
		}

		[[nodiscard]] DeviceRetainResult RetainDeviceAndPixelShader(
			ID3D11DeviceContext* a_context,
			ID3D11PixelShader* a_pixelShader) noexcept
		{
			DeviceRetainResult result{};
			ID3D11Device* pixelShaderDevice = nullptr;
			__try {
				a_context->GetDevice(&result.device);
				a_pixelShader->GetDevice(&pixelShaderDevice);
				if (result.device && pixelShaderDevice == result.device) {
					a_pixelShader->AddRef();
					result.pixelShader = a_pixelShader;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			SafeRelease(pixelShaderDevice);
			return result;
		}

		[[nodiscard]] PixelShaderQuery QueryPixelShaderIsolated(
			ID3D11DeviceContext* a_context) noexcept
		{
			PixelShaderQuery result{};
			__try {
				a_context->PSGetShader(&result.pixelShader, nullptr, nullptr);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			return result;
		}

		[[nodiscard]] BoundState QueryBoundStateIsolated(ID3D11DeviceContext* a_context) noexcept
		{
			BoundState state{};
			ID3D11DeviceContext1* context1 = nullptr;
			ID3D11Multithread* multithread = nullptr;
			ID3D11RenderTargetView* targetView = nullptr;
			ID3D11Resource* targetResource = nullptr;
			ID3D11Texture2D* targetTexture = nullptr;
			__try {
				state.contextType = static_cast<std::uint32_t>(a_context->GetType());
				a_context->GetDevice(&state.device);
				if (SUCCEEDED(a_context->QueryInterface(
						__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread))) &&
					multithread) {
					state.multithreadInterface = true;
					state.multithreadProtected = multithread->GetMultithreadProtected() != FALSE;
				}
				a_context->PSGetShader(&state.pixelShader, nullptr, nullptr);
				a_context->PSGetShaderResources(kCubeArraySlot, 1, &state.t8);
				if (state.t8) {
					D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
					state.t8->GetDesc(&desc);
					state.t8ViewDimension = static_cast<std::uint32_t>(desc.ViewDimension);
				}

				const HRESULT queryHr = a_context->QueryInterface(
					__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));
				if (SUCCEEDED(queryHr) && context1) {
					const UINT slots[3] = { kPerDrawSlot, kStereoSlot, kCameraSlot };
					ID3D11Buffer** buffers[3] = { &state.cb2, &state.cb8, &state.cb12 };
					for (std::size_t i = 0; i < 3; ++i) {
						context1->PSGetConstantBuffers1(
							slots[i], 1, buffers[i],
							&state.firstConstant[i], &state.numConstants[i]);
					}
					state.rangeKnown = true;
				} else {
					a_context->PSGetConstantBuffers(kPerDrawSlot, 1, &state.cb2);
					a_context->PSGetConstantBuffers(kStereoSlot, 1, &state.cb8);
					a_context->PSGetConstantBuffers(kCameraSlot, 1, &state.cb12);
				}

				UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
				a_context->RSGetViewports(&viewportCount, state.viewports);
				state.viewportCount = viewportCount;
				a_context->OMGetRenderTargets(1, &targetView, nullptr);
				if (targetView) {
					D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
					targetView->GetDesc(&viewDesc);
					targetView->GetResource(&targetResource);
					if (targetResource && viewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D &&
						viewDesc.Texture2D.MipSlice == 0 &&
						SUCCEEDED(targetResource->QueryInterface(
							__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&targetTexture))) && targetTexture) {
						D3D11_TEXTURE2D_DESC desc{};
						targetTexture->GetDesc(&desc);
						if (desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && (desc.Width & 1u) == 0) {
							state.targetWidth = desc.Width;
							state.targetHeight = desc.Height;
						}
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				state.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			SafeRelease(targetTexture);
			SafeRelease(targetResource);
			SafeRelease(targetView);
			SafeRelease(multithread);
			SafeRelease(context1);
			return state;
		}

		[[nodiscard]] TechniqueObservationBoundState QueryTechniqueObservationStateIsolated(
			ID3D11DeviceContext* a_context) noexcept
		{
			TechniqueObservationBoundState state{};
			ID3D11DeviceContext1* context1 = nullptr;
			ID3D11Multithread* multithread = nullptr;
			ID3D11ShaderResourceView* views[kObservedResourceCount]{};
			ID3D11Resource* resources[kObservedResourceCount]{};
			ID3D11Texture2D* textures[kObservedResourceCount]{};
			__try {
				state.contextType = static_cast<std::uint32_t>(a_context->GetType());
				a_context->GetDevice(&state.device);
				if (SUCCEEDED(a_context->QueryInterface(
						__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread))) &&
					multithread) {
					state.multithreadInterface = true;
					state.multithreadProtected = multithread->GetMultithreadProtected() != FALSE;
				}
				a_context->PSGetShader(&state.pixelShader, nullptr, nullptr);
				const HRESULT queryHr = a_context->QueryInterface(
					__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));
				if (SUCCEEDED(queryHr) && context1) {
					const UINT slots[3] = { kPerDrawSlot, kStereoSlot, kCameraSlot };
					ID3D11Buffer** buffers[3] = { &state.cb2, &state.cb8, &state.cb12 };
					for (std::size_t i = 0; i < 3; ++i) {
						context1->PSGetConstantBuffers1(
							slots[i], 1, buffers[i],
							&state.firstConstant[i], &state.numConstants[i]);
					}
					state.rangeKnown = true;
				}

				UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
				a_context->RSGetViewports(&viewportCount, state.viewports);
				state.viewportCount = viewportCount;

				for (std::size_t i = 0; i < kObservedResourceCount; ++i) {
					auto& capture = state.resources[i];
					capture.slot = kObservedResourceSlots[i];
					a_context->PSGetShaderResources(capture.slot, 1, &views[i]);
					if (!views[i]) {
						continue;
					}
					capture.present = true;
					capture.viewIdentity = reinterpret_cast<std::uintptr_t>(views[i]);
					D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
					views[i]->GetDesc(&viewDesc);
					capture.viewFormat = static_cast<std::uint32_t>(viewDesc.Format);
					capture.viewDimension = static_cast<std::uint32_t>(viewDesc.ViewDimension);
					views[i]->GetResource(&resources[i]);
					if (!resources[i]) {
						continue;
					}
					capture.resourceIdentity = reinterpret_cast<std::uintptr_t>(resources[i]);
					D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
					resources[i]->GetType(&dimension);
					capture.resourceDimension = static_cast<std::uint32_t>(dimension);
					if (SUCCEEDED(resources[i]->QueryInterface(
							__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&textures[i]))) &&
						textures[i]) {
						capture.texture2D = true;
						D3D11_TEXTURE2D_DESC textureDesc{};
						textures[i]->GetDesc(&textureDesc);
						capture.width = textureDesc.Width;
						capture.height = textureDesc.Height;
						capture.mipLevels = textureDesc.MipLevels;
						capture.arraySize = textureDesc.ArraySize;
						capture.format = static_cast<std::uint32_t>(textureDesc.Format);
						capture.sampleCount = textureDesc.SampleDesc.Count;
						capture.sampleQuality = textureDesc.SampleDesc.Quality;
						capture.usage = static_cast<std::uint32_t>(textureDesc.Usage);
						capture.bindFlags = textureDesc.BindFlags;
						capture.cpuAccessFlags = textureDesc.CPUAccessFlags;
						capture.miscFlags = textureDesc.MiscFlags;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				state.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			for (std::size_t i = 0; i < kObservedResourceCount; ++i) {
				SafeRelease(textures[i]);
				SafeRelease(resources[i]);
				SafeRelease(views[i]);
			}
			SafeRelease(multithread);
			SafeRelease(context1);
			return state;
		}

		[[nodiscard]] ReadbackResult ReadBufferIsolated(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			ID3D11Buffer* a_source,
			BufferCapture& a_out) noexcept
		{
			ReadbackResult result{};
			ID3D11Buffer* staging = nullptr;
			bool mapped = false;
			__try {
				D3D11_BUFFER_DESC desc{};
				a_source->GetDesc(&desc);
				a_out.identity = reinterpret_cast<std::uintptr_t>(a_source);
				a_out.byteWidth = desc.ByteWidth;
				a_out.usage = static_cast<std::uint32_t>(desc.Usage);
				a_out.bindFlags = desc.BindFlags;
				a_out.cpuAccessFlags = desc.CPUAccessFlags;
				if (desc.ByteWidth == 0 || desc.ByteWidth > kMaxBufferBytes) {
					result.hr = E_INVALIDARG;
				} else {
					D3D11_BUFFER_DESC stagingDesc = desc;
					stagingDesc.Usage = D3D11_USAGE_STAGING;
					stagingDesc.BindFlags = 0;
					stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					stagingDesc.MiscFlags = 0;
					stagingDesc.StructureByteStride = 0;
					result.hr = a_device->CreateBuffer(&stagingDesc, nullptr, &staging);
					if (SUCCEEDED(result.hr) && staging) {
						a_context->CopyResource(staging, a_source);
						D3D11_MAPPED_SUBRESOURCE readback{};
						result.hr = a_context->Map(staging, 0, D3D11_MAP_READ, 0, &readback);
						if (SUCCEEDED(result.hr) && readback.pData) {
							mapped = true;
							std::memcpy(a_out.data, readback.pData, desc.ByteWidth);
							a_out.dataBytes = desc.ByteWidth;
							a_context->Unmap(staging, 0);
							mapped = false;
							result.copied = true;
						}
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (mapped && staging) {
				__try {
					a_context->Unmap(staging, 0);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					if (result.exceptionCode == 0) {
						result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
					}
				}
			}
			SafeRelease(staging);
			return result;
		}

		void NotePixelShader(std::uintptr_t a_shader) noexcept
		{
			if (a_shader == 0 ||
				std::find(g_pixelShaders, g_pixelShaders + g_pixelShaderCount, a_shader) !=
					g_pixelShaders + g_pixelShaderCount) {
				return;
			}
			if (g_pixelShaderCount < kMaxPixelShaders) {
				g_pixelShaders[g_pixelShaderCount++] = a_shader;
				g_compositeShaders.store(
					static_cast<std::uint32_t>(g_pixelShaderCount), std::memory_order_relaxed);
			}
		}

		[[nodiscard]] std::string HexBytes(const std::uint8_t* a_data, std::size_t a_size)
		{
			std::string out;
			out.reserve(a_size * 2);
			for (std::size_t i = 0; i < a_size; ++i) {
				out += fmt::format(FMT_STRING("{:02X}"), a_data[i]);
			}
			return out;
		}

		[[nodiscard]] std::string JsonFloat(float a_value)
		{
			return std::isfinite(a_value) ?
				fmt::format(FMT_STRING("{:.9g}"), a_value) : "null";
		}

		void AppendFloat4(std::string& a_out, const std::uint8_t* a_data) noexcept
		{
			float values[4]{};
			std::memcpy(values, a_data, sizeof(values));
			a_out += fmt::format(
				FMT_STRING("[{},{},{},{}]"),
				JsonFloat(values[0]), JsonFloat(values[1]),
				JsonFloat(values[2]), JsonFloat(values[3]));
		}

		void AppendUint4(std::string& a_out, const std::uint8_t* a_data) noexcept
		{
			std::uint32_t values[4]{};
			std::memcpy(values, a_data, sizeof(values));
			a_out += fmt::format(
				FMT_STRING("[{},{},{},{}]"),
				values[0], values[1], values[2], values[3]);
		}

		[[nodiscard]] std::size_t EffectiveByteOffset(const BufferCapture& a_buffer, std::size_t a_shaderRow) noexcept
		{
			return (static_cast<std::size_t>(a_buffer.firstConstant) + a_shaderRow) *
			       kBytesPerConstant;
		}

		[[nodiscard]] bool EffectiveRangeCovers(
			const BufferCapture& a_buffer,
			std::size_t a_firstShaderRow,
			std::size_t a_rowCount) noexcept
		{
			return a_buffer.rangeKnown &&
			       a_buffer.numConstants >= a_firstShaderRow + a_rowCount &&
			       EffectiveByteOffset(a_buffer, a_firstShaderRow) +
				       a_rowCount * kBytesPerConstant <=
				       a_buffer.dataBytes;
		}

		void AppendBufferJson(
			std::string& a_out,
			const BufferCapture& a_buffer,
			bool a_includeReconstruction) noexcept
		{
			a_out += fmt::format(
				FMT_STRING(R"({{"identity":"{:016X}","byteWidth":{},"usage":{},"bindFlags":{},"cpuAccessFlags":{},"rangeKnown":{},"firstConstant":{},"numConstants":{},"fullHex":"{}")"),
				a_buffer.identity,
				a_buffer.byteWidth,
				a_buffer.usage,
				a_buffer.bindFlags,
				a_buffer.cpuAccessFlags,
				a_buffer.rangeKnown ? "true"sv : "false"sv,
				a_buffer.firstConstant,
				a_buffer.numConstants,
				HexBytes(a_buffer.data, a_buffer.dataBytes));

			const auto effectiveRow0 = EffectiveByteOffset(a_buffer, 0);
			if (effectiveRow0 + kBytesPerConstant <= a_buffer.dataBytes) {
				a_out += R"(,"effectiveRow0Float":)";
				AppendFloat4(a_out, a_buffer.data + effectiveRow0);
				a_out += R"(,"effectiveRow0Uint":)";
				AppendUint4(a_out, a_buffer.data + effectiveRow0);
			}
			if (a_includeReconstruction) {
				const auto physicalOffset = kFirstReconstructionRow * kBytesPerConstant;
				const auto reconstructionBytes =
					kReconstructionRowCount * kBytesPerConstant;
				if (physicalOffset + reconstructionBytes <= a_buffer.dataBytes) {
					a_out += fmt::format(
						FMT_STRING(R"(,"physicalRows32To47Hex":"{}")"),
						HexBytes(a_buffer.data + physicalOffset, reconstructionBytes));
				}
				const auto effectiveOffset =
					EffectiveByteOffset(a_buffer, kFirstReconstructionRow);
				if (effectiveOffset + reconstructionBytes <= a_buffer.dataBytes) {
					a_out += fmt::format(
						FMT_STRING(R"(,"effectiveRows32To47Hex":"{}","effectiveRows32To47Float":[)"),
						HexBytes(a_buffer.data + effectiveOffset, reconstructionBytes));
					for (std::size_t row = 0; row < kReconstructionRowCount; ++row) {
						if (row != 0) {
							a_out += ',';
						}
						AppendFloat4(
							a_out,
							a_buffer.data + effectiveOffset + row * kBytesPerConstant);
					}
					a_out += ']';
				}
			}
			a_out += '}';
		}

		void AppendShaderResourceJson(
			std::string& a_out,
			const ShaderResourceCapture& a_resource)
		{
			a_out += fmt::format(
				FMT_STRING(R"({{"slot":{},"present":{},"viewIdentity":"{:016X}","viewFormat":{},"viewDimension":{},"resourceIdentity":"{:016X}","resourceDimension":{},"texture2D":{},"texture2DDesc":{{"width":{},"height":{},"mipLevels":{},"arraySize":{},"format":{},"sampleCount":{},"sampleQuality":{},"usage":{},"bindFlags":{},"cpuAccessFlags":{},"miscFlags":{}}}}})"),
				a_resource.slot,
				a_resource.present ? "true"sv : "false"sv,
				a_resource.viewIdentity,
				a_resource.viewFormat,
				a_resource.viewDimension,
				a_resource.resourceIdentity,
				a_resource.resourceDimension,
				a_resource.texture2D ? "true"sv : "false"sv,
				a_resource.width,
				a_resource.height,
				a_resource.mipLevels,
				a_resource.arraySize,
				a_resource.format,
				a_resource.sampleCount,
				a_resource.sampleQuality,
				a_resource.usage,
				a_resource.bindFlags,
				a_resource.cpuAccessFlags,
				a_resource.miscFlags);
		}

		[[nodiscard]] std::string ObservationsJsonLocked()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < g_sampleCount; ++i) {
				const auto& sample = g_samples[i];
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"({{"route":"preparedPacketDraw","serial":{},"threadId":{},"reduced":{},"callerReturn":"{:016X}","callerReturnRva":"{:X}","packet":"{:016X}","packetDrawCount":{},"pass":"{:016X}","shaderState":"{:016X}","engineShader":"{:016X}","engineShaderVtable":"{:016X}","technique":{},"vertexTechnique":{},"pixelTechnique":{},"pixelShaderRecord":"{:016X}","expectedPixelShader":"{:016X}","boundPixelShader":"{:016X}","pixelShaderMatchesRecord":{},"context":"{:016X}","contextType":{},"multithreadInterface":{},"multithreadProtected":{},"registryDeviceMatches":{},"engineCameraBuffer":"{:016X}","cb12MatchesEngine":{},"t8ViewDimension":{},"t8TextureCubeArray":{},"ratios":[{},{}],"viewports":[)"),
					sample.serial,
					sample.threadId,
					sample.reduced ? "true"sv : "false"sv,
					sample.callerReturn,
					sample.callerReturnRva,
					sample.packet,
					sample.packetDrawCount,
					sample.pass,
					sample.shaderState,
					sample.engineShader,
					sample.engineShaderVtable,
					sample.technique,
					sample.vertexTechnique,
					sample.pixelTechnique,
					sample.pixelShaderRecord,
					sample.expectedPixelShader,
					sample.boundPixelShader,
					sample.pixelShaderMatchesRecord ? "true"sv : "false"sv,
					sample.context,
					sample.contextType,
					sample.multithreadInterface ? "true"sv : "false"sv,
					sample.multithreadProtected ? "true"sv : "false"sv,
					sample.registryDeviceMatches ? "true"sv : "false"sv,
					sample.engineCameraBuffer,
					sample.cb12MatchesEngine ? "true"sv : "false"sv,
					sample.t8ViewDimension,
					sample.t8ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY ? "true"sv : "false"sv,
					JsonFloat(sample.ratioWidth),
					JsonFloat(sample.ratioHeight));
				for (std::uint32_t viewport = 0; viewport < sample.viewportCount; ++viewport) {
					if (viewport != 0) {
						out += ',';
					}
					const auto& v = sample.viewports[viewport];
					out += fmt::format(
						FMT_STRING("[{},{},{},{},{},{}]"),
						JsonFloat(v.TopLeftX), JsonFloat(v.TopLeftY),
						JsonFloat(v.Width), JsonFloat(v.Height),
						JsonFloat(v.MinDepth), JsonFloat(v.MaxDepth));
				}
				out += R"(],"cb2":)";
				AppendBufferJson(out, sample.cb2, false);
				out += R"(,"cb8":)";
				AppendBufferJson(out, sample.cb8, false);
				out += R"(,"cb12":)";
				AppendBufferJson(out, sample.cb12, true);
				out += '}';
			}
			out += ']';
			return out;
		}

		[[nodiscard]] std::string RouteObservationsJsonLocked()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < g_routeSampleCount; ++i) {
				const auto& sample = g_routeSamples[i];
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"({{"threadId":{},"pass":"{:016X}","shaderState":"{:016X}","engineShader":"{:016X}","technique":{},"shape":"{:016X}","shapeReadable":{},"geometryType":{},"hasPassOverride":{},"pixelShaderRecord":"{:016X}","expectedPixelShader":"{:016X}","ratios":[{},{}],"count":{}}})"),
					sample.threadId,
					sample.pass,
					sample.shaderState,
					sample.engineShader,
					sample.technique,
					sample.shape,
					sample.shapeReadable ? "true"sv : "false"sv,
					sample.geometryType,
					sample.hasPassOverride ? "true"sv : "false"sv,
					sample.pixelShaderRecord,
					sample.expectedPixelShader,
					JsonFloat(sample.ratioWidth),
					JsonFloat(sample.ratioHeight),
					sample.count);
			}
			out += ']';
			return out;
		}

		[[nodiscard]] std::string FamilyCoverageJsonLocked()
		{
			constexpr const char* reasons[] = {
				"none", "noPass", "notComposite", "noContext", "ratiosUnreadable",
				"bindingsMissing", "bufferSize", "readbackFailed", "wrongCaller",
				"wrongTechnique", "nativeRatio", "unsupportedContext", "protectionFailed",
				"resourceBuildFailed", "privatePreparationFailed", "privateBindFailed",
				"originalRestoreFailed", "stateUncertain", "stereoBasisFailed",
				"outsideCameraContract", "familyDisabled", "staleEpisode", "tableMismatch",
				"notPrepared"
			};
			constexpr const char* legacyReasons[] = {
				"none", "protectionUnavailable", "wrongRenderThread", "ownerIdentityMismatch",
				"bindingsMissing", "shaderIdentityMismatch", "bufferContract", "viewportContract",
				"nativePassThrough", "dimensionReadFailed", "dimensionCacheStale",
				"privatePreparationFailed", "privateBindFailed", "originalRestoreFailed",
				"stateUncertain", "staleEpisode", "evidenceReadFailed"
			};
			std::string out = fmt::format(
				FMT_STRING(R"({{"generation":{},"enabled":{},"recordCount":{},"tableMatches":{},"liveBytecodeVerified":false,"records":[)"),
				g_generation.load(std::memory_order_relaxed), Enabled() ? "true"sv : "false"sv,
				CompositeContracts::kRecordCount, g_familyTableMatches ? "true"sv : "false"sv);
			for (std::size_t i = 0; i <= CompositeContracts::kRecordCount; ++i) {
				const auto& row = g_familyCoverage[i];
				const bool known = i < CompositeContracts::kRecordCount;
				if (i != 0) {
					out += ',';
				}
				const auto kind = !known ? "unknown"sv :
					CompositeContracts::kRecords[i].kind == CompositeContracts::Kind::kCube ? "cubemap"sv :
					CompositeContracts::kRecords[i].kind == CompositeContracts::Kind::kCamera ? "camera"sv : "outsideCameraContract"sv;
				out += fmt::format(
					FMT_STRING(R"({{"technique":{},"kind":"{}","observed":{},"native":{},"reduced":{},"invalidRatio":{},"admitted":{},"finished":{},"privateDraws":{},"restored":{},"restoreFailures":{},"declines":{{)"),
					known ? std::to_string(CompositeContracts::kRecords[i].technique) : "null",
					kind, row.observed, row.native, row.reduced, row.invalidRatio, row.admitted,
					row.finished, row.privateDraws, row.restored, row.restoreFailures);
				bool first = true;
				for (std::size_t reason = 1; reason < std::size(reasons); ++reason) {
					if (row.declines[reason] != 0) {
						out += fmt::format(FMT_STRING(R"({}"{}":{})"), first ? ""sv : ","sv, reasons[reason], row.declines[reason]);
						first = false;
					}
				}
				out += R"(},"technique65600Declines":{)";
				first = true;
				for (std::size_t reason = 1; reason < std::size(legacyReasons); ++reason) {
					if (row.legacyDeclines[reason] != 0) {
						out += fmt::format(FMT_STRING(R"({}"{}":{})"), first ? ""sv : ","sv, legacyReasons[reason], row.legacyDeclines[reason]);
						first = false;
					}
				}
				out += "}}";
			}
			out += "]}";
			return out;
		}

		[[nodiscard]] std::string RegistryRecordsJsonLocked()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < g_registeredTechniqueCount; ++i) {
				const auto& record = g_registeredTechniques[i];
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"({{"source":"dfCompositePixelTable","engineShader":"{:016X}","shaderState":"{:016X}","pixelShaderRecord":"{:016X}","pixelShader":"{:016X}","vertexTechnique":{},"pixelTechnique":{},"enrollments":{}}})"),
					record.engineShader,
					record.shaderState,
					record.pixelShaderRecord,
					reinterpret_cast<std::uintptr_t>(record.pixelShader),
					record.vertexTechnique,
					record.pixelTechnique,
					record.enrollments);
			}
			out += ']';
			return out;
		}

		[[nodiscard]] std::string UnknownPostCommitsJsonLocked()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < g_unknownPostCommitCount; ++i) {
				const auto& sample = g_unknownPostCommits[i];
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"({{"route":"preparedPacketDraw","classification":"unknown","callerReturn":"{:016X}","callerReturnRva":"{:X}","boundPixelShader":"{:016X}","threadId":{},"count":{}}})"),
					sample.callerReturn,
					sample.callerReturnRva,
					sample.boundPixelShader,
					sample.threadId,
					sample.count);
			}
			out += ']';
			return out;
		}

		[[nodiscard]] std::string KnownNonCubeJsonLocked()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < g_knownNonCubeSampleCount; ++i) {
				const auto& sample = g_knownNonCubeSamples[i];
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"({{"route":"preparedPacketDraw","classification":"knownCompositeNonCube","callerReturn":"{:016X}","callerReturnRva":"{:X}","pixelShaderRecord":"{:016X}","boundPixelShader":"{:016X}","vertexTechnique":{},"pixelTechnique":{},"t8ViewDimension":{},"threadId":{},"count":{}}})"),
					sample.callerReturn,
					sample.callerReturnRva,
					sample.pixelShaderRecord,
					sample.boundPixelShader,
					sample.vertexTechnique,
					sample.pixelTechnique,
					sample.t8ViewDimension,
					sample.threadId,
					sample.count);
			}
			out += ']';
			return out;
		}

		[[nodiscard]] bool MergeRegisteredRecord(
			ID3D11DeviceContext* a_context,
			std::uintptr_t a_shader,
			std::uintptr_t a_shaderState,
			std::uint32_t a_vertexTechnique,
			std::uint32_t a_pixelTechnique,
			std::uintptr_t a_record,
			ID3D11PixelShader* a_pixelShader) noexcept
		{
			auto retained = RetainDeviceAndPixelShader(a_context, a_pixelShader);
			if (retained.exceptionCode != 0) {
				g_exceptionCode.store(retained.exceptionCode, std::memory_order_relaxed);
			}
			if (!retained.device || !retained.pixelShader) {
				SafeRelease(retained.pixelShader);
				SafeRelease(retained.device);
				return false;
			}

			bool merged = false;
			{
				const std::lock_guard<std::mutex> guard{ g_lock };
				if (g_registryDevice && g_registryDevice != retained.device) {
					ClearRegistryLocked();
					ResetLocked();
					logger::info(
						"composite record registry rebuilt after D3D device identity changed"sv);
				}
				if (!g_registryDevice) {
					g_registryDevice = retained.device;
					retained.device = nullptr;
				}

				for (std::size_t i = 0; i < g_registeredTechniqueCount; ++i) {
					auto& present = g_registeredTechniques[i];
					if (present.pixelShaderRecord != a_record) {
						continue;
					}
					present.engineShader = a_shader;
					if (a_shaderState != 0) {
						present.shaderState = a_shaderState;
					}
					present.vertexTechnique = a_vertexTechnique;
					present.pixelTechnique = a_pixelTechnique;
					if (present.pixelShader != retained.pixelShader) {
						SafeRelease(present.pixelShader);
						present.pixelShader = retained.pixelShader;
						retained.pixelShader = nullptr;
					}
					if (present.enrollments != (std::numeric_limits<std::uint64_t>::max)()) {
						++present.enrollments;
					}
					merged = true;
					break;
				}

				if (!merged && g_registeredTechniqueCount < kMaxRegisteredTechniques) {
					auto& added = g_registeredTechniques[g_registeredTechniqueCount++];
					added.engineShader = a_shader;
					added.shaderState = a_shaderState;
					added.pixelShaderRecord = a_record;
					added.pixelShader = retained.pixelShader;
					retained.pixelShader = nullptr;
					added.vertexTechnique = a_vertexTechnique;
					added.pixelTechnique = a_pixelTechnique;
					added.enrollments = 1;
					merged = true;
				} else if (!merged &&
					g_registryOverflow != (std::numeric_limits<std::uint32_t>::max)()) {
					++g_registryOverflow;
				}
			}
			SafeRelease(retained.pixelShader);
			SafeRelease(retained.device);
			return merged;
		}

		void RecordTableSnapshotFailure() noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (g_tableSnapshotFailures != (std::numeric_limits<std::uint32_t>::max)()) {
				++g_tableSnapshotFailures;
			}
			g_tableSnapshotComplete = false;
		}

		void EnsureCompositePixelTableSnapshot(ID3D11DeviceContext* a_context) noexcept
		{
			if (g_tableSnapshotAttempted.load(std::memory_order_acquire) ||
				g_tableSnapshotInProgress.exchange(true, std::memory_order_acq_rel)) {
				return;
			}

			const REL::Relocation<std::uintptr_t> singletonSlot{
				REL::Offset(Addr::kCompositeShaderSingleton)
			};
			const REL::Relocation<std::uintptr_t> compositeVtable{
				REL::Offset(Addr::kCompositeShaderVtable)
			};
			void* shaderPtr = nullptr;
			void* vtablePtr = nullptr;
			void* tablePtr = nullptr;
			void* sentinelPtr = nullptr;
			std::uint32_t capacity = 0;
			std::uint32_t freeCount = 0;
			if (!Engine::ReadPtr(singletonSlot.address(), shaderPtr) || shaderPtr == nullptr ||
				!Engine::ReadPtr(reinterpret_cast<std::uintptr_t>(shaderPtr), vtablePtr) ||
				reinterpret_cast<std::uintptr_t>(vtablePtr) != compositeVtable.address() ||
				!Engine::ReadPtr(
					reinterpret_cast<std::uintptr_t>(shaderPtr) + Addr::kShaderPixelTableOffset,
					tablePtr) || tablePtr == nullptr ||
				!FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(
						reinterpret_cast<std::uintptr_t>(shaderPtr) +
						Addr::kShaderPixelTableCapacityOffset),
					&capacity, sizeof(capacity)) ||
				!FaultGuard::SafeReadBytes(
					reinterpret_cast<const void*>(
						reinterpret_cast<std::uintptr_t>(shaderPtr) +
						Addr::kShaderPixelTableFreeCountOffset),
					&freeCount, sizeof(freeCount)) ||
				!Engine::ReadPtr(
					reinterpret_cast<std::uintptr_t>(shaderPtr) +
						Addr::kShaderPixelTableSentinelOffset,
					sentinelPtr) || sentinelPtr == nullptr ||
				capacity == 0 || capacity > kMaxPixelTableBuckets || freeCount > capacity ||
				(capacity & (capacity - 1)) != 0) {
			RecordTableSnapshotFailure();
			g_tableSnapshotAttempted.store(true, std::memory_order_release);
			g_tableSnapshotInProgress.store(false, std::memory_order_release);
			return;
		}

			std::uintptr_t seenRecords[kMaxPixelTableNodes]{};
			std::size_t seenRecordCount = 0;
			bool complete = true;
			const auto shader = reinterpret_cast<std::uintptr_t>(shaderPtr);
			const auto table = reinterpret_cast<std::uintptr_t>(tablePtr);
			const auto sentinel = reinterpret_cast<std::uintptr_t>(sentinelPtr);
			const auto tableEnd = table + static_cast<std::uintptr_t>(capacity) * 16;
			for (std::size_t slot = 0; slot < capacity && complete; ++slot) {
				const auto node = table + slot * 16;
				void* recordPtr = nullptr;
				void* nextPtr = nullptr;
				if (!Engine::ReadPtr(node, recordPtr) ||
					!Engine::ReadPtr(node + 8, nextPtr)) {
					complete = false;
					break;
				}
				if (nextPtr == nullptr) {
					continue;
				}
				const auto record = reinterpret_cast<std::uintptr_t>(recordPtr);
				const auto next = reinterpret_cast<std::uintptr_t>(nextPtr);
				const bool nextIsSlot = next >= table && next < tableEnd &&
					((next - table) % 16) == 0;
				if (record == 0 || (next != sentinel && !nextIsSlot) ||
					std::find(seenRecords, seenRecords + seenRecordCount, record) !=
						seenRecords + seenRecordCount ||
					seenRecordCount >= kMaxPixelTableNodes) {
					complete = false;
					break;
				}
				std::uint32_t pixelTechnique = 0;
				void* pixelShaderPtr = nullptr;
				if (!FaultGuard::SafeReadBytes(
						reinterpret_cast<const void*>(record),
						&pixelTechnique, sizeof(pixelTechnique)) ||
					!Engine::ReadPtr(
						record + Addr::kShaderRecordD3DObjectOffset,
						pixelShaderPtr) || pixelShaderPtr == nullptr) {
					complete = false;
					break;
				}
				seenRecords[seenRecordCount++] = record;
				if (!MergeRegisteredRecord(
						a_context, shader, 0, 0, pixelTechnique, record,
						reinterpret_cast<ID3D11PixelShader*>(pixelShaderPtr))) {
					complete = false;
					break;
				}
			}
			complete = complete && seenRecordCount == capacity - freeCount;

			{
				const std::lock_guard<std::mutex> guard{ g_lock };
				g_tableSnapshotCapacity = capacity;
				g_tableSnapshotRecords = static_cast<std::uint32_t>(seenRecordCount);
				g_tableSnapshotComplete = complete && seenRecordCount != 0;
				g_familyTableMatches = g_tableSnapshotComplete &&
					g_registeredTechniqueCount == CompositeContracts::kRecordCount;
				for (std::size_t i = 0; i < g_registeredTechniqueCount && g_familyTableMatches; ++i) {
					const auto id = g_registeredTechniques[i].pixelTechnique;
					g_familyTableMatches = CompositeContracts::Find(id) != nullptr;
					for (std::size_t previous = 0; previous < i; ++previous) {
						if (g_registeredTechniques[previous].pixelTechnique == id) {
							g_familyTableMatches = false;
						}
					}
				}
				if (!g_tableSnapshotComplete &&
					g_tableSnapshotFailures != (std::numeric_limits<std::uint32_t>::max)()) {
					++g_tableSnapshotFailures;
				}
			}
			g_tableSnapshotAttempted.store(true, std::memory_order_release);
			g_tableSnapshotInProgress.store(false, std::memory_order_release);
		}

		void ResetTableRegistryLocked() noexcept
		{
			g_familyTableMatches = false;
			ClearRegistryLocked();
			g_tableSnapshotCapacity = 0;
			g_tableSnapshotRecords = 0;
			g_tableSnapshotFailures = 0;
			g_tableSnapshotComplete = false;
			g_tableSnapshotAttempted.store(false, std::memory_order_release);
			g_tableSnapshotInProgress.store(false, std::memory_order_release);
		}

		void ReleaseCorrectionResources() noexcept
		{
			const bool hadResources = g_correctionDevice || g_correctionContext ||
				g_correctionShaders[0] || g_correctionShaders[1] || g_correctionParameters || g_correctedRaw ||
				g_finiteMask || g_privateCamera;
			g_correctionIsolation.Shutdown();
			SafeRelease(g_privateCamera);
			SafeRelease(g_finiteMaskUav);
			SafeRelease(g_finiteMask);
			SafeRelease(g_correctedRawUav);
			SafeRelease(g_correctedRaw);
			SafeRelease(g_correctionParameters);
			for (auto*& shader : g_correctionShaders) {
				SafeRelease(shader);
			}
			SafeRelease(g_correctionContext1);
			SafeRelease(g_correctionContext);
			SafeRelease(g_correctionDevice);
			g_correctionBufferBytes = 0;
			g_correctionResourcesReady = false;
			g_ownerThreadId = 0;
			if (hadResources) {
				g_resourceReleases.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void ReleaseProtectionEpoch() noexcept
		{
			// Protection itself is never disabled. Dropping these references only forgets
			// an obsolete context/device identity after device recreation.
			SafeRelease(g_multithread);
			SafeRelease(g_protectionContext);
			SafeRelease(g_protectionDevice);
			g_multithreadPriorProtected.store(false, std::memory_order_relaxed);
			g_multithreadEnabledByModule.store(false, std::memory_order_relaxed);
			g_multithreadProtected.store(false, std::memory_order_relaxed);
		}

		[[nodiscard]] bool EnsureMultithreadProtectionLocked(
			ID3D11DeviceContext* a_context,
			std::uint32_t& a_exceptionCode) noexcept
		{
			a_exceptionCode = 0;
			if (!a_context) {
				return false;
			}
			if (g_protectionContext == a_context && g_multithread && g_protectionDevice) {
				BOOL protectedNow = FALSE;
				__try {
					protectedNow = g_multithread->GetMultithreadProtected();
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					a_exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
				}
				g_multithreadProtected.store(protectedNow != FALSE, std::memory_order_relaxed);
				return a_exceptionCode == 0 && protectedNow != FALSE;
			}

			if (g_correctionResourcesReady) {
				return false;
			}
			ReleaseProtectionEpoch();
			ID3D11Device* device = nullptr;
			ID3D11Multithread* multithread = nullptr;
			bool valid = false;
			bool priorProtected = false;
			bool enabledByModule = false;
			g_protectionAttempts.fetch_add(1, std::memory_order_relaxed);
			__try {
				if (a_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) {
					a_context->GetDevice(&device);
					if (device &&
						(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) == 0 &&
						SUCCEEDED(a_context->QueryInterface(
							__uuidof(ID3D11Multithread),
							reinterpret_cast<void**>(&multithread))) && multithread) {
					priorProtected = multithread->GetMultithreadProtected() != FALSE;
					if (!priorProtected) {
						const BOOL previousAtSet =
							multithread->SetMultithreadProtected(TRUE);
						enabledByModule = previousAtSet == FALSE;
					}
						valid = multithread->GetMultithreadProtected() != FALSE;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
				valid = false;
			}
			if (!valid || a_exceptionCode != 0) {
				SafeRelease(multithread);
				SafeRelease(device);
				return false;
			}

			bool contextRetained = false;
			__try {
				a_context->AddRef();
				contextRetained = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (!contextRetained) {
				SafeRelease(multithread);
				SafeRelease(device);
				return false;
			}
			g_protectionContext = a_context;
			g_protectionDevice = device;
			device = nullptr;
			g_multithread = multithread;
			multithread = nullptr;
			g_multithreadPriorProtected.store(priorProtected, std::memory_order_relaxed);
			g_multithreadEnabledByModule.store(enabledByModule, std::memory_order_relaxed);
			g_multithreadProtected.store(true, std::memory_order_release);
			if (enabledByModule) {
				g_protectionEnables.fetch_add(1, std::memory_order_relaxed);
				logger::warn(
					"composite correction enabled D3D11 immediate-context multithread protection for the process; every immediate-context call now carries the runtime's serialization overhead"sv);
			}
			return true;
		}

		[[nodiscard]] bool EnterMultithreadLocked(
			PreparedDrawToken& a_token,
			std::uint32_t& a_exceptionCode) noexcept
		{
			a_exceptionCode = 0;
			if (!g_multithread || !g_multithreadProtected.load(std::memory_order_acquire)) {
				return false;
			}
			__try {
				g_multithread->AddRef();
				a_token.multithread = g_multithread;
				g_multithread->Enter();
				a_token.entered = true;
				g_protectionEnters.fetch_add(1, std::memory_order_relaxed);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (!a_token.entered) {
				SafeRelease(a_token.multithread);
			}
			return a_token.entered;
		}

		[[nodiscard]] bool AcquireMultithreadProtection(
			ID3D11DeviceContext* a_context,
			PreparedDrawToken& a_token,
			std::uint32_t& a_exceptionCode) noexcept
		{
			const std::lock_guard<std::mutex> epochGuard{ g_protectionEpochLock };
			return EnsureMultithreadProtectionLocked(a_context, a_exceptionCode) &&
				EnterMultithreadLocked(a_token, a_exceptionCode);
		}

		[[nodiscard]] bool EnterExistingMultithread(
			PreparedDrawToken& a_token,
			std::uint32_t& a_exceptionCode) noexcept
		{
			const std::lock_guard<std::mutex> epochGuard{ g_protectionEpochLock };
			return EnterMultithreadLocked(a_token, a_exceptionCode);
		}

		[[nodiscard]] bool EnterTechnique65600CorrectionEpoch(
			ID3D11DeviceContext* a_context,
			std::uint64_t a_generation,
			std::uint64_t a_correctionEpoch,
			PreparedDrawToken& a_token,
			Technique65600CorrectionDecline& a_decline,
			std::uint32_t& a_exceptionCode) noexcept
		{
			a_decline = Technique65600CorrectionDecline::kNone;
			a_exceptionCode = 0;
			const std::lock_guard<std::mutex> epochGuard{ g_protectionEpochLock };
			if (!g_technique65600CorrectionEnabled.load(std::memory_order_acquire) ||
				a_correctionEpoch != g_technique65600CorrectionEpoch.load(std::memory_order_acquire) ||
				a_generation != g_generation.load(std::memory_order_acquire) || !Enabled()) {
				a_decline = Technique65600CorrectionDecline::kStaleEpisode;
				return false;
			}
			// Exact 65600 may be the first eligible composite draw in an episode. Establish
			// protection here, then create correction resources only after its complete
			// shader and constant-buffer contract has been validated.
			if (!EnsureMultithreadProtectionLocked(a_context, a_exceptionCode)) {
				a_decline = Technique65600CorrectionDecline::kProtectionUnavailable;
				return false;
			}
			if (!a_context || g_protectionContext != a_context || !g_protectionDevice) {
				a_decline = Technique65600CorrectionDecline::kOwnerIdentityMismatch;
				return false;
			}
			if (!EnterMultithreadLocked(a_token, a_exceptionCode)) {
				a_decline = Technique65600CorrectionDecline::kProtectionUnavailable;
				return false;
			}
			a_token.technique65600CorrectionEpoch = a_correctionEpoch;
			return true;
		}

		[[nodiscard]] bool EnterTechniqueObservationMultithreadIsolated(
			ID3D11Multithread* a_multithread,
			TechniqueObservationThreadToken& a_token,
			std::uint32_t& a_exceptionCode) noexcept
		{
			a_exceptionCode = 0;
			__try {
				a_multithread->AddRef();
				a_token.multithread = a_multithread;
				a_multithread->Enter();
				a_token.entered = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (!a_token.entered) {
				SafeRelease(a_token.multithread);
			}
			return a_token.entered;
		}

		[[nodiscard]] bool EnterTechniqueObservationEpoch(
			ID3D11DeviceContext* a_context,
			TechniqueObservationThreadToken& a_token,
			TechniqueObservationFailure& a_failure,
			std::uint32_t& a_exceptionCode) noexcept
		{
			a_failure = TechniqueObservationFailure::kNone;
			a_exceptionCode = 0;
			const std::lock_guard<std::mutex> epochGuard{ g_protectionEpochLock };
			const auto threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			if (g_ownerThreadId == 0 || g_ownerThreadId != threadId) {
				a_failure = TechniqueObservationFailure::kWrongRenderThread;
				return false;
			}
			if (!g_ready.load(std::memory_order_acquire) ||
				!g_multithreadProtected.load(std::memory_order_acquire) || !g_multithread) {
				a_failure = TechniqueObservationFailure::kProtectionUnavailable;
				return false;
			}
			if (!a_context || g_protectionContext != a_context ||
				!g_protectionDevice || !g_correctionDevice ||
				g_protectionDevice != g_correctionDevice) {
				a_failure = TechniqueObservationFailure::kOwnerIdentityMismatch;
				return false;
			}
			a_token.ownerThreadId = g_ownerThreadId;
			a_token.contextIdentity = reinterpret_cast<std::uintptr_t>(g_protectionContext);
			a_token.deviceIdentity = reinterpret_cast<std::uintptr_t>(g_protectionDevice);
			if (!EnterTechniqueObservationMultithreadIsolated(
					g_multithread, a_token, a_exceptionCode)) {
				a_failure = TechniqueObservationFailure::kEnterFailed;
			}
			return a_token.entered;
		}

		[[nodiscard]] bool LeaveTechniqueObservationEpoch(
			TechniqueObservationThreadToken& a_token,
			std::uint32_t& a_exceptionCode) noexcept
		{
			a_exceptionCode = 0;
			if (!a_token.entered || !a_token.multithread) {
				return true;
			}
			bool left = false;
			__try {
				a_token.multithread->Leave();
				left = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			a_token.entered = false;
			SafeRelease(a_token.multithread);
			return left;
		}

		[[nodiscard]] bool ProtectionContextDiffers(ID3D11DeviceContext* a_context) noexcept
		{
			const std::lock_guard<std::mutex> epochGuard{ g_protectionEpochLock };
			return g_protectionContext && g_protectionContext != a_context;
		}

		[[nodiscard]] bool LeaveMultithread(
			PreparedDrawToken& a_token,
			std::uint32_t& a_exceptionCode) noexcept
		{
			a_exceptionCode = 0;
			if (!a_token.entered || !a_token.multithread) {
				return true;
			}
			bool left = false;
			__try {
				a_token.multithread->Leave();
				left = true;
				g_protectionLeaves.fetch_add(1, std::memory_order_relaxed);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				a_exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			a_token.entered = false;
			SafeRelease(a_token.multithread);
			return left;
		}

		[[nodiscard]] HRESULT CompileCorrectionShader(
			ID3D11Device* a_device,
			bool a_productBasis,
			ID3D11ComputeShader** a_shader) noexcept
		{
			*a_shader = nullptr;
			if (!g_compile) {
				g_compilerModule = ::LoadLibraryW(L"d3dcompiler_47.dll");
				if (g_compilerModule) {
					g_compile = reinterpret_cast<D3DCompileFn>(
						::GetProcAddress(g_compilerModule, "D3DCompile"));
				}
			}
			if (!g_compile) {
				return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
			}
			ID3DBlob* bytecode = nullptr;
			ID3DBlob* errors = nullptr;
			const D3D_SHADER_MACRO defines[] = {
				{ "PRODUCT_BASIS", a_productBasis ? "1" : "0" }, { nullptr, nullptr }
			};
			HRESULT hr = g_compile(
				kCorrectionShader, sizeof(kCorrectionShader) - 1,
				"Fo4VRUpscaler.CompositeCamera", defines, nullptr, "main", "cs_5_0",
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
				0, &bytecode, &errors);
			if (FAILED(hr) && errors && errors->GetBufferPointer()) {
				logger::error(
					FMT_STRING("composite correction shader compile failed: {}"),
					static_cast<const char*>(errors->GetBufferPointer()));
			}
			if (SUCCEEDED(hr) && bytecode) {
				hr = a_device->CreateComputeShader(
					bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, a_shader);
			}
			SafeRelease(errors);
			SafeRelease(bytecode);
			return hr;
		}

		[[nodiscard]] HRESULT CreateRawBufferAndUav(
			ID3D11Device* a_device,
			UINT a_bytes,
			ID3D11Buffer** a_buffer,
			ID3D11UnorderedAccessView** a_uav) noexcept
		{
			*a_buffer = nullptr;
			*a_uav = nullptr;
			D3D11_BUFFER_DESC bufferDesc{};
			bufferDesc.ByteWidth = a_bytes;
			bufferDesc.Usage = D3D11_USAGE_DEFAULT;
			bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			HRESULT hr = a_device->CreateBuffer(&bufferDesc, nullptr, a_buffer);
			if (FAILED(hr) || !*a_buffer) {
				return FAILED(hr) ? hr : E_FAIL;
			}
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = a_bytes / sizeof(std::uint32_t);
			uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			hr = a_device->CreateUnorderedAccessView(*a_buffer, &uavDesc, a_uav);
			if (FAILED(hr) || !*a_uav) {
				SafeRelease(*a_buffer);
				return FAILED(hr) ? hr : E_FAIL;
			}
			return S_OK;
		}

		[[nodiscard]] bool EnsureCorrectionResources(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			std::int32_t& a_hr) noexcept
		{
			a_hr = S_OK;
			if (g_correctionResourcesReady) {
				return g_correctionDevice == a_device && g_correctionContext == a_context &&
					g_correctionBufferBytes == kCameraBufferBytes;
			}
			D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
			a_hr = a_device->CheckFeatureSupport(
				D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
			if (FAILED(a_hr) || !options.ConstantBufferOffsetting) {
				return false;
			}
			if (!g_correctionIsolation.Initialize(a_device, a_context)) {
				a_hr = g_correctionIsolation.LastError();
				return false;
			}
			for (std::size_t i = 0; i < std::size(g_correctionShaders); ++i) {
				a_hr = CompileCorrectionShader(a_device, i != 0, &g_correctionShaders[i]);
				if (FAILED(a_hr) || !g_correctionShaders[i]) {
					ReleaseCorrectionResources();
					return false;
				}
			}

			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = sizeof(CorrectionParameters);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			a_hr = a_device->CreateBuffer(&desc, nullptr, &g_correctionParameters);
			if (SUCCEEDED(a_hr)) {
				a_hr = CreateRawBufferAndUav(
					a_device, kCameraBufferBytes, &g_correctedRaw, &g_correctedRawUav);
			}
			if (SUCCEEDED(a_hr)) {
				a_hr = CreateRawBufferAndUav(
					a_device, 16, &g_finiteMask, &g_finiteMaskUav);
			}
			if (SUCCEEDED(a_hr)) {
				desc = {};
				desc.ByteWidth = kCameraBufferBytes;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				a_hr = a_device->CreateBuffer(&desc, nullptr, &g_privateCamera);
			}
			if (FAILED(a_hr) || !g_correctionParameters || !g_correctedRawUav ||
				!g_finiteMaskUav || !g_privateCamera) {
				ReleaseCorrectionResources();
				return false;
			}

			a_device->AddRef();
			g_correctionDevice = a_device;
			a_context->AddRef();
			g_correctionContext = a_context;
			if (FAILED(a_context->QueryInterface(
					__uuidof(ID3D11DeviceContext1),
					reinterpret_cast<void**>(&g_correctionContext1))) || !g_correctionContext1) {
				a_hr = E_NOINTERFACE;
				ReleaseCorrectionResources();
				return false;
			}
			g_correctionBufferBytes = kCameraBufferBytes;
			g_ownerThreadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			g_correctionResourcesReady = true;
			g_resourceBuilds.fetch_add(1, std::memory_order_relaxed);
			g_resourceEpoch.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		[[nodiscard]] PrivatePreparationResult PreparePrivateCamera(
			ID3D11DeviceContext* a_context,
			ID3D11DeviceContext1* a_context1,
			ID3D11Buffer* a_source,
			ID3D11Buffer* a_cb2,
			ID3D11Buffer* a_cb8,
			float a_ratioWidth,
			float a_ratioHeight,
			const D3D11_VIEWPORT& a_viewport,
			const UINT a_firstConstants[3],
			const UINT a_numConstants[3],
			UINT a_dimensionRow = 0,
			bool a_deriveViewportScale = false,
			UINT a_nativeWidth = 0,
			UINT a_nativeHeight = 0) noexcept
		{
			PrivatePreparationResult result{};
			bool mapped = false;
			__try {
				result.began = g_correctionIsolation.Begin();
				if (result.began) {
					__try {
						D3D11_MAPPED_SUBRESOURCE mapping{};
						result.hr = a_context->Map(
							g_correctionParameters, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapping);
						if (SUCCEEDED(result.hr) && mapping.pData) {
							mapped = true;
							const CorrectionParameters parameters{
								1.0f / a_ratioWidth,
								1.0f / a_ratioHeight,
								kCameraBufferRows,
								a_firstConstants[2] + static_cast<UINT>(kFirstReconstructionRow),
								a_ratioWidth,
								a_ratioHeight,
								a_viewport.Width,
								a_viewport.Height,
								a_dimensionRow,
								a_deriveViewportScale ? 1u : 0u,
								a_nativeWidth,
								a_nativeHeight
							};
							std::memcpy(mapping.pData, &parameters, sizeof(parameters));
							a_context->Unmap(g_correctionParameters, 0);
							mapped = false;

							constexpr UINT clear[4]{};
							a_context->ClearUnorderedAccessViewUint(g_finiteMaskUav, clear);
							ID3D11Buffer* sourceConstant[1] = { a_source };
							a_context1->CSSetConstantBuffers1(
								0, 1, sourceConstant,
								&a_firstConstants[2], &a_numConstants[2]);
							ID3D11Buffer* parameterConstant[1] = { g_correctionParameters };
							a_context->CSSetConstantBuffers(1, 1, parameterConstant);
							ID3D11Buffer* perDrawConstant[1] = { a_cb2 };
							a_context1->CSSetConstantBuffers1(
								2, 1, perDrawConstant,
								&a_firstConstants[0], &a_numConstants[0]);
							ID3D11Buffer* stereoConstant[1] = { a_cb8 };
							a_context1->CSSetConstantBuffers1(
								3, 1, stereoConstant,
								&a_firstConstants[1], &a_numConstants[1]);
							a_context->CSSetShader(g_correctionShaders[a_dimensionRow == 5 ? 1 : 0], nullptr, 0);
							ID3D11UnorderedAccessView* outputs[2] = {
								g_correctedRawUav, g_finiteMaskUav
							};
							a_context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
							a_context->Dispatch((kCameraBufferRows + 63) / 64, 1, 1);

							ID3D11UnorderedAccessView* nullOutputs[2]{};
							a_context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
							ID3D11Buffer* nullConstants[4]{};
							a_context->CSSetConstantBuffers(0, 4, nullConstants);
							a_context->CSSetShader(nullptr, nullptr, 0);
							a_context->CopyResource(g_privateCamera, g_correctedRaw);
							result.dispatched = true;
						}
					} __finally {
						if (mapped) {
							a_context->Unmap(g_correctionParameters, 0);
							mapped = false;
						}
						result.ended = g_correctionIsolation.End();
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			if (g_correctionIsolation.StateUncertain()) {
				static_cast<void>(g_correctionIsolation.RecoverUncertainState());
			}
			result.stateUncertain = D3D11StateIsolation::HostStateUncertain();
			return result;
		}

		struct BindingResult
		{
			std::uint32_t exceptionCode = 0;
			bool verified = false;
		};

		[[nodiscard]] BindingResult SetAndVerifyCameraBinding(
			ID3D11DeviceContext1* a_context,
			ID3D11Buffer* a_buffer,
			UINT a_firstConstant,
			UINT a_numConstants) noexcept
		{
			BindingResult result{};
			ID3D11Buffer* observed = nullptr;
			UINT observedFirst = 0;
			UINT observedCount = 0;
			__try {
				a_context->PSSetConstantBuffers1(
					kCameraSlot, 1, &a_buffer, &a_firstConstant, &a_numConstants);
				a_context->PSGetConstantBuffers1(
					kCameraSlot, 1, &observed, &observedFirst, &observedCount);
				result.verified = observed == a_buffer &&
					observedFirst == a_firstConstant && observedCount == a_numConstants;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				result.exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
			}
			SafeRelease(observed);
			return result;
		}

		[[nodiscard]] bool ReadCorrectionEvidence(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			ID3D11Buffer* a_source,
			ID3D11Buffer* a_cb2,
			ID3D11Buffer* a_cb8,
			const UINT a_firstConstants[3],
			const UINT a_numConstants[3],
			CorrectionCapture& a_capture) noexcept
		{
			a_capture.source.rangeKnown = true;
			a_capture.source.firstConstant = a_firstConstants[2];
			a_capture.source.numConstants = a_numConstants[2];
			a_capture.privateCopy.rangeKnown = true;
			a_capture.privateCopy.firstConstant = a_firstConstants[2];
			a_capture.privateCopy.numConstants = a_numConstants[2];
			a_capture.cb2.rangeKnown = true;
			a_capture.cb2.firstConstant = a_firstConstants[0];
			a_capture.cb2.numConstants = a_numConstants[0];
			a_capture.cb8.rangeKnown = true;
			a_capture.cb8.firstConstant = a_firstConstants[1];
			a_capture.cb8.numConstants = a_numConstants[1];
			const auto source = ReadBufferIsolated(
				a_device, a_context, a_source, a_capture.source);
			const auto corrected = ReadBufferIsolated(
				a_device, a_context, g_privateCamera, a_capture.privateCopy);
			const auto perDraw = ReadBufferIsolated(
				a_device, a_context, a_cb2, a_capture.cb2);
			const auto stereo = ReadBufferIsolated(
				a_device, a_context, a_cb8, a_capture.cb8);
			BufferCapture mask{};
			const auto maskRead = ReadBufferIsolated(
				a_device, a_context, g_finiteMask, mask);
			if (!source.copied || !corrected.copied || !perDraw.copied ||
				!stereo.copied || !maskRead.copied ||
				source.exceptionCode || corrected.exceptionCode || maskRead.exceptionCode ||
				perDraw.exceptionCode || stereo.exceptionCode ||
				mask.dataBytes < sizeof(std::uint32_t)) {
				return false;
			}
			std::uint32_t rawMask = 0;
			std::memcpy(&rawMask, mask.data, sizeof(rawMask));
			if ((rawMask & ~0x1Fu) != 0) {
				return false;
			}
			a_capture.stereoBasisValidated = (rawMask & 0x10u) != 0;
			a_capture.correctedMask = rawMask & 0xFu;
			a_capture.skippedMask = (~a_capture.correctedMask) & 0xFu;
			a_capture.maskKnown = true;
			a_capture.completeReconstruction =
				a_capture.stereoBasisValidated && a_capture.correctedMask == 0xFu;
			if (EffectiveRangeCovers(a_capture.cb2, 0, 1) &&
				EffectiveRangeCovers(a_capture.cb8, 0, 1)) {
				float sizeValues[4]{};
				float stereoValues[4]{};
				std::memcpy(
					sizeValues,
					a_capture.cb2.data + EffectiveByteOffset(a_capture.cb2, 0),
					sizeof(sizeValues));
				std::memcpy(
					stereoValues,
					a_capture.cb8.data + EffectiveByteOffset(a_capture.cb8, 0),
					sizeof(stereoValues));
				a_capture.nativeWidth = sizeValues[2];
				a_capture.nativeHeight = sizeValues[3];
				a_capture.stereoValue = stereoValues[0];
			}
			return true;
		}

		[[nodiscard]] bool BufferDescriptionMatchesCamera(ID3D11Buffer* a_buffer) noexcept
		{
			D3D11_BUFFER_DESC desc{};
			bool read = false;
			__try {
				a_buffer->GetDesc(&desc);
				read = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				read = false;
			}
			return read && desc.ByteWidth == kCameraBufferBytes &&
				desc.Usage == D3D11_USAGE_DYNAMIC &&
				desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER &&
				desc.CPUAccessFlags == D3D11_CPU_ACCESS_WRITE && desc.MiscFlags == 0;
		}

		[[nodiscard]] bool BufferRangeCoversEffectiveRow0(
			ID3D11Buffer* a_buffer,
			UINT a_firstConstant,
			UINT a_numConstants) noexcept
		{
			D3D11_BUFFER_DESC desc{};
			bool read = false;
			__try {
				a_buffer->GetDesc(&desc);
				read = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				read = false;
			}
			return read && a_numConstants >= 1 &&
				(a_firstConstant % 16) == 0 && (a_numConstants % 16) == 0 &&
				a_numConstants <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT &&
				(static_cast<std::uint64_t>(a_firstConstant) + 1) * kBytesPerConstant <=
					desc.ByteWidth;
		}

		[[nodiscard]] bool BufferRangeCoversShaderRows(
			ID3D11Buffer* a_buffer,
			UINT a_firstConstant,
			UINT a_numConstants,
			std::size_t a_shaderRows) noexcept
		{
			D3D11_BUFFER_DESC desc{};
			bool read = false;
			__try {
				a_buffer->GetDesc(&desc);
				read = true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				read = false;
			}
			return read && a_numConstants >= a_shaderRows &&
				(a_firstConstant % 16) == 0 && (a_numConstants % 16) == 0 &&
				a_numConstants <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT &&
				(static_cast<std::uint64_t>(a_firstConstant) + a_shaderRows) *
					kBytesPerConstant <= desc.ByteWidth;
		}

		[[nodiscard]] bool Technique65600CorrectionCurrent(
			std::uint64_t a_epoch,
			std::uint64_t a_generation) noexcept
		{
			return g_technique65600CorrectionEnabled.load(std::memory_order_acquire) &&
				a_epoch == g_technique65600CorrectionEpoch.load(std::memory_order_acquire) &&
				a_generation == g_generation.load(std::memory_order_acquire) && Enabled();
		}

		[[nodiscard]] bool CountTechnique65600CorrectionEvent(
			std::atomic<std::uint64_t>& a_counter,
			std::uint64_t a_epoch,
			std::uint64_t a_generation) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!Technique65600CorrectionCurrent(a_epoch, a_generation)) {
				return false;
			}
			a_counter.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		void RecordTechnique65600CorrectionDecline(
			std::uint64_t a_epoch,
			std::uint64_t a_generation,
			Technique65600CorrectionDecline a_decline,
			std::int32_t a_hr = S_OK,
			std::uint32_t a_exceptionCode = 0) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!Technique65600CorrectionCurrent(a_epoch, a_generation)) {
				return;
			}
			g_technique65600CorrectionDeclines.fetch_add(1, std::memory_order_relaxed);
			g_technique65600CorrectionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			g_technique65600CorrectionLastDecline.store(
				static_cast<std::uint32_t>(a_decline), std::memory_order_relaxed);
			g_technique65600CorrectionLastHr.store(a_hr, std::memory_order_relaxed);
			g_technique65600CorrectionExceptionCode.store(
				a_exceptionCode, std::memory_order_relaxed);
		}

		void RecordTechnique65600TerminalFailure(
			std::uint64_t a_epoch,
			std::uint64_t a_generation,
			Technique65600CorrectionDecline a_decline,
			std::int32_t a_hr,
			std::uint32_t a_exceptionCode) noexcept
		{
			g_exceptionCode.store(a_exceptionCode, std::memory_order_relaxed);
			g_terminalStateUncertain.store(true, std::memory_order_release);
			g_ready.store(false, std::memory_order_relaxed);
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!Technique65600CorrectionCurrent(a_epoch, a_generation)) {
				return;
			}
			g_technique65600CorrectionDeclines.fetch_add(1, std::memory_order_relaxed);
			g_technique65600CorrectionLastDecline.store(
				static_cast<std::uint32_t>(a_decline), std::memory_order_relaxed);
			g_technique65600CorrectionLastHr.store(a_hr, std::memory_order_relaxed);
			g_technique65600CorrectionExceptionCode.store(
				a_exceptionCode, std::memory_order_relaxed);
		}

		[[nodiscard]] bool ReserveTechnique65600CorrectionCapture(
			std::uint64_t a_epoch,
			std::uint64_t a_generation) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!g_diagnosticsEnabled.load(std::memory_order_acquire) ||
				!Technique65600CorrectionCurrent(a_epoch, a_generation) ||
				g_technique65600CorrectionCaptureReservations >=
					kMaxTechnique65600CorrectionSamples ||
				g_technique65600CorrectionCaptureCount >=
					kMaxTechnique65600CorrectionSamples) {
				return false;
			}
			++g_technique65600CorrectionCaptureReservations;
			g_technique65600CorrectionCaptureAttempts.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		[[nodiscard]] Technique65600DimensionResult Technique65600NativeDimensions(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			ID3D11Buffer* a_cb2,
			UINT a_firstConstant,
			UINT a_numConstants,
			std::uint64_t a_epoch,
			std::uint64_t a_generation,
			std::uint64_t a_resourceEpoch) noexcept
		{
			Technique65600DimensionResult result{};
			{
				const std::lock_guard<std::mutex> guard{ g_lock };
				if (!Technique65600CorrectionCurrent(a_epoch, a_generation)) {
					result.decline = Technique65600CorrectionDecline::kStaleEpisode;
					return result;
				}
				const bool cacheIdentityMatches =
					g_technique65600DimensionCache.correctionEpoch == a_epoch &&
					g_technique65600DimensionCache.generation == a_generation &&
					g_technique65600DimensionCache.resourceEpoch == a_resourceEpoch &&
					g_technique65600DimensionCache.context ==
						reinterpret_cast<std::uintptr_t>(a_context) &&
					g_technique65600DimensionCache.device ==
						reinterpret_cast<std::uintptr_t>(a_device);
				if (cacheIdentityMatches && g_technique65600DimensionCache.valid) {
					result.width = g_technique65600DimensionCache.width;
					result.height = g_technique65600DimensionCache.height;
					result.resourceEpoch = a_resourceEpoch;
					result.cacheIdentityMatched = true;
					result.valid = true;
					return result;
				}
				if (g_technique65600DimensionCache.attempted) {
					result.decline = cacheIdentityMatches ?
						Technique65600CorrectionDecline::kDimensionReadFailed :
						Technique65600CorrectionDecline::kDimensionCacheStale;
					return result;
				}
				g_technique65600DimensionCache.attempted = true;
				g_technique65600DimensionCache.correctionEpoch = a_epoch;
				g_technique65600DimensionCache.generation = a_generation;
				g_technique65600DimensionCache.resourceEpoch = a_resourceEpoch;
				g_technique65600DimensionCache.context =
					reinterpret_cast<std::uintptr_t>(a_context);
				g_technique65600DimensionCache.device =
					reinterpret_cast<std::uintptr_t>(a_device);
				g_technique65600DimensionReadAttempts.fetch_add(1, std::memory_order_relaxed);
			}

			BufferCapture cb2{};
			cb2.rangeKnown = true;
			cb2.firstConstant = a_firstConstant;
			cb2.numConstants = a_numConstants;
			const auto readback = ReadBufferIsolated(a_device, a_context, a_cb2, cb2);
			result.hr = readback.hr;
			result.exceptionCode = readback.exceptionCode;
			result.decline = Technique65600CorrectionDecline::kDimensionReadFailed;
			if (readback.copied && readback.exceptionCode == 0 &&
				EffectiveRangeCovers(cb2, 0, 1)) {
				float values[4]{};
				std::memcpy(
					values, cb2.data + EffectiveByteOffset(cb2, 0), sizeof(values));
				const bool integralDimensions =
					std::isfinite(values[2]) && std::isfinite(values[3]) &&
					values[2] > 0.0f && values[3] > 0.0f &&
					std::trunc(values[2]) == values[2] &&
					std::trunc(values[3]) == values[3] &&
					static_cast<double>(values[2]) <=
						static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) &&
					static_cast<double>(values[3]) <=
						static_cast<double>((std::numeric_limits<std::uint32_t>::max)());
				const bool reciprocalDimensions =
					std::isfinite(values[0]) && std::isfinite(values[1]) &&
					values[0] > 0.0f && values[1] > 0.0f && integralDimensions &&
					std::fabs(values[0] * values[2] - 1.0f) <= 0.0001f &&
					std::fabs(values[1] * values[3] - 1.0f) <= 0.0001f &&
					(static_cast<std::uint32_t>(values[2]) & 1u) == 0;
				if (reciprocalDimensions) {
					result.width = values[2];
					result.height = values[3];
					result.valid = true;
					result.decline = Technique65600CorrectionDecline::kNone;
				}
			}

			const std::lock_guard<std::mutex> guard{ g_lock };
			const bool publish = Technique65600CorrectionCurrent(a_epoch, a_generation) &&
				g_resourceEpoch.load(std::memory_order_acquire) == a_resourceEpoch &&
				g_technique65600DimensionCache.correctionEpoch == a_epoch &&
				g_technique65600DimensionCache.generation == a_generation &&
				g_technique65600DimensionCache.resourceEpoch == a_resourceEpoch &&
				g_technique65600DimensionCache.context ==
					reinterpret_cast<std::uintptr_t>(a_context) &&
				g_technique65600DimensionCache.device ==
					reinterpret_cast<std::uintptr_t>(a_device);
			if (!publish) {
				result.valid = false;
				result.decline = Technique65600CorrectionDecline::kStaleEpisode;
				return result;
			}
			if (result.valid) {
				g_technique65600DimensionCache.width = result.width;
				g_technique65600DimensionCache.height = result.height;
				g_technique65600DimensionCache.valid = true;
				result.resourceEpoch = a_resourceEpoch;
				result.cacheIdentityMatched = true;
			} else {
				g_technique65600DimensionReadFailures.fetch_add(1, std::memory_order_relaxed);
			}
			return result;
		}

		[[nodiscard]] bool Technique65600ViewportContract(
			const BoundState& a_bound,
			float a_nativeWidth,
			float a_nativeHeight,
			float a_globalRatioWidth,
			float a_globalRatioHeight,
			float& a_derivedRatioWidth,
			float& a_derivedRatioHeight) noexcept
		{
			if (a_bound.viewportCount != 1 ||
				!std::isfinite(a_globalRatioWidth) || !std::isfinite(a_globalRatioHeight) ||
				a_globalRatioWidth <= 0.0f || a_globalRatioHeight <= 0.0f ||
				a_globalRatioWidth > 1.0f || a_globalRatioHeight > 1.0f) {
				return false;
			}
			const auto& viewport = a_bound.viewports[0];
			const bool viewportShape =
				std::isfinite(viewport.TopLeftX) && viewport.TopLeftX == 0.0f &&
				std::isfinite(viewport.TopLeftY) && viewport.TopLeftY == 0.0f &&
				std::isfinite(viewport.Width) && viewport.Width > 0.0f &&
				std::isfinite(viewport.Height) && viewport.Height > 0.0f &&
				std::trunc(viewport.Width) == viewport.Width &&
				std::trunc(viewport.Height) == viewport.Height &&
				static_cast<double>(viewport.Width) <=
					static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) &&
				static_cast<double>(viewport.Height) <=
					static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) &&
				(static_cast<std::uint32_t>(viewport.Width) & 1u) == 0 &&
				std::isfinite(viewport.MinDepth) &&
				viewport.MinDepth == kTechnique65600MinDepth &&
				std::isfinite(viewport.MaxDepth) && viewport.MaxDepth == 1.0f;
			if (!viewportShape) {
				return false;
			}

			const double expectedWidth = 2.0 * std::floor(
				(static_cast<double>(a_nativeWidth) * 0.5) *
				static_cast<double>(a_globalRatioWidth));
			const double expectedHeight = std::floor(
				static_cast<double>(a_nativeHeight) *
				static_cast<double>(a_globalRatioHeight));
			if (!std::isfinite(expectedWidth) || !std::isfinite(expectedHeight) ||
				expectedWidth <= 0.0 || expectedHeight <= 0.0 ||
				expectedWidth > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) ||
				expectedHeight > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) ||
				viewport.Width != static_cast<float>(expectedWidth) ||
				viewport.Height != static_cast<float>(expectedHeight) ||
				viewport.Width >= a_nativeWidth || viewport.Height >= a_nativeHeight) {
				return false;
			}

			a_derivedRatioWidth = viewport.Width / a_nativeWidth;
			a_derivedRatioHeight = viewport.Height / a_nativeHeight;
			return std::isfinite(a_derivedRatioWidth) &&
				std::isfinite(a_derivedRatioHeight) &&
				a_derivedRatioWidth > 0.0f && a_derivedRatioWidth < 1.0f &&
				a_derivedRatioHeight > 0.0f && a_derivedRatioHeight < 1.0f;
		}

		[[nodiscard]] bool ViewportBasisMatches(
			const BoundState& a_bound,
			float a_ratioWidth,
			float a_ratioHeight) noexcept
		{
			if (a_bound.viewportCount != 1) {
				return false;
			}
			const auto& viewport = a_bound.viewports[0];
			return std::isfinite(viewport.TopLeftX) && viewport.TopLeftX == 0.0f &&
				std::isfinite(viewport.TopLeftY) && viewport.TopLeftY == 0.0f &&
				std::isfinite(viewport.Width) && viewport.Width > 0.0f &&
				std::isfinite(viewport.Height) && viewport.Height > 0.0f &&
				std::isfinite(viewport.MinDepth) &&
				(viewport.MinDepth == 0.0f || viewport.MinDepth == kTechnique65600MinDepth) &&
				std::isfinite(viewport.MaxDepth) && viewport.MaxDepth == 1.0f &&
				std::isfinite(a_ratioWidth) && std::isfinite(a_ratioHeight);
		}

		[[nodiscard]] RegisteredTechnique FindRegisteredTechnique(
			ID3D11PixelShader* a_pixelShader) noexcept
		{
			RegisteredTechnique found{};
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!g_tableSnapshotComplete) {
				return found;
			}
			for (std::size_t i = 0; i < g_registeredTechniqueCount; ++i) {
				if (g_registeredTechniques[i].pixelShader == a_pixelShader) {
					found = g_registeredTechniques[i];
					break;
				}
			}
			return found;
		}

		void RecordTechniqueObservationFailure(
			std::uint64_t a_epoch,
			TechniqueObservationFailure a_failure,
			bool a_gpuAttempt,
			std::int32_t a_hr = S_OK,
			std::uint32_t a_exceptionCode = 0) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!g_diagnosticsEnabled.load(std::memory_order_acquire) ||
				a_epoch != g_techniqueObservationEpoch.load(std::memory_order_acquire)) {
				return;
			}
			if (a_gpuAttempt) {
				g_techniqueObservationFailures.fetch_add(1, std::memory_order_relaxed);
			} else {
				g_techniqueObservationEligibilityDeclines.fetch_add(1, std::memory_order_relaxed);
			}
			g_techniqueObservationLastFailure.store(
				static_cast<std::uint32_t>(a_failure), std::memory_order_relaxed);
			g_techniqueObservationLastHr.store(a_hr, std::memory_order_relaxed);
			g_techniqueObservationExceptionCode.store(a_exceptionCode, std::memory_order_relaxed);
		}

		[[nodiscard]] bool BeginTechniqueObservationEligibility(
			std::uint64_t a_diagnosticEpoch,
			std::uint64_t a_correctionGeneration) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!g_diagnosticsEnabled.load(std::memory_order_acquire) ||
				a_diagnosticEpoch != g_techniqueObservationEpoch.load(std::memory_order_acquire) ||
				a_correctionGeneration != g_generation.load(std::memory_order_acquire) || !Enabled()) {
				return false;
			}
			g_techniqueObservationEligibleDraws.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		[[nodiscard]] bool ReserveTechniqueObservationAttempt(
			std::uint64_t a_diagnosticEpoch,
			std::uint64_t a_correctionGeneration) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!g_diagnosticsEnabled.load(std::memory_order_acquire) ||
				a_diagnosticEpoch != g_techniqueObservationEpoch.load(std::memory_order_acquire) ||
				a_correctionGeneration != g_generation.load(std::memory_order_acquire) || !Enabled()) {
				return false;
			}
			const auto attempts =
				g_techniqueObservationAttempts.load(std::memory_order_relaxed);
			if (attempts >= kMaxTechniqueObservationSamples ||
				g_techniqueObservationCaptureCount >= kMaxTechniqueObservationSamples) {
				return false;
			}
			g_techniqueObservationAttempts.store(attempts + 1, std::memory_order_release);
			return true;
		}

		[[nodiscard]] bool ReadTechniqueObservationEvidence(
			ID3D11Device* a_device,
			ID3D11DeviceContext* a_context,
			const TechniqueObservationBoundState& a_bound,
			TechniqueObservationCapture& a_capture,
			std::int32_t& a_hr,
			std::uint32_t& a_exceptionCode) noexcept
		{
			a_capture.cb2.rangeKnown = a_bound.rangeKnown;
			a_capture.cb2.firstConstant = a_bound.firstConstant[0];
			a_capture.cb2.numConstants = a_bound.numConstants[0];
			a_capture.cb8.rangeKnown = a_bound.rangeKnown;
			a_capture.cb8.firstConstant = a_bound.firstConstant[1];
			a_capture.cb8.numConstants = a_bound.numConstants[1];
			a_capture.cb12.rangeKnown = a_bound.rangeKnown;
			a_capture.cb12.firstConstant = a_bound.firstConstant[2];
			a_capture.cb12.numConstants = a_bound.numConstants[2];

			const auto perDraw = ReadBufferIsolated(a_device, a_context, a_bound.cb2, a_capture.cb2);
			const auto stereo = ReadBufferIsolated(a_device, a_context, a_bound.cb8, a_capture.cb8);
			const auto camera = ReadBufferIsolated(a_device, a_context, a_bound.cb12, a_capture.cb12);
			const ReadbackResult results[3] = { perDraw, stereo, camera };
			for (const auto& result : results) {
				if (!result.copied || FAILED(result.hr) || result.exceptionCode != 0) {
					a_hr = result.hr;
					a_exceptionCode = result.exceptionCode;
					return false;
				}
			}

			a_capture.cameraRangeCoversShaderRows =
				EffectiveRangeCovers(a_capture.cb12, 0, kObservedCameraRows);
			if (!EffectiveRangeCovers(a_capture.cb2, 0, 3) ||
				!EffectiveRangeCovers(a_capture.cb8, 0, 1) ||
				!a_capture.cameraRangeCoversShaderRows) {
				a_hr = E_INVALIDARG;
				return false;
			}

			float sizeValues[4]{};
			float stereoValues[4]{};
			std::uint32_t stereoBits[4]{};
			std::memcpy(
				sizeValues,
				a_capture.cb2.data + EffectiveByteOffset(a_capture.cb2, 0),
				sizeof(sizeValues));
			std::memcpy(
				stereoValues,
				a_capture.cb8.data + EffectiveByteOffset(a_capture.cb8, 0),
				sizeof(stereoValues));
			std::memcpy(stereoBits, stereoValues, sizeof(stereoBits));
			a_capture.nativeWidth = sizeValues[2];
			a_capture.nativeHeight = sizeValues[3];
			a_capture.stereoValue = stereoValues[0];
			bool finitePositive = true;
			for (const float value : sizeValues) {
				finitePositive = finitePositive && std::isfinite(value) && value > 0.0f;
			}
			const bool viewportKnown = a_capture.viewportCount == 1 &&
				std::isfinite(a_capture.viewports[0].TopLeftX) &&
				a_capture.viewports[0].TopLeftX == 0.0f &&
				std::isfinite(a_capture.viewports[0].TopLeftY) &&
				a_capture.viewports[0].TopLeftY == 0.0f &&
				std::isfinite(a_capture.viewports[0].Width) &&
				a_capture.viewports[0].Width > 0.0f &&
				std::isfinite(a_capture.viewports[0].Height) &&
				a_capture.viewports[0].Height > 0.0f &&
				std::isfinite(a_capture.viewports[0].MinDepth) &&
				a_capture.viewports[0].MinDepth == 0.0f &&
				std::isfinite(a_capture.viewports[0].MaxDepth) &&
				a_capture.viewports[0].MaxDepth == 1.0f;
			a_capture.stereoBasisValidated = a_capture.ratiosRead &&
				a_capture.ratiosValid && viewportKnown && finitePositive &&
				stereoBits[0] == 0x3F800000u &&
				std::fabs(sizeValues[0] * sizeValues[2] - 1.0f) <= 0.0001f &&
				std::fabs(sizeValues[1] * sizeValues[3] - 1.0f) <= 0.0001f &&
				std::fabs(sizeValues[2] * a_capture.ratioWidth - a_capture.viewports[0].Width) <= 0.05f &&
				std::fabs(sizeValues[3] * a_capture.ratioHeight - a_capture.viewports[0].Height) <= 0.05f;
			return true;
		}

		[[nodiscard]] bool LatchTechniqueObservationIdentity(
			std::uint64_t a_diagnosticEpoch,
			std::uint64_t a_correctionGeneration,
			std::uint32_t a_threadId,
			std::uintptr_t a_context,
			ID3D11Device* a_device,
			bool& a_registryDeviceMatches,
			bool& a_episodeIdentityMatches) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!g_diagnosticsEnabled.load(std::memory_order_acquire) ||
				a_diagnosticEpoch != g_techniqueObservationEpoch.load(std::memory_order_acquire) ||
				a_correctionGeneration != g_generation.load(std::memory_order_acquire) || !Enabled()) {
				return false;
			}
			a_registryDeviceMatches = a_device && a_device == g_registryDevice;
			const auto device = reinterpret_cast<std::uintptr_t>(a_device);
			if (g_techniqueObservationThreadId == 0 && a_registryDeviceMatches) {
				g_techniqueObservationThreadId = a_threadId;
				g_techniqueObservationContext = a_context;
				g_techniqueObservationDevice = device;
			}
			a_episodeIdentityMatches =
				g_techniqueObservationThreadId == a_threadId &&
				g_techniqueObservationContext == a_context &&
				g_techniqueObservationDevice == device;
			return true;
		}

		[[nodiscard]] bool PublishTechniqueObservation(
			std::uint64_t a_diagnosticEpoch,
			std::uint64_t a_correctionGeneration,
			TechniqueObservationCapture& a_capture) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!g_diagnosticsEnabled.load(std::memory_order_acquire) ||
				a_diagnosticEpoch != g_techniqueObservationEpoch.load(std::memory_order_acquire) ||
				a_correctionGeneration != g_generation.load(std::memory_order_acquire) ||
				!Enabled() ||
				g_techniqueObservationCaptureCount >= kMaxTechniqueObservationSamples) {
				return false;
			}
			a_capture.serial = g_techniqueObservationCaptureCount + 1;
			g_techniqueObservationCaptures[g_techniqueObservationCaptureCount++] = a_capture;
			g_techniqueObservationSuccesses.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		[[nodiscard]] bool ObserveTechnique65600(
			ID3D11DeviceContext* a_context,
			const PixelShaderQuery& a_preliminary,
			const RegisteredTechnique& a_preliminaryRecord,
			std::uintptr_t a_callerReturn,
			std::uintptr_t a_callerReturnRva,
			std::uintptr_t a_packet,
			std::uint32_t a_packetDrawCount,
			bool a_ratiosRead,
			float a_ratioWidth,
			float a_ratioHeight,
			std::uint64_t a_correctionGeneration,
			std::uint64_t a_diagnosticEpoch) noexcept
		{
			if (!BeginTechniqueObservationEligibility(
					a_diagnosticEpoch, a_correctionGeneration)) {
				return true;
			}
			if (g_techniqueObservationSuccesses.load(std::memory_order_acquire) >=
				kMaxTechniqueObservationSamples ||
				g_techniqueObservationAttempts.load(std::memory_order_acquire) >=
					kMaxTechniqueObservationSamples) {
				return true;
			}

			TechniqueObservationThreadToken threadToken{};
			TechniqueObservationFailure failure = TechniqueObservationFailure::kNone;
			std::uint32_t exceptionCode = 0;
			if (!EnterTechniqueObservationEpoch(
					a_context, threadToken, failure, exceptionCode)) {
				RecordTechniqueObservationFailure(
					a_diagnosticEpoch, failure, false, S_OK, exceptionCode);
				return true;
			}

			TechniqueObservationCapture capture{};
			TechniqueObservationBoundState bound{};
			RegisteredTechnique selected{};
			capture.correctionGeneration = a_correctionGeneration;
			capture.diagnosticEpoch = a_diagnosticEpoch;
			capture.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			capture.expectedRenderThreadId = threadToken.ownerThreadId;
			capture.callerReturn = a_callerReturn;
			capture.callerReturnRva = a_callerReturnRva;
			capture.packet = a_packet;
			capture.packetDrawCount = a_packetDrawCount;
			capture.pixelTechnique = a_preliminaryRecord.pixelTechnique;
			capture.pixelShaderRecord = a_preliminaryRecord.pixelShaderRecord;
			capture.expectedPixelShader =
				reinterpret_cast<std::uintptr_t>(a_preliminaryRecord.pixelShader);
			capture.context = reinterpret_cast<std::uintptr_t>(a_context);
			capture.ratiosRead = a_ratiosRead;
			capture.ratiosValid = a_ratiosRead &&
				std::isfinite(a_ratioWidth) && std::isfinite(a_ratioHeight) &&
				a_ratioWidth > 0.0f && a_ratioHeight > 0.0f &&
				a_ratioWidth <= 1.0f && a_ratioHeight <= 1.0f;
			capture.ratioWidth = a_ratioWidth;
			capture.ratioHeight = a_ratioHeight;

			bool registryDeviceMatches = false;
			bool episodeIdentityMatches = false;
			bool attempted = false;
			bool evidenceRead = false;
			std::int32_t readHr = S_OK;
			std::uint32_t readException = 0;
			std::uint32_t boundException = 0;
			bool left = false;
			__try {
				__try {
					bound = QueryTechniqueObservationStateIsolated(a_context);
					boundException = bound.exceptionCode;
					selected = FindRegisteredTechnique(bound.pixelShader);
					capture.boundPixelShader = reinterpret_cast<std::uintptr_t>(bound.pixelShader);
					capture.device = reinterpret_cast<std::uintptr_t>(bound.device);
					capture.contextType = bound.contextType;
					capture.multithreadInterface = bound.multithreadInterface;
					capture.multithreadProtected = bound.multithreadProtected;
					capture.viewportCount = bound.viewportCount;
					std::memcpy(capture.viewports, bound.viewports, sizeof(capture.viewports));
					std::memcpy(capture.resources, bound.resources, sizeof(capture.resources));
					capture.engineCameraBuffer = EngineCameraBuffer();
					capture.cb12MatchesEngine = bound.cb12 &&
						reinterpret_cast<std::uintptr_t>(bound.cb12) == capture.engineCameraBuffer;
					capture.pixelShaderMatchesRecord = bound.pixelShader &&
						bound.pixelShader == a_preliminary.pixelShader &&
						selected.pixelShader == bound.pixelShader &&
						selected.pixelShaderRecord == a_preliminaryRecord.pixelShaderRecord &&
						selected.pixelTechnique == kObservedPixelTechnique;

					const bool armCurrent = LatchTechniqueObservationIdentity(
						a_diagnosticEpoch, a_correctionGeneration, capture.threadId,
						capture.context, bound.device,
						registryDeviceMatches, episodeIdentityMatches);
					capture.registryDeviceMatches = registryDeviceMatches;
					capture.ownerIdentityMatches = episodeIdentityMatches &&
						capture.threadId == threadToken.ownerThreadId &&
						capture.context == threadToken.contextIdentity &&
						capture.device == threadToken.deviceIdentity;
					const bool identityGate = armCurrent && bound.exceptionCode == 0 &&
						bound.device && capture.pixelShaderMatchesRecord &&
						registryDeviceMatches && capture.ownerIdentityMatches &&
						bound.contextType == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
						bound.multithreadInterface && bound.multithreadProtected;
					const bool bindingGate = identityGate && bound.rangeKnown &&
						bound.cb2 && bound.cb8 && bound.cb12 &&
						BufferRangeCoversShaderRows(
							bound.cb2, bound.firstConstant[0], bound.numConstants[0], 3) &&
						BufferRangeCoversShaderRows(
							bound.cb8, bound.firstConstant[1], bound.numConstants[1], 1) &&
						BufferRangeCoversShaderRows(
							bound.cb12, bound.firstConstant[2], bound.numConstants[2],
							kObservedCameraRows);
					if (!identityGate) {
						failure = !armCurrent ? TechniqueObservationFailure::kStaleArm :
							bound.exceptionCode != 0 ? TechniqueObservationFailure::kBoundStateFailed :
								!capture.pixelShaderMatchesRecord ?
									TechniqueObservationFailure::kShaderIdentityMismatch :
									TechniqueObservationFailure::kOwnerIdentityMismatch;
					} else if (!bindingGate) {
						failure = episodeIdentityMatches ?
							TechniqueObservationFailure::kBindingsMissing :
							TechniqueObservationFailure::kEpisodeIdentityMismatch;
					} else if (ReserveTechniqueObservationAttempt(
							a_diagnosticEpoch, a_correctionGeneration)) {
						// The attempt is consumed immediately before this call's first CopyResource.
						attempted = true;
						evidenceRead = ReadTechniqueObservationEvidence(
							bound.device, a_context, bound, capture, readHr, readException);
						if (!evidenceRead) {
							failure = TechniqueObservationFailure::kReadbackFailed;
						}
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
					failure = TechniqueObservationFailure::kBoundStateFailed;
					evidenceRead = false;
				}
			} __finally {
				__try {
					ReleaseTechniqueObservationBoundState(bound);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					if (exceptionCode == 0) {
						exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
					}
					failure = TechniqueObservationFailure::kBoundStateFailed;
					evidenceRead = false;
				}
				std::uint32_t leaveException = 0;
				left = LeaveTechniqueObservationEpoch(threadToken, leaveException);
				if (!left) {
					failure = TechniqueObservationFailure::kLeaveFailed;
					exceptionCode = leaveException;
					evidenceRead = false;
				}
			}
			if (!left) {
				g_exceptionCode.store(exceptionCode, std::memory_order_relaxed);
				g_terminalStateUncertain.store(true, std::memory_order_release);
				g_ready.store(false, std::memory_order_relaxed);
				RecordDecline(Decline::kStateUncertain, E_FAIL, exceptionCode);
				RecordTechniqueObservationFailure(
					a_diagnosticEpoch, failure, attempted, E_FAIL, exceptionCode);
				return false;
			}
			const bool armCurrent = g_diagnosticsEnabled.load(std::memory_order_acquire) &&
				a_diagnosticEpoch == g_techniqueObservationEpoch.load(std::memory_order_acquire) &&
				a_correctionGeneration == g_generation.load(std::memory_order_acquire) && Enabled();
			if (!armCurrent) {
				return true;
			}
			if (!attempted) {
				if (failure != TechniqueObservationFailure::kNone) {
					RecordTechniqueObservationFailure(
						a_diagnosticEpoch, failure, false, readHr,
						boundException != 0 ? boundException : exceptionCode);
				}
				return true;
			}
			if (!evidenceRead) {
				RecordTechniqueObservationFailure(
					a_diagnosticEpoch, failure, true, readHr,
					readException != 0 ? readException : exceptionCode);
				return true;
			}
			static_cast<void>(PublishTechniqueObservation(
				a_diagnosticEpoch, a_correctionGeneration, capture));
			return true;
		}

		[[nodiscard]] std::uint64_t NextPreliminaryDrawSerial(
			std::uint64_t a_generation) noexcept
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!Enabled() || a_generation != g_generation.load(std::memory_order_acquire)) {
				return 0;
			}
			return ++g_preliminaryDrawSerial;
		}

		[[nodiscard]] PreliminaryRatioClass ClassifyPreliminaryRatio(
			bool a_ratiosRead,
			float a_ratioWidth,
			float a_ratioHeight) noexcept
		{
			if (!a_ratiosRead ||
				!std::isfinite(a_ratioWidth) || !std::isfinite(a_ratioHeight) ||
				a_ratioWidth <= 0.0f || a_ratioHeight <= 0.0f ||
				a_ratioWidth > 1.0f || a_ratioHeight > 1.0f) {
				return PreliminaryRatioClass::kInvalid;
			}
			return a_ratioWidth == 1.0f && a_ratioHeight == 1.0f ?
				PreliminaryRatioClass::kNative : PreliminaryRatioClass::kReduced;
		}

		[[nodiscard]] bool RecordPreliminarySampleLocked(
			PreliminaryPixelShaderSample a_sample,
			PreliminaryRatioClass a_ratioClass,
			std::uint64_t a_drawSerial,
			PreliminaryPixelShaderSample* a_samples,
			std::size_t& a_sampleCount,
			std::size_t a_capacity,
			std::uint64_t& a_overflow) noexcept
		{
			for (std::size_t i = 0; i < a_sampleCount; ++i) {
				auto& present = a_samples[i];
				if (present.observedPixelShader == a_sample.observedPixelShader &&
					present.pixelShaderRecord == a_sample.pixelShaderRecord &&
					present.pixelTechnique == a_sample.pixelTechnique &&
					present.threadId == a_sample.threadId &&
					present.exceptionCode == a_sample.exceptionCode &&
					present.kind == a_sample.kind &&
					present.registryKnown == a_sample.registryKnown) {
					present.firstDrawSerial = (std::min)(present.firstDrawSerial, a_drawSerial);
					present.lastDrawSerial = (std::max)(present.lastDrawSerial, a_drawSerial);
					++present.count;
					if (a_ratioClass == PreliminaryRatioClass::kReduced) {
						if (present.firstReducedDrawSerial == 0) {
							present.firstReducedDrawSerial = a_drawSerial;
						} else {
							present.firstReducedDrawSerial =
								(std::min)(present.firstReducedDrawSerial, a_drawSerial);
						}
						present.lastReducedDrawSerial =
							(std::max)(present.lastReducedDrawSerial, a_drawSerial);
						++present.reducedCount;
					} else if (a_ratioClass == PreliminaryRatioClass::kNative) {
						++present.nativeCount;
					} else {
						++present.invalidRatioCount;
					}
					return false;
				}
			}
			if (a_sampleCount >= a_capacity) {
				++a_overflow;
				return true;
			}
			a_sample.firstDrawSerial = a_drawSerial;
			a_sample.lastDrawSerial = a_drawSerial;
			a_sample.count = 1;
			if (a_ratioClass == PreliminaryRatioClass::kReduced) {
				a_sample.reducedCount = 1;
				a_sample.firstReducedDrawSerial = a_drawSerial;
				a_sample.lastReducedDrawSerial = a_drawSerial;
			} else if (a_ratioClass == PreliminaryRatioClass::kNative) {
				a_sample.nativeCount = 1;
			} else {
				a_sample.invalidRatioCount = 1;
			}
			a_samples[a_sampleCount++] = a_sample;
			return false;
		}

		void RecordAdmittedPreliminaryMatch(
			const PixelShaderQuery& a_query,
			const RegisteredTechnique& a_record,
			PreliminaryRatioClass a_ratioClass,
			std::uint64_t a_generation,
			std::uint64_t a_drawSerial) noexcept
		{
			if (a_drawSerial == 0) {
				return;
			}
			PreliminaryPixelShaderSample sample{};
			sample.observedPixelShader = reinterpret_cast<std::uintptr_t>(a_query.pixelShader);
			sample.pixelShaderRecord = a_record.pixelShaderRecord;
			sample.pixelTechnique = a_record.pixelTechnique;
			sample.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			sample.exceptionCode = a_query.exceptionCode;
			sample.kind = PreliminaryPixelShaderKind::kAdmitted;
			sample.registryKnown = a_record.pixelShaderRecord != 0 &&
				a_record.pixelShader == a_query.pixelShader;

			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!Enabled() || a_generation != g_generation.load(std::memory_order_acquire)) {
				return;
			}
			++g_preliminaryAdmittedMatches;
			switch (a_ratioClass) {
			case PreliminaryRatioClass::kReduced:
				++g_preliminaryAdmittedReduced;
				break;
			case PreliminaryRatioClass::kNative:
				++g_preliminaryAdmittedNative;
				break;
			case PreliminaryRatioClass::kInvalid:
				++g_preliminaryAdmittedInvalidRatio;
				break;
			}
			static_cast<void>(RecordPreliminarySampleLocked(
				sample, a_ratioClass, a_drawSerial,
				g_admittedPreliminarySamples, g_admittedPreliminarySampleCount,
				kMaxAdmittedPreliminarySamples, g_preliminaryAdmittedOverflow));
		}

		void RecordRejectedPixelShader(
			const PixelShaderQuery& a_query,
			const RegisteredTechnique& a_record,
			PreliminaryRatioClass a_ratioClass,
			std::uint64_t a_generation,
			std::uint64_t a_drawSerial) noexcept
		{
			if (a_drawSerial == 0) {
				return;
			}
			PreliminaryPixelShaderSample sample{};
			sample.observedPixelShader =
				reinterpret_cast<std::uintptr_t>(a_query.pixelShader);
			sample.pixelShaderRecord = a_record.pixelShaderRecord;
			sample.pixelTechnique = a_record.pixelTechnique;
			sample.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			sample.exceptionCode = a_query.exceptionCode;
			sample.registryKnown = a_record.pixelShaderRecord != 0 &&
				a_record.pixelShader == a_query.pixelShader;
			if (a_query.exceptionCode != 0) {
				sample.kind = PreliminaryPixelShaderKind::kException;
			} else if (!a_query.pixelShader) {
				sample.kind = PreliminaryPixelShaderKind::kNull;
			} else if (sample.registryKnown) {
				sample.kind = PreliminaryPixelShaderKind::kKnownOther;
			} else {
				sample.kind = PreliminaryPixelShaderKind::kUnknown;
			}

			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!Enabled() || a_generation != g_generation.load(std::memory_order_acquire)) {
				return;
			}
			if (a_ratioClass == PreliminaryRatioClass::kNative) {
				++g_rejectedNativeRatio;
			} else if (a_ratioClass == PreliminaryRatioClass::kInvalid) {
				++g_rejectedInvalidRatio;
			} else {
				switch (sample.kind) {
				case PreliminaryPixelShaderKind::kAdmitted:
					break;
				case PreliminaryPixelShaderKind::kKnownOther:
					++g_rejectedKnownOtherReduced;
					break;
				case PreliminaryPixelShaderKind::kUnknown:
					++g_rejectedUnknownReduced;
					break;
				case PreliminaryPixelShaderKind::kNull:
					++g_rejectedNullReduced;
					break;
				case PreliminaryPixelShaderKind::kException:
					++g_rejectedExceptionsReduced;
					break;
				}
			}
			const bool knownOther = sample.kind == PreliminaryPixelShaderKind::kKnownOther;
			auto* const samples = knownOther ?
				g_knownOtherPreliminarySamples : g_unknownPreliminarySamples;
			auto& sampleCount = knownOther ?
				g_knownOtherPreliminarySampleCount : g_unknownPreliminarySampleCount;
			const auto capacity = knownOther ?
				kMaxKnownOtherPreliminarySamples : kMaxUnknownPreliminarySamples;
			auto& poolOverflow = knownOther ?
				g_knownOtherPreliminaryOverflow : g_unknownPreliminaryOverflow;
			const bool overflowed = RecordPreliminarySampleLocked(
				sample, a_ratioClass, a_drawSerial,
				samples, sampleCount, capacity, poolOverflow);
			if (overflowed) {
				++g_rejectedCensusOverflow;
				const auto kindIndex = static_cast<std::uint32_t>(sample.kind) - 1;
				if (kindIndex < 4) {
					++g_rejectedOverflowByKind[kindIndex];
				}
			}
		}

		[[nodiscard]] std::string PreliminaryCensusJsonLocked()
		{
			std::string out = fmt::format(
				FMT_STRING(R"({{"counts":{{"exactCallerDraws":{},"wrongCallerRoutes":{},"admittedMatches":{},"admittedReduced":{},"admittedNative":{},"admittedInvalidRatio":{},"knownOtherReduced":{},"unknownReduced":{},"nullReduced":{},"exceptionsReduced":{},"rejectedNativeRatio":{},"rejectedInvalidRatio":{},"admittedDistinct":{},"knownOtherDistinct":{},"unknownPoolDistinct":{},"admittedOverflow":{},"rejectedOverflow":{},"knownOtherOverflow":{},"unknownPoolOverflow":{},"unknownOverflow":{},"nullOverflow":{},"exceptionsOverflow":{}}},"capacity":{},"overflow":{},"observations":[)"),
				g_preliminaryDrawSerial,
				g_wrongCallerRoutes.load(std::memory_order_relaxed), g_preliminaryAdmittedMatches,
				g_preliminaryAdmittedReduced, g_preliminaryAdmittedNative,
				g_preliminaryAdmittedInvalidRatio, g_rejectedKnownOtherReduced,
				g_rejectedUnknownReduced, g_rejectedNullReduced,
				g_rejectedExceptionsReduced, g_rejectedNativeRatio,
				g_rejectedInvalidRatio, g_admittedPreliminarySampleCount,
				g_knownOtherPreliminarySampleCount, g_unknownPreliminarySampleCount,
				g_preliminaryAdmittedOverflow, g_rejectedCensusOverflow,
				g_knownOtherPreliminaryOverflow, g_unknownPreliminaryOverflow,
				g_rejectedOverflowByKind[1], g_rejectedOverflowByKind[2],
				g_rejectedOverflowByKind[3],
				kMaxAdmittedPreliminarySamples + kMaxKnownOtherPreliminarySamples +
					kMaxUnknownPreliminarySamples,
				g_preliminaryAdmittedOverflow + g_rejectedCensusOverflow);
			bool first = true;
			const auto appendSample = [&out, &first](const PreliminaryPixelShaderSample& a_sample) {
				if (!first) {
					out += ',';
				}
				first = false;
				const auto kind = a_sample.kind == PreliminaryPixelShaderKind::kAdmitted ?
					"admitted"sv : a_sample.kind == PreliminaryPixelShaderKind::kKnownOther ?
						"knownOther"sv : a_sample.kind == PreliminaryPixelShaderKind::kUnknown ?
							"unknown"sv : a_sample.kind == PreliminaryPixelShaderKind::kNull ?
								"null"sv : "exception"sv;
				out += fmt::format(
					FMT_STRING(R"({{"kind":"{}","registryKnown":{},"observedPixelShader":"{:016X}","pixelShaderRecord":"{:016X}","pixelTechnique":{},"threadId":{},"exceptionCode":{},"firstDrawSerial":{},"lastDrawSerial":{},"count":{},"reducedCount":{},"nativeCount":{},"invalidRatioCount":{},"firstReducedDrawSerial":{},"lastReducedDrawSerial":{}}})"),
					kind, a_sample.registryKnown ? "true"sv : "false"sv,
					a_sample.observedPixelShader, a_sample.pixelShaderRecord,
					a_sample.pixelTechnique, a_sample.threadId, a_sample.exceptionCode,
					a_sample.firstDrawSerial, a_sample.lastDrawSerial, a_sample.count,
					a_sample.reducedCount, a_sample.nativeCount, a_sample.invalidRatioCount,
					a_sample.firstReducedDrawSerial, a_sample.lastReducedDrawSerial);
			};
			for (std::size_t i = 0; i < g_admittedPreliminarySampleCount; ++i) {
				appendSample(g_admittedPreliminarySamples[i]);
			}
			for (std::size_t i = 0; i < g_knownOtherPreliminarySampleCount; ++i) {
				appendSample(g_knownOtherPreliminarySamples[i]);
			}
			for (std::size_t i = 0; i < g_unknownPreliminarySampleCount; ++i) {
				appendSample(g_unknownPreliminarySamples[i]);
			}
			out += "]}";
			return out;
		}

		[[nodiscard]] std::string CorrectionObservationsJsonLocked()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < g_correctionCaptureCount; ++i) {
				const auto& sample = g_correctionCaptures[i];
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"({{"serial":{},"generation":{},"threadId":{},"callerReturn":"{:016X}","callerReturnRva":"{:X}","packet":"{:016X}","packetDrawCount":{},"pixelTechnique":{},"pixelShaderRecord":"{:016X}","boundPixelShader":"{:016X}","originalCamera":"{:016X}","privateCamera":"{:016X}","firstConstant":{},"numConstants":{},"ratios":[{},{}],"viewport":[{},{}],"nativeDimensionSource":"boundRtv","nativeDimensions":[{},{}],"basis":"{}","derivedRatios":[{},{}],"cb8Stereo":{},"stereoBasisValidated":{},"maskKnown":{},"correctedMask":{},"skippedMask":{},"completeReconstruction":{},"privateBindingVerified":{},"helperCalled":{},"originalBindingVerified":{},"cb2":)"),
					sample.serial, sample.generation, sample.threadId,
					sample.callerReturn, sample.callerReturnRva, sample.packet,
					sample.packetDrawCount, sample.pixelTechnique, sample.pixelShaderRecord,
					sample.boundPixelShader, sample.originalCamera, sample.privateCamera,
					sample.firstConstant, sample.numConstants,
					JsonFloat(sample.ratioWidth), JsonFloat(sample.ratioHeight),
					JsonFloat(sample.viewportWidth), JsonFloat(sample.viewportHeight),
					JsonFloat(sample.nativeWidth), JsonFloat(sample.nativeHeight),
					CompositeContracts::Find(sample.pixelTechnique)->dimensionRow == 5 ? "row0TimesRow5"sv : "row0"sv,
					JsonFloat(sample.derivedRatioWidth), JsonFloat(sample.derivedRatioHeight),
					JsonFloat(sample.stereoValue),
					sample.stereoBasisValidated ? "true"sv : "false"sv,
					sample.maskKnown ? "true"sv : "false"sv,
					sample.correctedMask, sample.skippedMask,
					sample.completeReconstruction ? "true"sv : "false"sv,
					sample.privateBindingVerified ? "true"sv : "false"sv,
					sample.helperCalled ? "true"sv : "false"sv,
					sample.originalBindingVerified ? "true"sv : "false"sv);
				AppendBufferJson(out, sample.cb2, false);
				out += R"(,"cb8":)";
				AppendBufferJson(out, sample.cb8, false);
				out += R"(,"source":)";
				AppendBufferJson(out, sample.source, true);
				out += R"(,"privateCopy":)";
				AppendBufferJson(out, sample.privateCopy, true);
				out += '}';
			}
			out += ']';
			return out;
		}

		[[nodiscard]] std::string Technique65600CorrectionObservationsJsonLocked()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < g_technique65600CorrectionCaptureCount; ++i) {
				const auto& sample = g_technique65600CorrectionCaptures[i];
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"({{"route":"exactTechnique65600PrivateCamera","serial":{},"generation":{},"correctionEpoch":{},"resourceEpoch":{},"threadId":{},"expectedOwnerThreadId":{},"callerReturn":"{:016X}","callerReturnRva":"{:X}","packet":"{:016X}","packetDrawCount":{},"pixelTechnique":{},"pixelShaderRecord":"{:016X}","expectedPixelShader":"{:016X}","boundPixelShader":"{:016X}","context":"{:016X}","device":"{:016X}","engineCameraBuffer":"{:016X}","contextType":{},"registryDeviceMatches":{},"ownerIdentityMatches":{},"cb12MatchesEngine":{},"multithreadInterface":{},"multithreadProtected":{},"dimensionCacheMatched":{},"originalCamera":"{:016X}","privateCamera":"{:016X}","firstConstant":{},"numConstants":{},"globalRatios":[{},{}],"derivedRatios":[{},{}],"scales":[{},{}],"nativeDimensionSource":"cb2","cb2NativeDimensions":[{},{}],"viewport":[{},{},{},{},{},{}],"cb8Stereo":{},"stereoBasisValidated":{},"maskKnown":{},"correctedMask":{},"skippedMask":{},"expectedCorrectedMask":{},"expectedSkippedMask":{},"completeReconstruction":{},"expectedPartialReconstruction":{},"privateBindingVerified":{},"helperCalled":{},"originalBindingVerified":{},"cb2":)"),
					sample.serial, sample.generation, sample.correctionEpoch,
					sample.resourceEpoch, sample.threadId,
					sample.expectedOwnerThreadId,
					sample.callerReturn, sample.callerReturnRva, sample.packet,
					sample.packetDrawCount, sample.pixelTechnique, sample.pixelShaderRecord,
					sample.expectedPixelShader, sample.boundPixelShader,
					sample.context, sample.device, sample.engineCameraBuffer,
					sample.contextType,
					sample.registryDeviceMatches ? "true"sv : "false"sv,
					sample.ownerIdentityMatches ? "true"sv : "false"sv,
					sample.cb12MatchesEngine ? "true"sv : "false"sv,
					sample.multithreadInterface ? "true"sv : "false"sv,
					sample.multithreadProtected ? "true"sv : "false"sv,
					sample.dimensionCacheMatched ? "true"sv : "false"sv,
					sample.originalCamera, sample.privateCamera,
					sample.firstConstant, sample.numConstants,
					JsonFloat(sample.ratioWidth), JsonFloat(sample.ratioHeight),
					JsonFloat(sample.derivedRatioWidth), JsonFloat(sample.derivedRatioHeight),
					JsonFloat(sample.scaleX), JsonFloat(sample.scaleY),
					JsonFloat(sample.nativeWidth), JsonFloat(sample.nativeHeight),
					JsonFloat(sample.viewportTopLeftX), JsonFloat(sample.viewportTopLeftY),
					JsonFloat(sample.viewportWidth), JsonFloat(sample.viewportHeight),
					JsonFloat(sample.viewportMinDepth), JsonFloat(sample.viewportMaxDepth),
					JsonFloat(sample.stereoValue),
					sample.stereoBasisValidated ? "true"sv : "false"sv,
					sample.maskKnown ? "true"sv : "false"sv,
					sample.correctedMask, sample.skippedMask,
					kTechnique65600ExpectedCorrectedMask,
					kTechnique65600ExpectedSkippedMask,
					sample.completeReconstruction ? "true"sv : "false"sv,
					sample.expectedPartialReconstruction ? "true"sv : "false"sv,
					sample.privateBindingVerified ? "true"sv : "false"sv,
					sample.helperCalled ? "true"sv : "false"sv,
					sample.originalBindingVerified ? "true"sv : "false"sv);
				AppendBufferJson(out, sample.cb2, false);
				out += R"(,"cb8":)";
				AppendBufferJson(out, sample.cb8, false);
				out += R"(,"source":)";
				AppendBufferJson(out, sample.source, true);
				out += R"(,"privateCopy":)";
				AppendBufferJson(out, sample.privateCopy, true);
				out += '}';
			}
			out += ']';
			return out;
		}

		[[nodiscard]] std::string Technique65600ObservationsJsonLocked()
		{
			std::string out{ "[" };
			for (std::size_t i = 0; i < g_techniqueObservationCaptureCount; ++i) {
				const auto& sample = g_techniqueObservationCaptures[i];
				if (i != 0) {
					out += ',';
				}
				out += fmt::format(
					FMT_STRING(R"({{"route":"preparedPacketDrawPreHelper","serial":{},"correctionGeneration":{},"diagnosticEpoch":{},"threadId":{},"expectedRenderThreadId":{},"callerReturn":"{:016X}","callerReturnRva":"{:X}","packet":"{:016X}","packetDrawCount":{},"pixelTechnique":{},"pixelShaderRecord":"{:016X}","expectedPixelShader":"{:016X}","boundPixelShader":"{:016X}","pixelShaderMatchesRecord":{},"context":"{:016X}","device":"{:016X}","engineCameraBuffer":"{:016X}","contextType":{},"registryDeviceMatches":{},"ownerIdentityMatches":{},"cb12MatchesEngine":{},"multithreadInterface":{},"multithreadProtected":{},"ratiosRead":{},"ratiosValid":{},"ratios":[{},{}],"viewportCount":{},"viewports":[)"),
					sample.serial,
					sample.correctionGeneration,
					sample.diagnosticEpoch,
					sample.threadId,
					sample.expectedRenderThreadId,
					sample.callerReturn,
					sample.callerReturnRva,
					sample.packet,
					sample.packetDrawCount,
					sample.pixelTechnique,
					sample.pixelShaderRecord,
					sample.expectedPixelShader,
					sample.boundPixelShader,
					sample.pixelShaderMatchesRecord ? "true"sv : "false"sv,
					sample.context,
					sample.device,
					sample.engineCameraBuffer,
					sample.contextType,
					sample.registryDeviceMatches ? "true"sv : "false"sv,
					sample.ownerIdentityMatches ? "true"sv : "false"sv,
					sample.cb12MatchesEngine ? "true"sv : "false"sv,
					sample.multithreadInterface ? "true"sv : "false"sv,
					sample.multithreadProtected ? "true"sv : "false"sv,
					sample.ratiosRead ? "true"sv : "false"sv,
					sample.ratiosValid ? "true"sv : "false"sv,
					JsonFloat(sample.ratioWidth),
					JsonFloat(sample.ratioHeight),
					sample.viewportCount);
				for (std::uint32_t viewport = 0; viewport < sample.viewportCount; ++viewport) {
					if (viewport != 0) {
						out += ',';
					}
					const auto& value = sample.viewports[viewport];
					out += fmt::format(
						FMT_STRING("[{},{},{},{},{},{}]"),
						JsonFloat(value.TopLeftX), JsonFloat(value.TopLeftY),
						JsonFloat(value.Width), JsonFloat(value.Height),
						JsonFloat(value.MinDepth), JsonFloat(value.MaxDepth));
				}
				out += fmt::format(
					FMT_STRING(R"(],"cb2NativeDimensions":[{},{}],"cb8Stereo":{},"stereoBasisValidated":{},"cameraRangeCoversShaderRows":{},"shaderDeclaredResourceSlots":[0,3,4,5,7,11],"observedUnusedSlot":8,"resources":[)"),
					JsonFloat(sample.nativeWidth), JsonFloat(sample.nativeHeight),
					JsonFloat(sample.stereoValue),
					sample.stereoBasisValidated ? "true"sv : "false"sv,
					sample.cameraRangeCoversShaderRows ? "true"sv : "false"sv);
				for (std::size_t resource = 0; resource < kObservedResourceCount; ++resource) {
					if (resource != 0) {
						out += ',';
					}
					AppendShaderResourceJson(out, sample.resources[resource]);
				}
				out += R"(],"cb2":)";
				AppendBufferJson(out, sample.cb2, false);
				out += R"(,"cb8":)";
				AppendBufferJson(out, sample.cb8, false);
				out += R"(,"cb12":)";
				AppendBufferJson(out, sample.cb12, false);
				out += '}';
			}
			out += ']';
			return out;
		}

		void ReleaseDeferredEpochObjects() noexcept
		{
			ReleaseCorrectionResources();
			const std::lock_guard<std::mutex> guard{ g_lock };
			ResetTableRegistryLocked();
		}

		[[nodiscard]] bool DrainRequestedRelease() noexcept
		{
			if (!g_releaseRequested.load(std::memory_order_acquire)) {
				return true;
			}
			const auto threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			if (g_ownerThreadId != 0 && g_ownerThreadId != threadId) {
				return false;
			}

			PreparedDrawToken releaseToken{};
			std::uint32_t exceptionCode = 0;
			if (g_correctionResourcesReady) {
				if (!EnterExistingMultithread(releaseToken, exceptionCode)) {
					if (exceptionCode != 0) {
						g_exceptionCode.store(exceptionCode, std::memory_order_relaxed);
						g_terminalStateUncertain.store(true, std::memory_order_release);
					}
					return false;
				}
			}

			bool released = false;
			bool left = !releaseToken.entered;
			__try {
				__try {
					ReleaseDeferredEpochObjects();
					released = true;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					exceptionCode = static_cast<std::uint32_t>(::GetExceptionCode());
					g_exceptionCode.store(exceptionCode, std::memory_order_relaxed);
					g_terminalStateUncertain.store(true, std::memory_order_release);
				}
			} __finally {
				if (releaseToken.entered) {
					left = LeaveMultithread(releaseToken, exceptionCode);
					if (!left) {
						g_exceptionCode.store(exceptionCode, std::memory_order_relaxed);
						g_terminalStateUncertain.store(true, std::memory_order_release);
					}
				}
			}
			if (released && left) {
				g_ready.store(false, std::memory_order_relaxed);
				g_releaseRequested.store(false, std::memory_order_release);
			}
			return released && left;
		}

		void RecordFamilyLegacyDecline(
			PreparedDrawToken& a_token,
			std::uint64_t a_epoch,
			std::uint64_t a_generation,
			Technique65600CorrectionDecline a_decline,
			std::int32_t a_hr = S_OK,
			std::uint32_t a_exception = 0) noexcept
		{
			a_token.coverageLegacyDecline = static_cast<std::uint32_t>(a_decline);
			RecordTechnique65600CorrectionDecline(a_epoch, a_generation, a_decline, a_hr, a_exception);
		}

		void BeginTechnique65600Correction(
			ID3D11DeviceContext* a_context,
			const PixelShaderQuery& a_preliminary,
			const RegisteredTechnique& a_preliminaryRecord,
			std::uintptr_t a_callerReturn,
			std::uintptr_t a_callerRva,
			std::uintptr_t a_packet,
			std::uint32_t a_packetDrawCount,
			bool a_ratiosRead,
			float a_globalRatioWidth,
			float a_globalRatioHeight,
			std::uint64_t a_generation,
			std::uint64_t a_correctionEpoch,
			PreparedDrawToken& a_token) noexcept
		{
			if (!CountTechnique65600CorrectionEvent(
					g_technique65600CorrectionCandidates,
					a_correctionEpoch, a_generation)) {
				return;
			}
			if (!a_ratiosRead || !std::isfinite(a_globalRatioWidth) ||
				!std::isfinite(a_globalRatioHeight) || a_globalRatioWidth <= 0.0f ||
				a_globalRatioHeight <= 0.0f || a_globalRatioWidth > 1.0f ||
				a_globalRatioHeight > 1.0f) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kViewportContract, E_INVALIDARG);
				return;
			}
			if (a_globalRatioWidth == 1.0f && a_globalRatioHeight == 1.0f) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kNativePassThrough);
				return;
			}

			Technique65600CorrectionDecline decline =
				Technique65600CorrectionDecline::kNone;
			std::uint32_t exceptionCode = 0;
			if (!EnterTechnique65600CorrectionEpoch(
					a_context, a_generation, a_correctionEpoch,
					a_token, decline, exceptionCode)) {
				if (exceptionCode != 0) {
					RecordTechnique65600TerminalFailure(
						a_correctionEpoch, a_generation,
						Technique65600CorrectionDecline::kStateUncertain,
						E_FAIL, exceptionCode);
					a_token.callOriginal = false;
				} else {
					RecordFamilyLegacyDecline(a_token,
						a_correctionEpoch, a_generation, decline, E_FAIL);
				}
				return;
			}
			a_token.technique65600Correction = true;
			a_token.technique65600CorrectionEpoch = a_correctionEpoch;

			auto bound = QueryBoundStateIsolated(a_context);
			const auto selected = FindRegisteredTechnique(bound.pixelShader);
			bool registryDeviceMatches = false;
			{
				const std::lock_guard<std::mutex> guard{ g_lock };
				registryDeviceMatches = bound.device && bound.device == g_registryDevice;
			}
			const auto engineCamera = EngineCameraBuffer();
			const bool fullShaderIdentity = bound.exceptionCode == 0 && bound.device &&
				bound.pixelShader && a_preliminary.exceptionCode == 0 &&
				a_preliminary.pixelShader == bound.pixelShader &&
				selected.pixelTechnique == kObservedPixelTechnique &&
				selected.pixelShaderRecord == a_preliminaryRecord.pixelShaderRecord &&
				selected.pixelShader == bound.pixelShader && registryDeviceMatches;
			const bool bufferContract = fullShaderIdentity &&
				bound.contextType == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
				bound.multithreadInterface && bound.multithreadProtected &&
				bound.cb2 && bound.cb8 && bound.cb12 &&
				reinterpret_cast<std::uintptr_t>(bound.cb12) == engineCamera &&
				bound.rangeKnown &&
				bound.firstConstant[0] == kExpectedFirstConstant &&
				bound.firstConstant[1] == kExpectedFirstConstant &&
				bound.firstConstant[2] == kExpectedFirstConstant &&
				bound.numConstants[0] == kExpectedNumConstants &&
				bound.numConstants[1] == kExpectedNumConstants &&
				bound.numConstants[2] == kExpectedNumConstants &&
				BufferRangeCoversShaderRows(
					bound.cb2, bound.firstConstant[0], bound.numConstants[0], 3) &&
				BufferRangeCoversShaderRows(
					bound.cb8, bound.firstConstant[1], bound.numConstants[1], 1) &&
				BufferRangeCoversShaderRows(
					bound.cb12, bound.firstConstant[2], bound.numConstants[2],
					kObservedCameraRows) &&
				BufferDescriptionMatchesCamera(bound.cb12);
			if (!bufferContract) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					bound.exceptionCode != 0 ?
						Technique65600CorrectionDecline::kBindingsMissing :
						(!fullShaderIdentity ?
							Technique65600CorrectionDecline::kShaderIdentityMismatch :
							Technique65600CorrectionDecline::kBufferContract),
					E_INVALIDARG, bound.exceptionCode);
				ReleaseBoundState(bound);
				return;
			}

			// Correction owns a complete resource epoch even when 65600 is the first
			// eligible family member. Malformed draws have already failed closed above.
			const auto threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			if (g_ownerThreadId != 0 && g_ownerThreadId != threadId) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kWrongRenderThread, RPC_E_WRONG_THREAD);
				ReleaseBoundState(bound);
				return;
			}
			if (!Technique65600CorrectionCurrent(a_correctionEpoch, a_generation)) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kStaleEpisode);
				ReleaseBoundState(bound);
				return;
			}
			std::int32_t resourceHr = S_OK;
			if (!EnsureCorrectionResources(bound.device, a_context, resourceHr)) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kProtectionUnavailable, resourceHr);
				ReleaseBoundState(bound);
				return;
			}
			if (!Technique65600CorrectionCurrent(a_correctionEpoch, a_generation)) {
				g_releaseRequested.store(true, std::memory_order_release);
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kStaleEpisode);
				ReleaseBoundState(bound);
				return;
			}
			if (g_ownerThreadId != threadId) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kWrongRenderThread, RPC_E_WRONG_THREAD);
				ReleaseBoundState(bound);
				return;
			}
			if (g_correctionContext != a_context || !g_correctionDevice ||
				g_protectionDevice != g_correctionDevice || !g_correctionContext1) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kOwnerIdentityMismatch, E_FAIL);
				ReleaseBoundState(bound);
				return;
			}

			const auto resourceEpoch = g_resourceEpoch.load(std::memory_order_acquire);
			const auto dimensions = Technique65600NativeDimensions(
				bound.device, a_context, bound.cb2,
				bound.firstConstant[0], bound.numConstants[0],
				a_correctionEpoch, a_generation, resourceEpoch);
			if (!dimensions.valid) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation, dimensions.decline,
					dimensions.hr, dimensions.exceptionCode);
				ReleaseBoundState(bound);
				return;
			}

			float derivedRatioWidth = 0.0f;
			float derivedRatioHeight = 0.0f;
			if (!Technique65600ViewportContract(
					bound, dimensions.width, dimensions.height,
					a_globalRatioWidth, a_globalRatioHeight,
					derivedRatioWidth, derivedRatioHeight)) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kViewportContract, E_INVALIDARG);
				ReleaseBoundState(bound);
				return;
			}

			if (!Technique65600CorrectionCurrent(a_correctionEpoch, a_generation) ||
				resourceEpoch != g_resourceEpoch.load(std::memory_order_acquire)) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kStaleEpisode);
				ReleaseBoundState(bound);
				return;
			}

			if (!CountTechnique65600CorrectionEvent(
					g_technique65600CorrectionAttempts,
					a_correctionEpoch, a_generation)) {
				ReleaseBoundState(bound);
				return;
			}
			a_token.coverageAdmitted = true;
			const auto preparation = PreparePrivateCamera(
				a_context, g_correctionContext1, bound.cb12, bound.cb2, bound.cb8,
				derivedRatioWidth, derivedRatioHeight, bound.viewports[0],
				bound.firstConstant, bound.numConstants);
			if (preparation.stateUncertain) {
				RecordTechnique65600TerminalFailure(
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kStateUncertain,
					preparation.hr, preparation.exceptionCode);
				a_token.callOriginal = false;
				ReleaseBoundState(bound);
				return;
			}
			if (!preparation.began || !preparation.dispatched || !preparation.ended) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kPrivatePreparationFailed,
					preparation.hr, preparation.exceptionCode);
				ReleaseBoundState(bound);
				return;
			}

			CorrectionCapture capture{};
			const bool captureWanted = ReserveTechnique65600CorrectionCapture(
				a_correctionEpoch, a_generation);
			if (captureWanted) {
				capture.generation = a_generation;
				capture.correctionEpoch = a_correctionEpoch;
				capture.resourceEpoch = dimensions.resourceEpoch;
				capture.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
				capture.expectedOwnerThreadId = g_ownerThreadId;
				capture.callerReturn = a_callerReturn;
				capture.callerReturnRva = a_callerRva;
				capture.packet = a_packet;
				capture.packetDrawCount = a_packetDrawCount;
				capture.pixelTechnique = selected.pixelTechnique;
				capture.pixelShaderRecord = selected.pixelShaderRecord;
				capture.expectedPixelShader =
					reinterpret_cast<std::uintptr_t>(selected.pixelShader);
				capture.boundPixelShader = reinterpret_cast<std::uintptr_t>(bound.pixelShader);
				capture.context = reinterpret_cast<std::uintptr_t>(a_context);
				capture.device = reinterpret_cast<std::uintptr_t>(bound.device);
				capture.engineCameraBuffer = engineCamera;
				capture.contextType = bound.contextType;
				capture.registryDeviceMatches = registryDeviceMatches;
				capture.ownerIdentityMatches =
					capture.threadId == capture.expectedOwnerThreadId &&
					a_context == g_protectionContext && a_context == g_correctionContext &&
					bound.device == g_protectionDevice && bound.device == g_correctionDevice;
				capture.cb12MatchesEngine =
					reinterpret_cast<std::uintptr_t>(bound.cb12) == engineCamera;
				capture.multithreadInterface = bound.multithreadInterface;
				capture.multithreadProtected = bound.multithreadProtected;
				capture.dimensionCacheMatched = dimensions.cacheIdentityMatched;
				capture.originalCamera = reinterpret_cast<std::uintptr_t>(bound.cb12);
				capture.privateCamera = reinterpret_cast<std::uintptr_t>(g_privateCamera);
				capture.firstConstant = bound.firstConstant[2];
				capture.numConstants = bound.numConstants[2];
				capture.ratioWidth = a_globalRatioWidth;
				capture.ratioHeight = a_globalRatioHeight;
				capture.derivedRatioWidth = derivedRatioWidth;
				capture.derivedRatioHeight = derivedRatioHeight;
				capture.scaleX = 1.0f / derivedRatioWidth;
				capture.scaleY = 1.0f / derivedRatioHeight;
				capture.nativeWidth = dimensions.width;
				capture.nativeHeight = dimensions.height;
				capture.viewportTopLeftX = bound.viewports[0].TopLeftX;
				capture.viewportTopLeftY = bound.viewports[0].TopLeftY;
				capture.viewportWidth = bound.viewports[0].Width;
				capture.viewportHeight = bound.viewports[0].Height;
				capture.viewportMinDepth = bound.viewports[0].MinDepth;
				capture.viewportMaxDepth = bound.viewports[0].MaxDepth;
				if (!ReadCorrectionEvidence(
						bound.device, a_context, bound.cb12, bound.cb2, bound.cb8,
						bound.firstConstant, bound.numConstants, capture)) {
					RecordFamilyLegacyDecline(a_token,
						a_correctionEpoch, a_generation,
						Technique65600CorrectionDecline::kEvidenceReadFailed, E_FAIL);
					ReleaseBoundState(bound);
					return;
				}
				// Exact 65600 has two finite and two nonfinite captured matrices. Its
				// accepted evidence is the separate expected-partial 0x3/0xC contract.
				capture.completeReconstruction = false;
				capture.expectedPartialReconstruction =
					capture.stereoBasisValidated &&
					capture.correctedMask == kTechnique65600ExpectedCorrectedMask &&
					capture.skippedMask == kTechnique65600ExpectedSkippedMask;
			}

			if (!Technique65600CorrectionCurrent(a_correctionEpoch, a_generation) ||
				resourceEpoch != g_resourceEpoch.load(std::memory_order_acquire)) {
				RecordFamilyLegacyDecline(a_token,
					a_correctionEpoch, a_generation,
					Technique65600CorrectionDecline::kStaleEpisode);
				ReleaseBoundState(bound);
				return;
			}

			g_correctionContext1->AddRef();
			a_token.context1 = g_correctionContext1;
			a_token.originalCamera = bound.cb12;
			bound.cb12 = nullptr;
			a_token.firstConstant = bound.firstConstant[2];
			a_token.numConstants = bound.numConstants[2];
			const auto privateBinding = SetAndVerifyCameraBinding(
				a_token.context1, g_privateCamera,
				a_token.firstConstant, a_token.numConstants);
			if (!privateBinding.verified) {
				const auto handback = SetAndVerifyCameraBinding(
					a_token.context1, a_token.originalCamera,
					a_token.firstConstant, a_token.numConstants);
				if (!handback.verified) {
					RecordTechnique65600TerminalFailure(
						a_correctionEpoch, a_generation,
						Technique65600CorrectionDecline::kOriginalRestoreFailed,
						E_FAIL, handback.exceptionCode);
					a_token.callOriginal = false;
				} else {
					RecordFamilyLegacyDecline(a_token,
						a_correctionEpoch, a_generation,
						Technique65600CorrectionDecline::kPrivateBindFailed, E_FAIL,
						privateBinding.exceptionCode);
				}
				{
					const std::lock_guard<std::mutex> guard{ g_lock };
					if (Technique65600CorrectionCurrent(a_correctionEpoch, a_generation)) {
						if (handback.verified) {
							g_technique65600OriginalRestoreVerifications.fetch_add(
								1, std::memory_order_relaxed);
						} else {
							g_technique65600OriginalRestoreFailures.fetch_add(
								1, std::memory_order_relaxed);
						}
					}
				}
				ReleaseBoundState(bound);
				return;
			}

			a_token.privateBound = true;
			a_token.correctionPrepared = true;
			a_token.technique65600MaskKnown = captureWanted && capture.maskKnown;
			a_token.technique65600ExpectedMask =
				captureWanted && capture.expectedPartialReconstruction;
			{
				const std::lock_guard<std::mutex> guard{ g_lock };
				if (Technique65600CorrectionCurrent(a_correctionEpoch, a_generation)) {
					g_technique65600PrivateBindVerifications.fetch_add(
						1, std::memory_order_relaxed);
					g_ready.store(true, std::memory_order_release);
					if (!captureWanted) {
						g_technique65600MaskKnownForAllPrivateDraws.store(
							false, std::memory_order_relaxed);
					} else if (g_technique65600CorrectionCaptureCount <
						kMaxTechnique65600CorrectionSamples) {
						capture.privateBindingVerified = true;
						capture.serial = g_technique65600CorrectionCaptureCount + 1;
						a_token.technique65600CaptureIndex = static_cast<std::uint32_t>(
							g_technique65600CorrectionCaptureCount);
						g_technique65600CorrectionCaptures[
							g_technique65600CorrectionCaptureCount++] = capture;
						g_technique65600LastCorrectedMask.store(
							capture.correctedMask, std::memory_order_relaxed);
						g_technique65600LastSkippedMask.store(
							capture.skippedMask, std::memory_order_relaxed);
						g_technique65600LastMaskKnown.store(true, std::memory_order_relaxed);
						if (!capture.expectedPartialReconstruction) {
							g_technique65600ExpectedPartialReconstruction.store(
								false, std::memory_order_relaxed);
						}
					}
				}
			}
			ReleaseBoundState(bound);
		}

		void FinishFamilyCoverage(
			PreparedDrawToken& a_token,
			std::uint32_t a_finishException,
			std::uint32_t a_leaveException) noexcept
		{
			if (a_token.coverageIndex > CompositeContracts::kRecordCount) {
				return;
			}
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (a_token.coverageIndex <= CompositeContracts::kRecordCount &&
				a_token.generation == g_generation.load(std::memory_order_acquire)) {
				auto& coverage = g_familyCoverage[a_token.coverageIndex];
				++coverage.finished;
				coverage.admitted += a_token.coverageAdmitted ? 1 : 0;
				coverage.privateDraws += a_token.privateBound && a_token.helperCalled ? 1 : 0;
				coverage.restored += a_token.privateBound && a_token.coverageRestored ? 1 : 0;
				coverage.restoreFailures += a_token.privateBound && !a_token.coverageRestored ? 1 : 0;
				if (a_finishException != 0 || a_leaveException != 0) {
					a_token.coverageDecline = static_cast<std::uint32_t>(Decline::kStateUncertain);
				} else if (a_token.privateBound && !a_token.coverageRestored) {
					a_token.coverageDecline = static_cast<std::uint32_t>(Decline::kOriginalRestoreFailed);
				} else if (!a_token.privateBound && a_token.coverageDecline == 0 &&
					a_token.coverageLegacyDecline == 0) {
					a_token.coverageDecline = static_cast<std::uint32_t>(Decline::kNotPrepared);
				}
				if (a_token.coverageDecline < std::size(coverage.declines)) {
					++coverage.declines[a_token.coverageDecline];
				}
				if (a_token.coverageLegacyDecline < std::size(coverage.legacyDeclines)) {
					++coverage.legacyDeclines[a_token.coverageLegacyDecline];
				}
			}
		}

		void FinishPreparedPacketDrawState(PreparedDrawToken& a_token) noexcept
		{
			bool restored = !a_token.privateBound;
			std::uint32_t restoreException = 0;
			if (a_token.privateBound && a_token.context1 && a_token.originalCamera) {
				const auto handback = SetAndVerifyCameraBinding(
					a_token.context1, a_token.originalCamera,
					a_token.firstConstant, a_token.numConstants);
				restored = handback.verified;
				restoreException = handback.exceptionCode;
				if (!restored) {
					g_exceptionCode.store(restoreException, std::memory_order_relaxed);
					g_terminalStateUncertain.store(true, std::memory_order_release);
					g_ready.store(false, std::memory_order_relaxed);
					if (!a_token.technique65600Correction) {
						g_originalRestoreFailures.fetch_add(1, std::memory_order_relaxed);
						RecordDecline(
							Decline::kOriginalRestoreFailed, E_FAIL, restoreException);
					}
				}
			}

			a_token.coverageRestored = restored;
			const std::lock_guard<std::mutex> guard{ g_lock };
			const bool currentEpisode = Enabled() &&
				a_token.generation == g_generation.load(std::memory_order_acquire);
			const bool currentTechnique65600Episode = currentEpisode &&
				a_token.technique65600Correction &&
				g_technique65600CorrectionEnabled.load(std::memory_order_acquire) &&
				a_token.technique65600CorrectionEpoch ==
					g_technique65600CorrectionEpoch.load(std::memory_order_acquire);
			if (a_token.technique65600Correction) {
				if (currentTechnique65600Episode && a_token.privateBound && !restored) {
					g_technique65600OriginalRestoreFailures.fetch_add(
						1, std::memory_order_relaxed);
					g_technique65600CorrectionDeclines.fetch_add(1, std::memory_order_relaxed);
					g_technique65600CorrectionLastDecline.store(
						static_cast<std::uint32_t>(
							Technique65600CorrectionDecline::kOriginalRestoreFailed),
						std::memory_order_relaxed);
					g_technique65600CorrectionLastHr.store(E_FAIL, std::memory_order_relaxed);
					g_technique65600CorrectionExceptionCode.store(
						restoreException, std::memory_order_relaxed);
				}
				if (currentTechnique65600Episode && a_token.privateBound && restored) {
					g_technique65600OriginalRestoreVerifications.fetch_add(
						1, std::memory_order_relaxed);
					if (a_token.helperCalled) {
						g_technique65600PrivateDraws.fetch_add(1, std::memory_order_relaxed);
						if (a_token.technique65600MaskKnown &&
							a_token.technique65600ExpectedMask) {
							g_technique65600CorrectedDraws.fetch_add(
								1, std::memory_order_relaxed);
						}
					}
				}
				if (currentTechnique65600Episode &&
					a_token.technique65600CaptureIndex != 0xFFFFFFFFu &&
					a_token.technique65600CaptureIndex <
						g_technique65600CorrectionCaptureCount) {
					auto& capture = g_technique65600CorrectionCaptures[
						a_token.technique65600CaptureIndex];
					if (capture.generation == a_token.generation &&
						capture.correctionEpoch == a_token.technique65600CorrectionEpoch) {
						capture.helperCalled = a_token.helperCalled;
						capture.originalBindingVerified = restored;
					}
				}
				return;
			}
			if (currentEpisode && a_token.privateBound && restored) {
				g_restores.fetch_add(1, std::memory_order_relaxed);
				g_originalRestoreVerifications.fetch_add(1, std::memory_order_relaxed);
				if (a_token.helperCalled) {
					// This counts helper invocations under a verified private binding. The GPU
					// may have copied all four matrices unchanged when its live basis/finite
					// gate failed; only bounded masks say whether coefficients changed.
					g_correctedDraws.fetch_add(1, std::memory_order_relaxed);
				}
			}
			if (currentEpisode && a_token.captureIndex != 0xFFFFFFFFu) {
				if (a_token.captureIndex < g_correctionCaptureCount) {
					auto& capture = g_correctionCaptures[a_token.captureIndex];
					if (capture.generation == a_token.generation) {
						capture.helperCalled = a_token.helperCalled;
						capture.originalBindingVerified = restored;
					}
				}
			}
		}
	}

	void BeginPreparedPacketDraw(
		std::uintptr_t a_callerReturn,
		std::uintptr_t a_packet,
		PreparedDrawToken& a_token) noexcept
	{
		a_token = {};
		a_token.generation = g_generation.load(std::memory_order_acquire);
		// 141DA51E0 serves every prepared-packet caller, not only the composite pass. The
		// latch stops correction for the session; it never stops the engine's draw. Only a
		// draw during which this module faulted, or could not prove the context state it
		// changed, declines its one helper call below.
		if (g_terminalStateUncertain.load(std::memory_order_acquire)) {
			return;
		}

		auto* const context = Engine::ImmediateContext();
		if (!context) {
			if (Enabled()) {
				RecordFamilyDecline(a_token, Decline::kNoContext);
			}
			return;
		}
		if (ProtectionContextDiffers(context)) {
			g_releaseRequested.store(true, std::memory_order_release);
		}
		if (!DrainRequestedRelease()) {
			return;
		}
		if (!Enabled()) {
			return;
		}

		const auto diagnosticEpoch =
			g_techniqueObservationEpoch.load(std::memory_order_acquire);
		const bool diagnosticsEnabled =
			g_diagnosticsEnabled.load(std::memory_order_acquire) &&
			diagnosticEpoch == g_techniqueObservationEpoch.load(std::memory_order_acquire);
		const auto technique65600CorrectionEpoch =
			g_technique65600CorrectionEpoch.load(std::memory_order_acquire);
		const bool technique65600CorrectionEnabled =
			g_technique65600CorrectionEnabled.load(std::memory_order_acquire) &&
			technique65600CorrectionEpoch ==
				g_technique65600CorrectionEpoch.load(std::memory_order_acquire);
		std::uint8_t packetDrawCount = 0;
		if (a_packet == 0 || !FaultGuard::SafeReadBytes(
				reinterpret_cast<const void*>(a_packet + 0x0c),
				&packetDrawCount, sizeof(packetDrawCount)) || packetDrawCount == 0) {
			return;
		}
		g_drawRoutes.fetch_add(1, std::memory_order_relaxed);
		g_preparedPacketDrawRoutes.fetch_add(1, std::memory_order_relaxed);

		const auto moduleBase = REL::Module::get().base();
		const auto callerRva = a_callerReturn >= moduleBase ? a_callerReturn - moduleBase : 0;
		if (callerRva != kAdmittedCallerReturnRva) {
			g_wrongCallerRoutes.fetch_add(1, std::memory_order_relaxed);
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			RecordFamilyDecline(a_token, Decline::kWrongCaller);
			return;
		}
		// Another module's unproven restore leaves the host state uncertain, and every
		// isolation Begin then refuses. That stops correction; it is not this draw's fault.
		if (D3D11StateIsolation::HostStateUncertain()) {
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			RecordFamilyDecline(a_token, Decline::kStateUncertain);
			return;
		}

		// The table census and this one preliminary PS identity read are observation-only.
		// Correction may establish protection for an admitted record. The ID 65600 observer
		// only enters that retained epoch; it never enables protection itself.
		EnsureCompositePixelTableSnapshot(context);
		auto preliminary = QueryPixelShaderIsolated(context);
		const auto preliminaryRecord = FindRegisteredTechnique(preliminary.pixelShader);
		float ratioWidth = 0.0f;
		float ratioHeight = 0.0f;
		const bool ratiosRead = DynamicResolution::CurrentRatios(ratioWidth, ratioHeight);
		const auto* const coverageContract = preliminaryRecord.pixelShaderRecord != 0 ?
			CompositeContracts::Find(preliminaryRecord.pixelTechnique) : nullptr;
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (a_token.generation == g_generation.load(std::memory_order_acquire)) {
				a_token.coverageIndex = coverageContract ?
					static_cast<std::uint32_t>(coverageContract - CompositeContracts::kRecords) :
					static_cast<std::uint32_t>(CompositeContracts::kRecordCount);
				auto& coverage = g_familyCoverage[a_token.coverageIndex];
				++coverage.observed;
				const auto ratioClass = ClassifyPreliminaryRatio(ratiosRead, ratioWidth, ratioHeight);
				if (ratioClass == PreliminaryRatioClass::kNative) {
					++coverage.native;
				} else if (ratioClass == PreliminaryRatioClass::kReduced) {
					++coverage.reduced;
				} else {
					++coverage.invalidRatio;
				}
			}
		}
		const bool cameraMatch = coverageContract &&
			coverageContract->kind == CompositeContracts::Kind::kCamera;
		const bool admittedMatch = preliminary.exceptionCode == 0 && coverageContract &&
			(IsAdmittedPixelTechnique(preliminaryRecord.pixelTechnique) ||
				(cameraMatch && technique65600CorrectionEnabled)) &&
			preliminaryRecord.pixelShaderRecord != 0 && preliminaryRecord.pixelShader;
		const bool technique65600Match = preliminary.exceptionCode == 0 &&
			preliminary.pixelShader &&
			preliminaryRecord.pixelTechnique == kObservedPixelTechnique &&
			preliminaryRecord.pixelShaderRecord != 0 &&
			preliminaryRecord.pixelShader == preliminary.pixelShader;
		if (diagnosticsEnabled) {
			const auto preliminaryDrawSerial = NextPreliminaryDrawSerial(a_token.generation);
			const auto ratioClass =
				ClassifyPreliminaryRatio(ratiosRead, ratioWidth, ratioHeight);
			if (admittedMatch) {
				RecordAdmittedPreliminaryMatch(
					preliminary, preliminaryRecord, ratioClass,
					a_token.generation, preliminaryDrawSerial);
			} else {
				RecordRejectedPixelShader(
					preliminary, preliminaryRecord, ratioClass,
					a_token.generation, preliminaryDrawSerial);
			}
			if (technique65600Match) {
				// ID 65600 remains outside cubemap correction admission. This bounded
				// path only snapshots state before the separate exact experiment below.
				if (!ObserveTechnique65600(
						context, preliminary, preliminaryRecord,
						a_callerReturn, callerRva, a_packet, packetDrawCount,
						ratiosRead, ratioWidth, ratioHeight,
						a_token.generation, diagnosticEpoch)) {
					SafeRelease(preliminary.pixelShader);
					a_token.callOriginal = false;
					return;
				}
			}
		}
		bool familyTableMatches = false;
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			familyTableMatches = g_familyTableMatches;
		}
		if (!familyTableMatches) {
			SafeRelease(preliminary.pixelShader);
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			RecordFamilyDecline(a_token, Decline::kTableMismatch);
			return;
		}
		if (technique65600CorrectionEnabled && technique65600Match) {
			BeginTechnique65600Correction(
				context, preliminary, preliminaryRecord,
				a_callerReturn, callerRva, a_packet, packetDrawCount,
				ratiosRead, ratioWidth, ratioHeight,
				a_token.generation, technique65600CorrectionEpoch, a_token);
			SafeRelease(preliminary.pixelShader);
			return;
		}
		SafeRelease(preliminary.pixelShader);
		if (!admittedMatch) {
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			RecordFamilyDecline(a_token,
				preliminary.exceptionCode != 0 ? Decline::kBindingsMissing :
					(cameraMatch ? Decline::kFamilyDisabled :
						(coverageContract ? Decline::kOutsideCameraContract : Decline::kWrongTechnique)),
				0, preliminary.exceptionCode);
			return;
		}

		if (!ratiosRead ||
			!std::isfinite(ratioWidth) || !std::isfinite(ratioHeight) ||
			ratioWidth <= 0.0f || ratioHeight <= 0.0f ||
			ratioWidth > 1.0f || ratioHeight > 1.0f ||
			(ratioWidth == 1.0f && ratioHeight == 1.0f)) {
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			RecordFamilyDecline(a_token,
				(ratioWidth == 1.0f && ratioHeight == 1.0f) ?
					Decline::kNativeRatio : Decline::kRatiosUnreadable);
			return;
		}
		g_lastRatioWidth.store(ratioWidth, std::memory_order_relaxed);
		g_lastRatioHeight.store(ratioHeight, std::memory_order_relaxed);
		g_correctionCandidates.fetch_add(1, std::memory_order_relaxed);

		std::uint32_t exceptionCode = 0;
		if (!AcquireMultithreadProtection(context, a_token, exceptionCode)) {
			RecordFamilyDecline(a_token, Decline::kProtectionFailed, E_NOINTERFACE, exceptionCode);
			g_exceptionCode.store(exceptionCode, std::memory_order_relaxed);
			if (exceptionCode != 0) {
				g_terminalStateUncertain.store(true, std::memory_order_release);
				g_ready.store(false, std::memory_order_relaxed);
				a_token.callOriginal = false;
			} else {
				g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			}
			return;
		}
		if (!Enabled() || a_token.generation != g_generation.load(std::memory_order_acquire) ||
			(cameraMatch && (!g_technique65600CorrectionEnabled.load(std::memory_order_acquire) ||
				technique65600CorrectionEpoch != g_technique65600CorrectionEpoch.load(std::memory_order_acquire)))) {
			a_token.coverageDecline = static_cast<std::uint32_t>(Decline::kStaleEpisode);
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		auto bound = QueryBoundStateIsolated(context);
		const auto selected = FindRegisteredTechnique(bound.pixelShader);
		bool registryDeviceMatches = false;
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			registryDeviceMatches = bound.device && bound.device == g_registryDevice;
		}
		const auto engineCamera = EngineCameraBuffer();
		const bool fullShaderIdentity = bound.exceptionCode == 0 && bound.device &&
			bound.pixelShader && coverageContract && admittedMatch &&
			selected.pixelTechnique == preliminaryRecord.pixelTechnique &&
			selected.pixelShaderRecord == preliminaryRecord.pixelShaderRecord &&
			selected.pixelShader == bound.pixelShader && registryDeviceMatches;
		const bool coreResourceGate = fullShaderIdentity &&
			bound.contextType == D3D11_DEVICE_CONTEXT_IMMEDIATE &&
			bound.multithreadInterface && bound.multithreadProtected &&
			(coverageContract->kind != CompositeContracts::Kind::kCube ||
				bound.t8ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY) &&
			bound.targetWidth > 0 && bound.targetHeight > 0 &&
			bound.cb12 && reinterpret_cast<std::uintptr_t>(bound.cb12) == engineCamera &&
			bound.rangeKnown && bound.firstConstant[2] == kExpectedFirstConstant &&
			bound.numConstants[2] == kExpectedNumConstants &&
			(bound.firstConstant[2] % 16) == 0 && (bound.numConstants[2] % 16) == 0 &&
			bound.numConstants[2] <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT &&
			bound.firstConstant[2] + kFirstReconstructionRow + kReconstructionRowCount <=
				kCameraBufferRows && BufferDescriptionMatchesCamera(bound.cb12) &&
			BufferRangeCoversShaderRows(bound.cb12, bound.firstConstant[2],
				bound.numConstants[2], coverageContract->cameraRows);
		const bool stereoBasisGate = coreResourceGate && bound.cb2 && bound.cb8 &&
			BufferRangeCoversShaderRows(
				bound.cb2, bound.firstConstant[0], bound.numConstants[0], coverageContract->perDrawRows) &&
			BufferRangeCoversEffectiveRow0(
				bound.cb8, bound.firstConstant[1], bound.numConstants[1]) &&
			ViewportBasisMatches(bound, ratioWidth, ratioHeight);
		if (!stereoBasisGate) {
			RecordFamilyDecline(a_token,
				bound.exceptionCode != 0 ? Decline::kBindingsMissing :
					(!fullShaderIdentity ? Decline::kWrongTechnique :
						(!coreResourceGate ? Decline::kBufferSize : Decline::kStereoBasisFailed)),
				E_INVALIDARG, bound.exceptionCode);
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			ReleaseBoundState(bound);
			return;
		}
		if (g_ownerThreadId != 0 &&
			g_ownerThreadId != static_cast<std::uint32_t>(::GetCurrentThreadId())) {
			RecordFamilyDecline(a_token, Decline::kUnsupportedContext, RPC_E_WRONG_THREAD);
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			ReleaseBoundState(bound);
			return;
		}

		std::int32_t resourceHr = S_OK;
		if (!EnsureCorrectionResources(bound.device, context, resourceHr)) {
			RecordFamilyDecline(a_token, Decline::kResourceBuildFailed, resourceHr);
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			ReleaseBoundState(bound);
			return;
		}
		a_token.coverageAdmitted = true;
		g_correctionAttempts.fetch_add(1, std::memory_order_relaxed);
		const auto preparation = PreparePrivateCamera(
			context, g_correctionContext1, bound.cb12, bound.cb2, bound.cb8,
			ratioWidth, ratioHeight, bound.viewports[0],
			bound.firstConstant, bound.numConstants, coverageContract->dimensionRow, true,
			bound.targetWidth, bound.targetHeight);
		if (preparation.stateUncertain) {
			a_token.coverageDecline = static_cast<std::uint32_t>(Decline::kStateUncertain);
			g_exceptionCode.store(preparation.exceptionCode, std::memory_order_relaxed);
			g_terminalStateUncertain.store(true, std::memory_order_release);
			g_ready.store(false, std::memory_order_relaxed);
			a_token.callOriginal = false;
			ReleaseBoundState(bound);
			return;
		}
		if (!preparation.began || !preparation.dispatched || !preparation.ended) {
			RecordFamilyDecline(a_token,
				Decline::kPrivatePreparationFailed,
				preparation.hr, preparation.exceptionCode);
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			ReleaseBoundState(bound);
			return;
		}

		CorrectionCapture capture{};
		bool captureWanted = false;
		if (diagnosticsEnabled) {
			const std::lock_guard<std::mutex> guard{ g_lock };
			captureWanted = g_correctionCaptureCount < kMaxCorrectionSamples;
		}
		if (captureWanted) {
			capture.serial = g_correctionCaptureCount + 1;
			capture.generation = a_token.generation;
			capture.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			capture.callerReturn = a_callerReturn;
			capture.callerReturnRva = callerRva;
			capture.packet = a_packet;
			capture.packetDrawCount = packetDrawCount;
			capture.pixelTechnique = selected.pixelTechnique;
			capture.pixelShaderRecord = selected.pixelShaderRecord;
			capture.boundPixelShader = reinterpret_cast<std::uintptr_t>(bound.pixelShader);
			capture.originalCamera = reinterpret_cast<std::uintptr_t>(bound.cb12);
			capture.privateCamera = reinterpret_cast<std::uintptr_t>(g_privateCamera);
			capture.firstConstant = bound.firstConstant[2];
			capture.numConstants = bound.numConstants[2];
			capture.ratioWidth = ratioWidth;
			capture.ratioHeight = ratioHeight;
			capture.viewportWidth = bound.viewports[0].Width;
			capture.viewportHeight = bound.viewports[0].Height;
			if (!ReadCorrectionEvidence(
					bound.device, context, bound.cb12, bound.cb2, bound.cb8,
					bound.firstConstant, bound.numConstants, capture)) {
				RecordFamilyDecline(a_token, Decline::kReadbackFailed, E_FAIL);
				g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
				ReleaseBoundState(bound);
				return;
			}
			capture.nativeWidth = static_cast<float>(bound.targetWidth);
			capture.nativeHeight = static_cast<float>(bound.targetHeight);
			capture.derivedRatioWidth = capture.viewportWidth / capture.nativeWidth;
			capture.derivedRatioHeight = capture.viewportHeight / capture.nativeHeight;
		}
		if (!Enabled() || a_token.generation != g_generation.load(std::memory_order_acquire) ||
			(cameraMatch && (!g_technique65600CorrectionEnabled.load(std::memory_order_acquire) ||
				technique65600CorrectionEpoch != g_technique65600CorrectionEpoch.load(std::memory_order_acquire)))) {
			a_token.coverageDecline = static_cast<std::uint32_t>(Decline::kStaleEpisode);
			g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			ReleaseBoundState(bound);
			return;
		}

		g_correctionContext1->AddRef();
		a_token.context1 = g_correctionContext1;
		a_token.originalCamera = bound.cb12;
		bound.cb12 = nullptr;
		a_token.firstConstant = bound.firstConstant[2];
		a_token.numConstants = bound.numConstants[2];
		const auto privateBinding = SetAndVerifyCameraBinding(
			a_token.context1, g_privateCamera,
			a_token.firstConstant, a_token.numConstants);
		if (!privateBinding.verified) {
			const auto handback = SetAndVerifyCameraBinding(
				a_token.context1, a_token.originalCamera,
				a_token.firstConstant, a_token.numConstants);
			RecordFamilyDecline(a_token,
				Decline::kPrivateBindFailed, E_FAIL,
				privateBinding.exceptionCode != 0 ?
					privateBinding.exceptionCode : handback.exceptionCode);
			if (!handback.verified) {
				g_originalRestoreFailures.fetch_add(1, std::memory_order_relaxed);
				g_terminalStateUncertain.store(true, std::memory_order_release);
				g_ready.store(false, std::memory_order_relaxed);
				a_token.callOriginal = false;
			} else {
				g_originalRestoreVerifications.fetch_add(1, std::memory_order_relaxed);
				g_correctionPassThroughs.fetch_add(1, std::memory_order_relaxed);
			}
			ReleaseBoundState(bound);
			return;
		}

		a_token.privateBound = true;
		a_token.correctionPrepared = true;
		if (captureWanted) {
			capture.privateBindingVerified = true;
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (Enabled() && a_token.generation == g_generation.load(std::memory_order_acquire) &&
				g_correctionCaptureCount < kMaxCorrectionSamples) {
				g_privateBindVerifications.fetch_add(1, std::memory_order_relaxed);
				g_ready.store(true, std::memory_order_relaxed);
				a_token.captureIndex = static_cast<std::uint32_t>(g_correctionCaptureCount);
				g_correctionCaptures[g_correctionCaptureCount++] = capture;
				g_lastCapturedCorrectedMask.store(capture.correctedMask, std::memory_order_relaxed);
				g_lastCapturedSkippedMask.store(capture.skippedMask, std::memory_order_relaxed);
				g_lastCapturedMaskKnown.store(true, std::memory_order_relaxed);
				if (!capture.completeReconstruction) {
					g_completeReconstruction.store(false, std::memory_order_relaxed);
				}
			}
		} else {
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (Enabled() && a_token.generation == g_generation.load(std::memory_order_acquire)) {
				g_privateBindVerifications.fetch_add(1, std::memory_order_relaxed);
				g_ready.store(true, std::memory_order_relaxed);
				g_maskKnownForAllCorrectedDraws.store(false, std::memory_order_relaxed);
				g_completeReconstruction.store(false, std::memory_order_relaxed);
			}
		}
		ReleaseBoundState(bound);
	}

	void EndPreparedPacketDraw(PreparedDrawToken& a_token) noexcept
	{
		std::uint32_t leaveException = 0;
		std::uint32_t finishException = 0;
		__try {
			__try {
				FinishPreparedPacketDrawState(a_token);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				finishException = static_cast<std::uint32_t>(::GetExceptionCode());
				if (a_token.technique65600Correction) {
					RecordTechnique65600TerminalFailure(
						a_token.technique65600CorrectionEpoch, a_token.generation,
						Technique65600CorrectionDecline::kStateUncertain,
						E_FAIL, finishException);
				} else {
					g_exceptionCode.store(finishException, std::memory_order_relaxed);
					g_terminalStateUncertain.store(true, std::memory_order_release);
					g_ready.store(false, std::memory_order_relaxed);
					RecordDecline(Decline::kStateUncertain, 0, finishException);
				}
			}
		} __finally {
			if (!LeaveMultithread(a_token, leaveException)) {
				if (a_token.technique65600Correction) {
					RecordTechnique65600TerminalFailure(
						a_token.technique65600CorrectionEpoch, a_token.generation,
						Technique65600CorrectionDecline::kStateUncertain,
						E_FAIL, leaveException);
				} else {
					g_exceptionCode.store(leaveException, std::memory_order_relaxed);
					g_terminalStateUncertain.store(true, std::memory_order_release);
					g_ready.store(false, std::memory_order_relaxed);
					RecordDecline(Decline::kStateUncertain, 0, leaveException);
				}
			}
		}
		FinishFamilyCoverage(a_token, finishException, leaveException);
		SafeRelease(a_token.originalCamera);
		SafeRelease(a_token.context1);
	}

	void SetDiagnosticsEnabled(bool a_enabled) noexcept
	{
		const std::lock_guard<std::mutex> guard{ g_lock };
		if (g_diagnosticsEnabled.load(std::memory_order_relaxed) == a_enabled) {
			return;
		}
		if (a_enabled) {
			ResetTechniqueObservationLocked();
			g_diagnosticsEnabled.store(true, std::memory_order_release);
		} else {
			g_diagnosticsEnabled.store(false, std::memory_order_release);
			g_techniqueObservationEpoch.fetch_add(1, std::memory_order_acq_rel);
		}
	}

	void SetTechnique65600CorrectionEnabled(bool a_enabled) noexcept
	{
		const std::lock_guard<std::mutex> guard{ g_lock };
		if (g_technique65600CorrectionEnabled.load(std::memory_order_relaxed) == a_enabled) {
			return;
		}
		if (a_enabled) {
			ResetTechnique65600CorrectionLocked();
			g_technique65600CorrectionEnabled.store(true, std::memory_order_release);
			logger::info(
				FMT_STRING("non-cubemap family correction armed; exact 65600 retains one CB2 dimension read and {} diagnostic captures"),
				kMaxTechnique65600CorrectionSamples);
		} else {
			g_technique65600CorrectionEnabled.store(false, std::memory_order_release);
			g_technique65600CorrectionEpoch.fetch_add(1, std::memory_order_acq_rel);
		}
	}

	void SetEnabled(bool a_enabled) noexcept
	{
		if (!a_enabled) {
			if (g_enabled.exchange(false, std::memory_order_acq_rel)) {
				g_generation.fetch_add(1, std::memory_order_acq_rel);
				g_releaseRequested.store(true, std::memory_order_release);
				g_ready.store(false, std::memory_order_relaxed);
			}
			return;
		}
		const std::lock_guard<std::mutex> guard{ g_lock };
		if (!g_enabled.load(std::memory_order_relaxed)) {
			if (g_registeredTechniqueCount != 0 ||
				g_generation.load(std::memory_order_relaxed) != 0) {
				g_releaseRequested.store(true, std::memory_order_release);
			}
			ResetLocked();
			g_generation.fetch_add(1, std::memory_order_acq_rel);
			g_enabled.store(true, std::memory_order_release);
			logger::info(
				FMT_STRING("composite correction armed for the audited installed-package DFComposite family; diagnostics={}; capture limit={} per correction episode"),
				g_diagnosticsEnabled.load(std::memory_order_acquire), kMaxCorrectionSamples);
		}
	}

	bool Enabled() noexcept
	{
		return g_enabled.load(std::memory_order_acquire);
	}

	void NoteDrawRoute(std::uintptr_t a_pass, std::uintptr_t a_shaderState) noexcept
	{
		if (!Enabled() || a_pass == 0) {
			return;
		}
		g_drawRoutes.fetch_add(1, std::memory_order_relaxed);
		void* engineShaderPtr = nullptr;
		void* engineVtablePtr = nullptr;
		if (!Engine::ReadPtr(a_pass + Addr::kRenderPassShaderOffset, engineShaderPtr) ||
			engineShaderPtr == nullptr ||
			!Engine::ReadPtr(reinterpret_cast<std::uintptr_t>(engineShaderPtr), engineVtablePtr) ||
			engineVtablePtr == nullptr) {
			return;
		}
		const REL::Relocation<std::uintptr_t> compositeVtable{
			REL::Offset(Addr::kCompositeShaderVtable)
		};
		if (reinterpret_cast<std::uintptr_t>(engineVtablePtr) != compositeVtable.address()) {
			return;
		}
		g_compositeDraws.fetch_add(1, std::memory_order_relaxed);

		RouteSample sample{};
		sample.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
		sample.pass = a_pass;
		sample.shaderState = a_shaderState;
		sample.engineShader = reinterpret_cast<std::uintptr_t>(engineShaderPtr);
		void* shapePtr = nullptr;
		void* passOverridePtr = nullptr;
		static_cast<void>(FaultGuard::SafeReadBytes(
			reinterpret_cast<const void*>(a_pass + Addr::kRenderPassTechniqueOffset),
			&sample.technique, sizeof(sample.technique)));
		const bool shapePointerReadable = FaultGuard::SafeReadBytes(
			reinterpret_cast<const void*>(a_pass + 0x18), &shapePtr, sizeof(shapePtr));
		static_cast<void>(FaultGuard::SafeReadBytes(
			reinterpret_cast<const void*>(a_pass + 0x20),
			&passOverridePtr, sizeof(passOverridePtr)));
		sample.shape = reinterpret_cast<std::uintptr_t>(shapePtr);
		sample.hasPassOverride = passOverridePtr != nullptr;
		if (shapePointerReadable && shapePtr != nullptr) {
			std::uint8_t geometryType = 0;
			sample.shapeReadable = FaultGuard::SafeReadBytes(
				reinterpret_cast<const void*>(
					reinterpret_cast<std::uintptr_t>(shapePtr) + 0x198),
				&geometryType, sizeof(geometryType));
			sample.geometryType = geometryType;
		}
		void* recordPtr = nullptr;
		void* pixelShaderPtr = nullptr;
		if (a_shaderState != 0 &&
			Engine::ReadPtr(a_shaderState + Addr::kPreparedShaderPixelRecordOffset, recordPtr) &&
			recordPtr != nullptr) {
			static_cast<void>(Engine::ReadPtr(
				reinterpret_cast<std::uintptr_t>(recordPtr) + Addr::kShaderRecordD3DObjectOffset,
				pixelShaderPtr));
		}
		sample.pixelShaderRecord = reinterpret_cast<std::uintptr_t>(recordPtr);
		sample.expectedPixelShader = reinterpret_cast<std::uintptr_t>(pixelShaderPtr);
		static_cast<void>(DynamicResolution::CurrentRatios(sample.ratioWidth, sample.ratioHeight));
		sample.count = 1;

		const std::lock_guard<std::mutex> guard{ g_lock };
		for (std::size_t i = 0; i < g_routeSampleCount; ++i) {
			auto& present = g_routeSamples[i];
			if (present.technique == sample.technique &&
				present.geometryType == sample.geometryType &&
				present.shapeReadable == sample.shapeReadable &&
				present.hasPassOverride == sample.hasPassOverride &&
				present.pixelShaderRecord == sample.pixelShaderRecord) {
				++present.count;
				present.ratioWidth = sample.ratioWidth;
				present.ratioHeight = sample.ratioHeight;
				return;
			}
		}
		if (g_routeSampleCount < kMaxRouteSamples) {
			g_routeSamples[g_routeSampleCount++] = sample;
		} else {
			++g_routeOverflow;
		}
	}

	void ObservePreparedPacketDraw(
		std::uintptr_t a_callerReturn,
		std::uintptr_t a_packet) noexcept
	{
		if (!Enabled()) {
			return;
		}
		std::uint8_t packetDrawCount = 0;
		if (a_packet == 0 || !FaultGuard::SafeReadBytes(
				reinterpret_cast<const void*>(a_packet + 0x0c),
				&packetDrawCount, sizeof(packetDrawCount)) || packetDrawCount == 0) {
			return;
		}
		g_drawRoutes.fetch_add(1, std::memory_order_relaxed);
		g_preparedPacketDrawRoutes.fetch_add(1, std::memory_order_relaxed);

		auto* const context = Engine::ImmediateContext();
		if (!context) {
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (Enabled()) {
				RecordDecline(Decline::kNoContext);
			}
			return;
		}
		EnsureCompositePixelTableSnapshot(context);
		{
			const std::lock_guard<std::mutex> guard{ g_lock };
			if (!g_tableSnapshotComplete) {
				return;
			}
		}

		auto query = QueryPixelShaderIsolated(context);

		const auto moduleBase = REL::Module::get().base();
		const auto callerRva = a_callerReturn >= moduleBase ?
			a_callerReturn - moduleBase : 0;
		const std::lock_guard<std::mutex> guard{ g_lock };
		if (!Enabled()) {
			SafeRelease(query.pixelShader);
			return;
		}
		if (query.exceptionCode != 0) {
			RecordDecline(Decline::kBindingsMissing, 0, query.exceptionCode);
			SafeRelease(query.pixelShader);
			return;
		}

		RegisteredTechnique* registered = nullptr;
		for (std::size_t i = 0; i < g_registeredTechniqueCount; ++i) {
			if (g_registeredTechniques[i].pixelShader == query.pixelShader) {
				registered = &g_registeredTechniques[i];
				break;
			}
		}
		if (!registered) {
			const auto liveIdentity = reinterpret_cast<std::uintptr_t>(query.pixelShader);
			for (std::size_t i = 0; i < g_unknownPostCommitCount; ++i) {
				auto& present = g_unknownPostCommits[i];
				if (present.callerReturn == a_callerReturn &&
					present.boundPixelShader == liveIdentity &&
					present.threadId == static_cast<std::uint32_t>(::GetCurrentThreadId())) {
					if (present.count != (std::numeric_limits<std::uint64_t>::max)()) {
						++present.count;
					}
					SafeRelease(query.pixelShader);
					return;
				}
			}
			if (g_unknownPostCommitCount < kMaxUnknownPostCommits) {
				auto& added = g_unknownPostCommits[g_unknownPostCommitCount++];
				added.callerReturn = a_callerReturn;
				added.callerReturnRva = callerRva;
				added.boundPixelShader = liveIdentity;
				added.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
				added.count = 1;
			} else if (g_unknownPostCommitOverflow != (std::numeric_limits<std::uint32_t>::max)()) {
				++g_unknownPostCommitOverflow;
			}
			SafeRelease(query.pixelShader);
			return;
		}

		const RegisteredTechnique selected = *registered;
		SafeRelease(query.pixelShader);
		g_compositeDraws.fetch_add(1, std::memory_order_relaxed);
		if (g_sampleCount >= kMaxSamples ||
			g_captureFailures >= kMaxCaptureFailures) {
			return;
		}

		float ratioWidth = 0.0f;
		float ratioHeight = 0.0f;
		if (!DynamicResolution::CurrentRatios(ratioWidth, ratioHeight) ||
			!std::isfinite(ratioWidth) || !std::isfinite(ratioHeight) ||
			ratioWidth <= 0.0f || ratioHeight <= 0.0f) {
			RecordDecline(Decline::kRatiosUnreadable);
			return;
		}
		g_lastRatioWidth.store(ratioWidth, std::memory_order_relaxed);
		g_lastRatioHeight.store(ratioHeight, std::memory_order_relaxed);
		const bool reduced = ratioWidth != 1.0f || ratioHeight != 1.0f;
		const auto bucketCount = reduced ? g_reducedSamples : g_nativeSamples;
		if (bucketCount >= kSamplesPerRatioClass) {
			return;
		}
		auto bound = QueryBoundStateIsolated(context);
		if (bound.exceptionCode != 0) {
			RecordCaptureFailure(Decline::kReadbackFailed, 0, bound.exceptionCode);
			ReleaseBoundState(bound);
			return;
		}
		if (!bound.device || !bound.pixelShader || bound.device != g_registryDevice) {
			RecordCaptureFailure(Decline::kBindingsMissing);
			ReleaseBoundState(bound);
			return;
		}
		if (bound.t8ViewDimension != D3D11_SRV_DIMENSION_TEXTURECUBEARRAY) {
			if (g_knownNonCubeMatches != (std::numeric_limits<std::uint64_t>::max)()) {
				++g_knownNonCubeMatches;
			}
			const auto boundIdentity = reinterpret_cast<std::uintptr_t>(bound.pixelShader);
			const auto threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
			for (std::size_t i = 0; i < g_knownNonCubeSampleCount; ++i) {
				auto& present = g_knownNonCubeSamples[i];
				if (present.callerReturn == a_callerReturn &&
					present.pixelShaderRecord == selected.pixelShaderRecord &&
					present.boundPixelShader == boundIdentity &&
					present.t8ViewDimension == bound.t8ViewDimension &&
					present.threadId == threadId) {
					if (present.count != (std::numeric_limits<std::uint64_t>::max)()) {
						++present.count;
					}
					ReleaseBoundState(bound);
					return;
				}
			}
			if (g_knownNonCubeSampleCount < kMaxKnownNonCubeSamples) {
				auto& added = g_knownNonCubeSamples[g_knownNonCubeSampleCount++];
				added.callerReturn = a_callerReturn;
				added.callerReturnRva = callerRva;
				added.pixelShaderRecord = selected.pixelShaderRecord;
				added.boundPixelShader = boundIdentity;
				added.vertexTechnique = selected.vertexTechnique;
				added.pixelTechnique = selected.pixelTechnique;
				added.t8ViewDimension = bound.t8ViewDimension;
				added.threadId = threadId;
				added.count = 1;
			} else if (g_knownNonCubeOverflow != (std::numeric_limits<std::uint32_t>::max)()) {
				++g_knownNonCubeOverflow;
			}
			ReleaseBoundState(bound);
			return;
		}
		if (!bound.cb2 || !bound.cb8 || !bound.cb12) {
			RecordCaptureFailure(Decline::kBindingsMissing);
			ReleaseBoundState(bound);
			return;
		}
		g_attempts.fetch_add(1, std::memory_order_relaxed);

		Sample sample{};
		sample.serial = g_captures.load(std::memory_order_relaxed) + 1;
		sample.threadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
		sample.reduced = reduced;
		sample.callerReturn = a_callerReturn;
		sample.callerReturnRva = callerRva;
		sample.packet = a_packet;
		sample.packetDrawCount = packetDrawCount;
		sample.shaderState = selected.shaderState;
		sample.engineShader = selected.engineShader;
		const REL::Relocation<std::uintptr_t> compositeVtable{
			REL::Offset(Addr::kCompositeShaderVtable)
		};
		sample.engineShaderVtable = compositeVtable.address();
		sample.technique = selected.pixelTechnique;
		sample.vertexTechnique = selected.vertexTechnique;
		sample.pixelTechnique = selected.pixelTechnique;
		sample.pixelShaderRecord = selected.pixelShaderRecord;
		sample.expectedPixelShader =
			reinterpret_cast<std::uintptr_t>(selected.pixelShader);
		sample.boundPixelShader = reinterpret_cast<std::uintptr_t>(bound.pixelShader);
		sample.pixelShaderMatchesRecord =
			sample.expectedPixelShader == sample.boundPixelShader;
		sample.context = reinterpret_cast<std::uintptr_t>(context);
		sample.contextType = bound.contextType;
		sample.multithreadInterface = bound.multithreadInterface;
		sample.multithreadProtected = bound.multithreadProtected;
		sample.registryDeviceMatches = bound.device == g_registryDevice;
		sample.engineCameraBuffer = EngineCameraBuffer();
		sample.cb12MatchesEngine =
			sample.engineCameraBuffer == reinterpret_cast<std::uintptr_t>(bound.cb12);
		sample.t8ViewDimension = bound.t8ViewDimension;
		sample.ratioWidth = ratioWidth;
		sample.ratioHeight = ratioHeight;
		sample.viewportCount = std::min<std::uint32_t>(
			bound.viewportCount,
			D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
		std::memcpy(
			sample.viewports, bound.viewports,
			sample.viewportCount * sizeof(D3D11_VIEWPORT));

		BufferCapture* captures[3] = { &sample.cb2, &sample.cb8, &sample.cb12 };
		ID3D11Buffer* buffers[3] = { bound.cb2, bound.cb8, bound.cb12 };
		ReadbackResult readbacks[3]{};
		bool readbackOk = true;
		for (std::size_t i = 0; i < 3; ++i) {
			captures[i]->rangeKnown = bound.rangeKnown;
			captures[i]->firstConstant = bound.firstConstant[i];
			captures[i]->numConstants = bound.numConstants[i];
			readbacks[i] = ReadBufferIsolated(
				bound.device, context, buffers[i], *captures[i]);
			if (!readbacks[i].copied || readbacks[i].exceptionCode != 0) {
				readbackOk = false;
			}
		}
		ReleaseBoundState(bound);

		if (!readbackOk) {
			for (const auto& readback : readbacks) {
				if (!readback.copied || readback.exceptionCode != 0) {
					RecordCaptureFailure(
						Decline::kReadbackFailed, readback.hr, readback.exceptionCode);
					break;
				}
			}
			return;
		}
		const bool cb2RangeValid = EffectiveRangeCovers(sample.cb2, 0, 1);
		const bool cb8RangeValid = EffectiveRangeCovers(sample.cb8, 0, 1);
		const bool cb12RangeValid = EffectiveRangeCovers(
			sample.cb12, kFirstReconstructionRow, kReconstructionRowCount);
		if (!cb2RangeValid || !cb8RangeValid || !cb12RangeValid) {
			RecordCaptureFailure(Decline::kBufferSize, E_INVALIDARG);
			return;
		}

		g_bufferBytes.store(sample.cb12.byteWidth, std::memory_order_relaxed);
		NotePixelShader(sample.boundPixelShader);
		g_samples[g_sampleCount++] = sample;
		if (reduced) {
			++g_reducedSamples;
		} else {
			++g_nativeSamples;
		}
		g_captures.fetch_add(1, std::memory_order_relaxed);
		const bool sampleValid =
			sample.pixelShaderMatchesRecord &&
			sample.registryDeviceMatches &&
			sample.cb12MatchesEngine &&
			cb2RangeValid && cb8RangeValid && cb12RangeValid;
		g_allSamplesValid = g_allSamplesValid && sampleValid;
		// Stage A never publishes production readiness; the operator still admits the
		// exact record/layout/caller evidence before any corrective build exists.
		g_ready.store(false, std::memory_order_relaxed);
		logger::info(
			FMT_STRING("composite prepared-packet pre-draw observation {}: {} ratios {:.6f} x {:.6f}, caller RVA {:X}, packet draws {}, pixel shader {:016X} matches family record {}, registry device {}, CB12 {:016X} matches engine {}, bytes {}, range known {} first {} count {}, viewports {}"),
			sample.serial,
			reduced ? "reduced"sv : "native"sv,
			ratioWidth,
			ratioHeight,
			sample.callerReturnRva,
			sample.packetDrawCount,
			sample.boundPixelShader,
			sample.pixelShaderMatchesRecord ? "yes"sv : "no"sv,
			sample.registryDeviceMatches ? "yes"sv : "no"sv,
			sample.cb12.identity,
			sample.cb12MatchesEngine ? "yes"sv : "no"sv,
			sample.cb12.byteWidth,
			sample.cb12.rangeKnown ? "yes"sv : "no"sv,
			sample.cb12.firstConstant,
			sample.cb12.numConstants,
			sample.viewportCount);

		if (g_nativeSamples == kSamplesPerRatioClass && !g_nativeCompleteLogged) {
			g_nativeCompleteLogged = true;
			logger::info("composite prepared-packet observation: native sample bucket complete"sv);
		}
		if (g_reducedSamples == kSamplesPerRatioClass && !g_reducedCompleteLogged) {
			g_reducedCompleteLogged = true;
			logger::info("composite prepared-packet observation: reduced sample bucket complete; readiness remains false pending operator admission"sv);
		}
	}

	void Release() noexcept
	{
		g_enabled.store(false, std::memory_order_release);
		g_ready.store(false, std::memory_order_relaxed);
		g_generation.fetch_add(1, std::memory_order_acq_rel);
		g_releaseRequested.store(true, std::memory_order_release);
	}

	void RequestRelease() noexcept
	{
		g_releaseRequested.store(true, std::memory_order_release);
	}

	State Snapshot() noexcept
	{
		State state{};
		state.diagnosticsEnabled = g_diagnosticsEnabled.load(std::memory_order_acquire);
		state.attempts = g_attempts.load(std::memory_order_relaxed);
		state.applied = g_captures.load(std::memory_order_relaxed);
		state.restores = g_restores.load(std::memory_order_relaxed);
		state.declines = g_declines.load(std::memory_order_relaxed);
		state.lastDecline = g_lastDecline.load(std::memory_order_relaxed);
		state.exceptionCode = g_exceptionCode.load(std::memory_order_relaxed);
		state.lastHr = g_lastHr.load(std::memory_order_relaxed);
		state.lastRatioWidth = g_lastRatioWidth.load(std::memory_order_relaxed);
		state.lastRatioHeight = g_lastRatioHeight.load(std::memory_order_relaxed);
		state.bufferBytes = g_bufferBytes.load(std::memory_order_relaxed);
		state.ready = g_ready.load(std::memory_order_relaxed);
		state.drawsExamined = g_drawRoutes.load(std::memory_order_relaxed);
		state.compositeDraws = g_compositeDraws.load(std::memory_order_relaxed);
		state.compositeShaders = g_compositeShaders.load(std::memory_order_relaxed);
		const std::lock_guard<std::mutex> guard{ g_lock };
		state.captureFailures = g_captureFailures;
		state.nativeSamples = static_cast<std::uint32_t>(g_nativeSamples);
		state.reducedSamples = static_cast<std::uint32_t>(g_reducedSamples);
		state.captureCapped = g_captureFailures >= kMaxCaptureFailures;
		state.routeOverflow = g_routeOverflow;
		state.preparedPacketDrawRoutes = g_preparedPacketDrawRoutes.load(std::memory_order_relaxed);
		state.registryRecordCount = static_cast<std::uint32_t>(g_registeredTechniqueCount);
		state.registryOverflow = g_registryOverflow;
		state.unknownPreparedPacketOverflow = g_unknownPostCommitOverflow;
		state.knownNonCubeMatches = g_knownNonCubeMatches;
		state.knownNonCubeOverflow = g_knownNonCubeOverflow;
		state.tableSnapshotCapacity = g_tableSnapshotCapacity;
		state.tableSnapshotRecords = g_tableSnapshotRecords;
		state.tableSnapshotFailures = g_tableSnapshotFailures;
		state.tableSnapshotComplete = g_tableSnapshotComplete;
		state.correctionCandidates = g_correctionCandidates.load(std::memory_order_relaxed);
		state.correctionAttempts = g_correctionAttempts.load(std::memory_order_relaxed);
		state.correctedDraws = g_correctedDraws.load(std::memory_order_relaxed);
		state.correctionPassThroughs = g_correctionPassThroughs.load(std::memory_order_relaxed);
		state.privateBindVerifications = g_privateBindVerifications.load(std::memory_order_relaxed);
		state.originalRestoreVerifications = g_originalRestoreVerifications.load(std::memory_order_relaxed);
		state.originalRestoreFailures = g_originalRestoreFailures.load(std::memory_order_relaxed);
		state.resourceBuilds = g_resourceBuilds.load(std::memory_order_relaxed);
		state.resourceReleases = g_resourceReleases.load(std::memory_order_relaxed);
		state.resourceEpoch = g_resourceEpoch.load(std::memory_order_relaxed);
		state.protectionAttempts = g_protectionAttempts.load(std::memory_order_relaxed);
		state.protectionEnables = g_protectionEnables.load(std::memory_order_relaxed);
		state.protectionEnters = g_protectionEnters.load(std::memory_order_relaxed);
		state.protectionLeaves = g_protectionLeaves.load(std::memory_order_relaxed);
		state.correctionCaptureCount = static_cast<std::uint32_t>(g_correctionCaptureCount);
		state.lastCapturedCorrectedMask = g_lastCapturedCorrectedMask.load(std::memory_order_relaxed);
		state.lastCapturedSkippedMask = g_lastCapturedSkippedMask.load(std::memory_order_relaxed);
		state.lastCapturedMaskKnown = g_lastCapturedMaskKnown.load(std::memory_order_relaxed);
		state.maskKnownForAllCorrectedDraws = g_maskKnownForAllCorrectedDraws.load(std::memory_order_relaxed);
		state.completeReconstruction = g_completeReconstruction.load(std::memory_order_relaxed) &&
			state.maskKnownForAllCorrectedDraws && state.correctedDraws != 0;
		state.multithreadPriorProtected =
			g_multithreadPriorProtected.load(std::memory_order_relaxed);
		state.multithreadEnabledByModule =
			g_multithreadEnabledByModule.load(std::memory_order_relaxed);
		state.multithreadProtected = g_multithreadProtected.load(std::memory_order_acquire);
		state.globalProtectionCostActive = state.multithreadProtected;
		state.releasePending = g_releaseRequested.load(std::memory_order_relaxed);
		state.terminalStateUncertain = g_terminalStateUncertain.load(std::memory_order_relaxed);
		state.technique65600DiagnosticEpoch =
			g_techniqueObservationEpoch.load(std::memory_order_relaxed);
		state.technique65600EligibleDraws =
			g_techniqueObservationEligibleDraws.load(std::memory_order_relaxed);
		state.technique65600EligibilityDeclines =
			g_techniqueObservationEligibilityDeclines.load(std::memory_order_relaxed);
		state.technique65600Attempts =
			g_techniqueObservationAttempts.load(std::memory_order_relaxed);
		state.technique65600Successes =
			g_techniqueObservationSuccesses.load(std::memory_order_relaxed);
		state.technique65600Failures =
			g_techniqueObservationFailures.load(std::memory_order_relaxed);
		state.technique65600LastFailure =
			g_techniqueObservationLastFailure.load(std::memory_order_relaxed);
		state.technique65600LastHr =
			g_techniqueObservationLastHr.load(std::memory_order_relaxed);
		state.technique65600ExceptionCode =
			g_techniqueObservationExceptionCode.load(std::memory_order_relaxed);
		state.technique65600CaptureCount =
			static_cast<std::uint32_t>(g_techniqueObservationCaptureCount);
		state.technique65600AttemptCapped =
			state.technique65600Attempts >= kMaxTechniqueObservationSamples;
		state.technique65600CaptureCapped =
			state.technique65600Successes >= kMaxTechniqueObservationSamples;
		state.technique65600CorrectionEnabled =
			g_technique65600CorrectionEnabled.load(std::memory_order_acquire);
		state.technique65600CorrectionEpoch =
			g_technique65600CorrectionEpoch.load(std::memory_order_relaxed);
		state.technique65600CorrectionCandidates =
			g_technique65600CorrectionCandidates.load(std::memory_order_relaxed);
		state.technique65600CorrectionAttempts =
			g_technique65600CorrectionAttempts.load(std::memory_order_relaxed);
		state.technique65600PrivateDraws =
			g_technique65600PrivateDraws.load(std::memory_order_relaxed);
		state.technique65600CorrectedDraws =
			g_technique65600CorrectedDraws.load(std::memory_order_relaxed);
		state.technique65600CorrectionPassThroughs =
			g_technique65600CorrectionPassThroughs.load(std::memory_order_relaxed);
		state.technique65600CorrectionDeclines =
			g_technique65600CorrectionDeclines.load(std::memory_order_relaxed);
		state.technique65600CorrectionLastDecline =
			g_technique65600CorrectionLastDecline.load(std::memory_order_relaxed);
		state.technique65600CorrectionLastHr =
			g_technique65600CorrectionLastHr.load(std::memory_order_relaxed);
		state.technique65600CorrectionExceptionCode =
			g_technique65600CorrectionExceptionCode.load(std::memory_order_relaxed);
		state.technique65600PrivateBindVerifications =
			g_technique65600PrivateBindVerifications.load(std::memory_order_relaxed);
		state.technique65600OriginalRestoreVerifications =
			g_technique65600OriginalRestoreVerifications.load(std::memory_order_relaxed);
		state.technique65600OriginalRestoreFailures =
			g_technique65600OriginalRestoreFailures.load(std::memory_order_relaxed);
		state.technique65600DimensionReadAttempts =
			g_technique65600DimensionReadAttempts.load(std::memory_order_relaxed);
		state.technique65600DimensionReadFailures =
			g_technique65600DimensionReadFailures.load(std::memory_order_relaxed);
		state.technique65600DimensionCacheValid = g_technique65600DimensionCache.valid;
		state.technique65600NativeWidth = g_technique65600DimensionCache.width;
		state.technique65600NativeHeight = g_technique65600DimensionCache.height;
		state.technique65600CorrectionCaptureAttempts =
			g_technique65600CorrectionCaptureAttempts.load(std::memory_order_relaxed);
		state.technique65600CorrectionCaptureCount =
			static_cast<std::uint32_t>(g_technique65600CorrectionCaptureCount);
		state.technique65600CorrectionCaptureCapped =
			g_technique65600CorrectionCaptureReservations >=
				kMaxTechnique65600CorrectionSamples;
		state.technique65600LastCorrectedMask =
			g_technique65600LastCorrectedMask.load(std::memory_order_relaxed);
		state.technique65600LastSkippedMask =
			g_technique65600LastSkippedMask.load(std::memory_order_relaxed);
		state.technique65600LastMaskKnown =
			g_technique65600LastMaskKnown.load(std::memory_order_relaxed);
		state.technique65600ExpectedPartialReconstruction =
			g_technique65600ExpectedPartialReconstruction.load(std::memory_order_relaxed) &&
			state.technique65600CorrectionCaptureCount != 0;
		state.technique65600MaskKnownForAllPrivateDraws =
			g_technique65600MaskKnownForAllPrivateDraws.load(std::memory_order_relaxed) &&
			state.technique65600PrivateDraws != 0;
		state.registryRecordsJson = RegistryRecordsJsonLocked();
		state.unknownPreparedPacketsJson = UnknownPostCommitsJsonLocked();
		state.knownNonCubeJson = KnownNonCubeJsonLocked();
		state.routeObservationsJson = RouteObservationsJsonLocked();
		state.observationsJson = ObservationsJsonLocked();
		state.correctionObservationsJson = CorrectionObservationsJsonLocked();
		state.technique65600ObservationsJson = Technique65600ObservationsJsonLocked();
		state.technique65600CorrectionObservationsJson =
			Technique65600CorrectionObservationsJsonLocked();
		state.preliminaryCensusJson = PreliminaryCensusJsonLocked();
		state.familyCoverageJson = FamilyCoverageJsonLocked();
		return state;
	}
}
