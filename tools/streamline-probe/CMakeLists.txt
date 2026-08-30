# The Streamline probe. Answers measurement M11 with no game, no headset and no hook,
# for the same reason tools/subrect-harness exists: a question that decides the whole
# backend choice should not need a play session to answer.
#
# It links nothing from this repo. The plugin must keep building with no vendor SDK
# staged, so this target only exists when the headers are actually present - a developer
# who has not run scripts/fetch-sdks.ps1 gets a configure-time note and no target, never
# a build failure.

set(SL_SDK_DIR "${CMAKE_SOURCE_DIR}/.sdk-cache/streamline")

if(NOT EXISTS "${SL_SDK_DIR}/sl.h")
	message(STATUS "streamline-probe: skipped, no Streamline SDK staged. Run scripts/fetch-sdks.ps1 to build it.")
	return()
endif()

add_executable(streamline_probe main.cpp)

target_compile_features(streamline_probe PRIVATE cxx_std_23)
target_include_directories(streamline_probe PRIVATE "${SL_SDK_DIR}")
target_link_libraries(streamline_probe PRIVATE d3d11 dxgi)

# sl.interposer.dll is loaded with LoadLibrary at run time, not linked. That is not a
# convenience: the game already links dxgi.lib and d3d11.lib, and an injected plugin
# cannot retroactively replace the process's import table. If Streamline only works when
# it owns those imports, this project cannot use it - so the probe must fail the same way
# the plugin would, which means it must not link the interposer either.

set_target_properties(
	streamline_probe
	PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/$<CONFIG>"
)

if(MSVC)
	target_compile_options(
		streamline_probe
		PRIVATE
		/sdl
		/utf-8
		/Zi
		/permissive-

		# The vendor headers are not ours to warn about, and this project builds at
		# /W4 /WX. Treating them as external is what keeps both true.
		/external:anglebrackets
		/external:W0

		/W4
		/WX
	)

	target_link_options(streamline_probe PRIVATE /WX)
endif()
