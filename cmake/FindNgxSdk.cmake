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
	set(UPSCALER_NGX_INCLUDE_DIR "" CACHE PATH "" FORCE)
	set(UPSCALER_NGX_LIBRARY "" CACHE FILEPATH "" FORCE)
	set(UPSCALER_NGX_TEST_RUNTIME "" CACHE FILEPATH "" FORCE)
	message(STATUS "NGX backend disabled (UPSCALER_ENABLE_NGX=OFF).")
	return()
endif()


set(_UPSCALER_NGX_SEARCH_ROOTS
	${CMAKE_CURRENT_SOURCE_DIR}/.sdk-cache/streamline
	${CMAKE_CURRENT_SOURCE_DIR}/.sdk-cache/ngx
)
if(UPSCALER_NGX_SDK_DIR)
	list(PREPEND _UPSCALER_NGX_SEARCH_ROOTS ${UPSCALER_NGX_SDK_DIR})
endif()

find_path(UPSCALER_NGX_INCLUDE_DIR
	nvsdk_ngx.h
	PATHS ${_UPSCALER_NGX_SEARCH_ROOTS}
	PATH_SUFFIXES "" include
	NO_DEFAULT_PATH
)
find_library(UPSCALER_NGX_LIBRARY
	NAMES nvsdk_ngx_d nvsdk_ngx_s
	PATHS ${_UPSCALER_NGX_SEARCH_ROOTS}
	PATH_SUFFIXES "" lib/x64 lib/Windows_x86_64
	NO_DEFAULT_PATH
)
find_file(UPSCALER_NGX_TEST_RUNTIME
	nvngx_dlss.dll
	PATHS ${_UPSCALER_NGX_SEARCH_ROOTS}
	PATH_SUFFIXES "" bin/x64
	NO_DEFAULT_PATH
)

if(UPSCALER_NGX_INCLUDE_DIR AND UPSCALER_NGX_LIBRARY)
	set(UPSCALER_NGX_SDK_DIR "${UPSCALER_NGX_INCLUDE_DIR}" CACHE PATH "Root of a staged NVIDIA NGX SDK." FORCE)
	message(STATUS "NGX SDK found: headers ${UPSCALER_NGX_INCLUDE_DIR}, library ${UPSCALER_NGX_LIBRARY}.")
else()
	set(UPSCALER_NGX_SDK_DIR "" CACHE PATH "Root of a staged NVIDIA NGX SDK." FORCE)
	message(STATUS "No complete NGX SDK staged; both headers and the x64 library are required.")
endif()
