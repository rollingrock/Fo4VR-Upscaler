# The standalone sub-rect harness. It links upscaler_backends and nothing else from this
# repo - no CommonLibF4, no F4SE, no VR runtime, no game - which is the point: the sub-rect
# question gets answered in an evening rather than a play session. It is not a separate
# repository because it shares exactly one thing with the plugin, the backend
# implementation, and a proof about a lookalike would not be a proof about what ships.

add_executable(subrect_harness main.cpp)

target_compile_features(subrect_harness PRIVATE cxx_std_23)
target_link_libraries(subrect_harness PRIVATE upscaler_backends)

# The UPSCALER_* definitions are PUBLIC on upscaler_backends precisely so this target sees
# the same configuration the library was built with. A PRIVATE define there would leave
# main.cpp compiling the SDK branch against an SDK that is not present.

target_compile_definitions(
	subrect_harness
	PRIVATE
	$<$<CONFIG:Debug>:UPSCALER_HARNESS_D3D_DEBUG>
)

if(UPSCALER_NGX_TEST_RUNTIME)
	get_filename_component(UPSCALER_NGX_TEST_RUNTIME_DIR "${UPSCALER_NGX_TEST_RUNTIME}" DIRECTORY)
	target_compile_definitions(
		subrect_harness
		PRIVATE
		UPSCALER_NGX_RUNTIME_DIR="${UPSCALER_NGX_TEST_RUNTIME_DIR}"
	)
	# Headers only. The negative control drives the raw SDK once to record what the
	# runtime itself answers to a base it was not created for; the library already
	# arrives through upscaler_backends, so this adds no link dependency.
	target_include_directories(subrect_harness PRIVATE ${UPSCALER_NGX_INCLUDE_DIR})
elseif(UPSCALER_NGX_SDK_DIR)
	message(FATAL_ERROR "The NGX hardware harness requires a staged production nvngx_dlss.dll.")
endif()

# A generator expression here suppresses the per-config subdirectory CMake would otherwise
# append, so the executable is at buildvr/tools/<config>/ for both the CI step and a human.
set_target_properties(
	subrect_harness
	PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/$<CONFIG>"
)

if(MSVC)
	target_compile_options(
		subrect_harness
		PRIVATE
		/sdl
		/utf-8
		/Zi
		/permissive-

		/external:anglebrackets
		/external:W0

		/W4
		/WX

		/wd4702
		/wd4267
		/wd4456
		/wd4189
		/wd4244
		/wd4834
		/wd4505
	)

	target_link_options(
		subrect_harness
		PRIVATE
		/WX
	)
endif()
