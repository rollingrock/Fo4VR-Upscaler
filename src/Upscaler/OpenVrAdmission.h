#pragma once

namespace Upscaler::OpenVrAdmission
{
	enum class Result
	{
		kAvailable,
		kMissingExports,
		kUnreliableValidation,
		kUnsupported,
		kLookupFailed,
	};

	// Some translation loaders accept every version in their validator, then abort
	// on an unsupported lookup. Refuse those loaders before requesting any interface.
	template <class Validator, class Lookup, class Error>
	[[nodiscard]] void* Query(Validator a_validate, Lookup a_lookup, const char* a_version,
		Error* a_error, Result& a_result) noexcept
	{
		a_result = Result::kMissingExports;
		if (!a_validate || !a_lookup) {
			return nullptr;
		}
		a_result = Result::kUnreliableValidation;
		if (a_validate("Fo4VRUpscaler_InvalidInterface_000")) {
			return nullptr;
		}
		a_result = Result::kUnsupported;
		if (!a_validate(a_version)) {
			return nullptr;
		}
		auto* resolved = a_lookup(a_version, a_error);
		a_result = resolved ? Result::kAvailable : Result::kLookupFailed;
		return resolved;
	}
}
