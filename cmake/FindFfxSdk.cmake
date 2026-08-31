# Locates and verifies the selected MIT FidelityFX DirectX 11 source fork. The
# dependency stays outside this repository and is opt-in; finding nothing while the
# backend is disabled is the ordinary SDK-free build.

set(UPSCALER_FFX_SDK_DIR "" CACHE PATH "Root of alandtse/FidelityFX-SDK-DX11 at the pinned commit.")
set(UPSCALER_FFX_EXPECTED_COMMIT "e65b2530631f2afb9a9ac753884926e49e20d608")

if(NOT UPSCALER_ENABLE_FFX)
	message(STATUS "FidelityFX backend disabled (UPSCALER_ENABLE_FFX=OFF).")
	return()
endif()

if(NOT UPSCALER_FFX_SDK_DIR)
	message(FATAL_ERROR
		"UPSCALER_ENABLE_FFX=ON requires -DUPSCALER_FFX_SDK_DIR=<external alandtse/FidelityFX-SDK-DX11 clone>.")
endif()

cmake_path(ABSOLUTE_PATH UPSCALER_FFX_SDK_DIR NORMALIZE OUTPUT_VARIABLE _UPSCALER_FFX_ROOT)
if(NOT EXISTS "${_UPSCALER_FFX_ROOT}/sdk/include/FidelityFX/host/backends/dx11/ffx_dx11.h" OR
	NOT EXISTS "${_UPSCALER_FFX_ROOT}/sdk/include/FidelityFX/host/ffx_fsr3upscaler.h" OR
	NOT EXISTS "${_UPSCALER_FFX_ROOT}/LICENSE.txt")
	message(FATAL_ERROR "UPSCALER_FFX_SDK_DIR is not the selected FidelityFX-SDK-DX11 source tree: ${_UPSCALER_FFX_ROOT}")
endif()

find_package(Git REQUIRED)
execute_process(
	COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${_UPSCALER_FFX_ROOT}" -C "${_UPSCALER_FFX_ROOT}" rev-parse HEAD
	RESULT_VARIABLE _UPSCALER_FFX_GIT_RESULT
	OUTPUT_VARIABLE _UPSCALER_FFX_COMMIT
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_VARIABLE _UPSCALER_FFX_GIT_ERROR
)
if(NOT _UPSCALER_FFX_GIT_RESULT EQUAL 0)
	message(FATAL_ERROR "Cannot verify the FidelityFX source commit: ${_UPSCALER_FFX_GIT_ERROR}")
endif()
if(NOT _UPSCALER_FFX_COMMIT STREQUAL UPSCALER_FFX_EXPECTED_COMMIT)
	message(FATAL_ERROR
		"FidelityFX source is ${_UPSCALER_FFX_COMMIT}; this project pins ${UPSCALER_FFX_EXPECTED_COMMIT}.")
endif()

execute_process(
	COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${_UPSCALER_FFX_ROOT}" -C "${_UPSCALER_FFX_ROOT}" status --porcelain --untracked-files=no
	RESULT_VARIABLE _UPSCALER_FFX_STATUS_RESULT
	OUTPUT_VARIABLE _UPSCALER_FFX_STATUS
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_VARIABLE _UPSCALER_FFX_STATUS_ERROR
)
if(NOT _UPSCALER_FFX_STATUS_RESULT EQUAL 0)
	message(FATAL_ERROR "Cannot verify the FidelityFX source worktree: ${_UPSCALER_FFX_STATUS_ERROR}")
endif()
if(_UPSCALER_FFX_STATUS)
	message(FATAL_ERROR "The pinned FidelityFX source worktree has tracked modifications; use a clean external clone.")
endif()

set(UPSCALER_FFX_SDK_DIR "${_UPSCALER_FFX_ROOT}" CACHE PATH "Root of alandtse/FidelityFX-SDK-DX11 at the pinned commit." FORCE)
message(STATUS "FidelityFX DX11 source verified at ${UPSCALER_FFX_EXPECTED_COMMIT}: ${UPSCALER_FFX_SDK_DIR}")
