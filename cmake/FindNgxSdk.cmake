# Locates a staged NVIDIA NGX SDK. Finding nothing is the normal case and is not an
# error: the tree must configure and build green on a machine with no vendor SDK, which
# is what the CI job asserts. This module only finds - it defines nothing on a target;
# the caller maps UPSCALER_NGX_SDK_DIR onto UPSCALER_NO_NGX.
#
# The SDK is never committed. scripts/fetch-sdks.ps1 stages it under .sdk-cache/, which
# is gitignored, and the licence disposition lives in THIRD_PARTY.md.

set(UPSCALER_NGX_SDK_DIR "" CACHE PATH "Root of a staged NVIDIA NGX SDK.")

if(NOT UPSCALER_ENABLE_NGX)
	set(UPSCALER_NGX_SDK_DIR "" CACHE PATH "" FORCE)
	message(STATUS "NGX backend disabled (UPSCALER_ENABLE_NGX=OFF).")
	return()
endif()

if(NOT UPSCALER_NGX_SDK_DIR)
	find_path(UPSCALER_NGX_SDK_DIR
		include/nvsdk_ngx.h
		PATHS
		${CMAKE_CURRENT_SOURCE_DIR}/.sdk-cache/ngx
		NO_DEFAULT_PATH
	)
endif()

if(UPSCALER_NGX_SDK_DIR)
	set(UPSCALER_NGX_INCLUDE_DIR "${UPSCALER_NGX_SDK_DIR}/include")
	find_library(UPSCALER_NGX_LIBRARY
		NAMES nvsdk_ngx_s
		PATHS ${UPSCALER_NGX_SDK_DIR}/lib/x64
		NO_DEFAULT_PATH
	)
	message(STATUS "NGX SDK found at ${UPSCALER_NGX_SDK_DIR}.")
else()
	message(STATUS "No NGX SDK staged; the NGX backend will report itself unavailable.")
endif()
