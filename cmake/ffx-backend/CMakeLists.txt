# FidelityFxD3D11.cpp owns an SEH boundary around the community backend dispatch.
# This subdirectory is intentional: it gives the translation unit a directory-scoped
# CMAKE_CXX_FLAGS snapshot with /EHsc removed before /EHa is selected. Appending /EHa
# to the root flags emits D9025, which is fatal under the harness's /WX build.

set(_UPSCALER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(_UPSCALER_FFX_SOURCE "${_UPSCALER_ROOT}/src/Upscaler/Backend/FidelityFxD3D11.cpp")
set(_UPSCALER_FFX_HEADER "${_UPSCALER_ROOT}/src/Upscaler/Backend/FidelityFxD3D11.h")

if(MSVC AND UPSCALER_ENABLE_FFX)
	string(REGEX REPLACE "(^|[ 	])/EHsc([ 	]|$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

add_library(upscaler_ffx_backend STATIC ${_UPSCALER_FFX_HEADER} ${_UPSCALER_FFX_SOURCE})
target_compile_features(upscaler_ffx_backend PRIVATE cxx_std_23)
target_include_directories(upscaler_ffx_backend PUBLIC "${_UPSCALER_ROOT}/src")
target_link_libraries(upscaler_ffx_backend PUBLIC spdlog::spdlog d3d11.lib dxgi.lib)

if(UPSCALER_ENABLE_FFX)
	target_compile_definitions(upscaler_ffx_backend PUBLIC UPSCALER_ENABLE_FFX)
	target_link_libraries(upscaler_ffx_backend PUBLIC ffx_backend_dx11_x64 ffx_fsr3upscaler_x64)
	if(MSVC)
		target_compile_options(upscaler_ffx_backend PRIVATE /EHa)
	endif()
endif()
