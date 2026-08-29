# The standalone sub-rect harness. It links upscaler_backends and nothing else from this
# repo - no CommonLibF4, no F4SE, no VR runtime, no game - which is the point: the sub-rect
# question gets answered in an evening rather than a play session. It is not a separate
# repository because it shares exactly one thing with the plugin, the backend
# implementation, and a proof about a lookalike would not be a proof about what ships.

find_package(Catch2 CONFIG REQUIRED)

add_executable(subrect_harness main.cpp)

target_compile_features(subrect_harness PRIVATE cxx_std_23)
target_link_libraries(subrect_harness PRIVATE upscaler_backends)

# The UPSCALER_* definitions are PUBLIC on upscaler_backends precisely so this target sees
# the same configuration the library was built with. A PRIVATE define there would leave
# main.cpp compiling the SDK branch against an SDK that is not present.
if(UPSCALER_NGX_SDK_DIR)
	target_link_libraries(subrect_harness PRIVATE Catch2::Catch2WithMain)
else()
	# With no SDK staged the harness is one main that says so and exits 0, so the
	# main-providing Catch2 target would be a duplicate symbol.
	target_link_libraries(subrect_harness PRIVATE Catch2::Catch2)
endif()

target_compile_definitions(
	subrect_harness
	PRIVATE
	$<$<CONFIG:Debug>:UPSCALER_HARNESS_D3D_DEBUG>
)

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
