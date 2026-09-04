cmake_minimum_required(VERSION 3.23)
set(NAME "fo4vr_upscaler")
set(VERSION 0.1.0)

# ---- Options ----
option(COPY_BUILD "Copy the build output to the Fallout directory." FALSE)
option(BUILD_TOOLS "Build the standalone D3D11 test harnesses under tools/." FALSE)
option(DEPLOY_TOML "Copy the shipped TOML over the deployed one on every build." ON)

# Backends are opt-in; NGX is the exception and ships on. Statically linking NVIDIA
# object code into the DLL is a redistribution decision, and it is taken and recorded
# in THIRD_PARTY.md (RTX SDKs License 1(c)/2(a); the project's Linking Exception is
# what keeps GPL terms off the vendor code). -DUPSCALER_ENABLE_NGX=OFF is the SDK-free
# contributor and CI build. Configuring ON with nothing staged is a hard error naming
# scripts/fetch-sdks.ps1, never a silent stub.
option(UPSCALER_ENABLE_NGX "Build the raw NVSDK_NGX D3D11 backend." ON)
option(UPSCALER_ENABLE_STREAMLINE "Build the Streamline backend." OFF)
option(UPSCALER_ENABLE_FFX "Build the FidelityFX (FSR) backend." OFF)

set(MO2_INSTALL_PATH "" CACHE PATH "MO2 mod folder's F4SE/Plugins directory; empty disables deploy.")

# NVIDIA's official x64 NGX library is built against the dynamic release CRT. Every
# object in one DLL must agree on that ABI, so an NGX-enabled build - the default one -
# uses static vcpkg libraries with the dynamic CRT, and the released DLL therefore
# imports VCRUNTIME140/MSVCP140. -DUPSCALER_ENABLE_NGX=OFF remains fully static.
if(UPSCALER_ENABLE_NGX)
	set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "" FORCE)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)
endif()

# ---- Cache build vars ----
macro(set_from_environment VARIABLE)
	if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
		set(${VARIABLE} $ENV{${VARIABLE}})
	endif()
endmacro()

macro(find_commonlib_path)
	if(CommonLibName AND NOT ${CommonLibName} STREQUAL "")
		# Check extern
		find_path(CommonLibPath
			CommonLibF4/include/REL/Relocation.h
			PATHS external/${CommonLibName}
			extern/${CommonLibName})

		if(${CommonLibPath} STREQUAL "CommonLibPath-NOTFOUND")
			# Check path
			set_from_environment(${CommonLibName}Path)
			set(CommonLibPath ${${CommonLibName}Path})
		endif()
	endif()
endmacro()

set_from_environment(VCPKG_ROOT)

add_compile_definitions(FALLOUTVR)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
set(CommonLibName "CommonLibF4")
set_from_environment(FalloutVRPath)
set(FalloutPath ${FalloutVRPath})
set(FalloutVersion "Fallout VR")

find_commonlib_path()
message(
	STATUS
	"Building ${NAME} ${VERSION} for ${FalloutVersion} at ${FalloutPath} with ${CommonLibName} at ${CommonLibPath}."
)

if(DEFINED VCPKG_ROOT)
	set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
else()
	message(
		WARNING
		"Variable VCPKG_ROOT is not set. Continuing without vcpkg."
	)
endif()

set(Boost_USE_STATIC_RUNTIME OFF CACHE BOOL "")

# ---- Project ----
project(
	${NAME}
	VERSION ${VERSION}
	LANGUAGES CXX
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
	@ONLY
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	@ONLY
)

# ---- Include guards ----
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(
		FATAL_ERROR
		"In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
	)
endif()

# ---- Globals ----
add_compile_definitions(
	F4SE_SUPPORT_XBYAK
)

if(MSVC)
	if(NOT ${CMAKE_GENERATOR} STREQUAL "Ninja")
		add_compile_options(
			/MP # Build with Multiple Processes
		)
	endif()
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(Boost_USE_STATIC_LIBS ON)
if(NOT UPSCALER_ENABLE_NGX)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
endif()

# ---- Dependencies ----
if(DEFINED CommonLibPath AND NOT ${CommonLibPath} STREQUAL "" AND IS_DIRECTORY ${CommonLibPath})
	add_subdirectory(${CommonLibPath} ${CommonLibName})
else()
	message(
		FATAL_ERROR
		"Variable ${CommonLibName}Path is not set."
	)
endif()

find_package(spdlog CONFIG REQUIRED)

include(cmake/FindFfxSdk.cmake)

if(UPSCALER_ENABLE_FFX)
	set(FFX_API_DX11 ON CACHE BOOL "" FORCE)
	set(FFX_API_DX12 OFF CACHE BOOL "" FORCE)
	set(FFX_API_VK OFF CACHE BOOL "" FORCE)
	set(FFX_ALL OFF CACHE BOOL "" FORCE)
	set(FFX_FSR3 OFF CACHE BOOL "" FORCE)
	set(FFX_FSR3UPSCALER ON CACHE BOOL "" FORCE)
	set(FFX_FI OFF CACHE BOOL "" FORCE)
	set(FFX_OF OFF CACHE BOOL "" FORCE)
	set(FFX_FSR ON CACHE BOOL "" FORCE)
	set(FFX_AUTO_COMPILE_SHADERS ON CACHE BOOL "" FORCE)
	block(SCOPE_FOR VARIABLES)
		# The pinned fork selects OLD CMP0116 for its shader depfiles. This project's
		# preset promotes deprecation warnings to errors; confine the compatibility
		# allowance to the external source directory.
		set(_UPSCALER_ERROR_DEPRECATED "${CMAKE_ERROR_DEPRECATED}")
		set(CMAKE_ERROR_DEPRECATED OFF CACHE BOOL "" FORCE)
		add_subdirectory("${UPSCALER_FFX_SDK_DIR}/sdk" "${CMAKE_CURRENT_BINARY_DIR}/ffx-sdk" EXCLUDE_FROM_ALL)
		set(CMAKE_ERROR_DEPRECATED "${_UPSCALER_ERROR_DEPRECATED}" CACHE BOOL "" FORCE)
	endblock()

	# The fork otherwise writes every configuration into one source-tree bin folder.
	# Keep generated archives inside this build tree so parallel presets cannot reuse
	# objects compiled with different CRT or IPO flags.
	foreach(_UPSCALER_FFX_TARGET
		ffx_backend_dx11_x64
		ffx_fsr3upscaler_x64)
		if(TARGET ${_UPSCALER_FFX_TARGET})
			set_target_properties(${_UPSCALER_FFX_TARGET} PROPERTIES
				ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ffx-sdk/lib"
				ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/ffx-sdk/lib/Debug"
				ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/ffx-sdk/lib/Release"
				ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_BINARY_DIR}/ffx-sdk/lib/RelWithDebInfo")
		endif()
	endforeach()
endif()

add_subdirectory(cmake/ffx-backend)

# ---- Backend library ----
# Deliberately links neither CommonLibF4 nor F4SE. The point of the seam is that a
# backend can be exercised by tools/subrect-harness with no game in the picture; if
# this target ever needs an engine header, the seam has already leaked.
include(cmake/backendlist.cmake)

add_library(
	upscaler_backends
	STATIC
	${backend_headers}
	${backend_sources}
)

target_compile_features(upscaler_backends PRIVATE cxx_std_23)
target_include_directories(upscaler_backends PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(upscaler_backends PUBLIC upscaler_ffx_backend spdlog::spdlog d3d11.lib dxgi.lib)

include(cmake/FindNgxSdk.cmake)

# PUBLIC, not PRIVATE: the harness links this library and must compile against the
# same configuration it was built with. A PRIVATE define here silently breaks it.
target_compile_definitions(
	upscaler_backends
	PUBLIC
	$<$<NOT:$<BOOL:${UPSCALER_NGX_SDK_DIR}>>:UPSCALER_NO_NGX>
	$<$<BOOL:${UPSCALER_ENABLE_STREAMLINE}>:UPSCALER_ENABLE_STREAMLINE>
	$<$<BOOL:${UPSCALER_ENABLE_FFX}>:UPSCALER_ENABLE_FFX>
)

if(UPSCALER_NGX_SDK_DIR)
	target_include_directories(upscaler_backends PRIVATE ${UPSCALER_NGX_INCLUDE_DIR})
	target_link_libraries(upscaler_backends PRIVATE ${UPSCALER_NGX_LIBRARY} version.lib)
endif()

# ---- Add source files ----
include(cmake/headerlist.cmake)
include(cmake/sourcelist.cmake)

source_group(
	TREE
	${CMAKE_CURRENT_SOURCE_DIR}
	FILES
	${headers}
	${sources}
)

source_group(
	TREE
	${CMAKE_CURRENT_BINARY_DIR}
	FILES
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
)

# ---- Create DLL ----
add_library(
	${PROJECT_NAME}
	SHARED
	${headers}
	${sources}
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	.clang-format
)

target_compile_features(
	${PROJECT_NAME}
	PRIVATE
	cxx_std_23
)

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
	_UNICODE
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}/include
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
	${CommonLibName}::${CommonLibName}
	upscaler_backends
)

target_precompile_headers(
	${PROJECT_NAME}
	PRIVATE
	src/PCH.h
)

if(MSVC)
	target_compile_options(
		${PROJECT_NAME}
		PRIVATE
		/sdl # Enable Additional Security Checks
		/utf-8 # Set Source and Executable character sets to UTF-8
		/Zi # Debug Information Format

		/permissive- # Standards conformance

		/Zc:alignedNew # C++17 over-aligned allocation
		/Zc:auto # Deduce Variable Type
		/Zc:char8_t
		/Zc:__cplusplus # Enable updated __cplusplus macro
		/Zc:externC
		/Zc:externConstexpr # Enable extern constexpr variables
		/Zc:forScope # Force Conformance in for Loop Scope
		/Zc:hiddenFriend
		/Zc:implicitNoexcept # Implicit Exception Specifiers
		/Zc:lambda
		/Zc:noexceptTypes # C++17 noexcept rules
		/Zc:preprocessor # Enable preprocessor conformance mode
		/Zc:referenceBinding # Enforce reference binding rules
		/Zc:rvalueCast # Enforce type conversion rules
		/Zc:sizedDealloc # Enable Global Sized Deallocation Functions
		/Zc:strictStrings # Disable string literal type conversion
		/Zc:ternary # Enforce conditional operator rules
		/Zc:threadSafeInit # Thread-safe Local Static Initialization
		/Zc:tlsGuards
		/Zc:trigraphs # Trigraphs Substitution
		/Zc:wchar_t # wchar_t Is Native Type

		/external:anglebrackets
		/external:W0

		/W4 # Warning level
		/WX # Warning level (warnings are errors)

		/wd4702
		/wd4267
		/wd4456
		/wd4189
		/wd4244
		/wd4834
		/wd4505

		"$<$<CONFIG:DEBUG>:>"
		"$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
	)

	target_link_options(
		${PROJECT_NAME}
		PRIVATE
		/WX # Treat Linker Warnings as Errors

		"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
		"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)

	# fix a warning with /Ob2 being overridden with /Ob3
	string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

if(BUILD_TOOLS)
	add_subdirectory(tools/subrect-harness)
	add_subdirectory(tools/streamline-probe)
endif()

# ---- Post build ----
if(COPY_BUILD)
	if(DEFINED FalloutPath)
		string(REGEX REPLACE "[/\\][Dd][Aa][Tt][Aa][/\\]?$" "" FalloutPath "${FalloutPath}") # remove " /data " if exists; cmake regex doesn't have /i
		add_custom_command(
			TARGET ${PROJECT_NAME}
			POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${FalloutPath}/Data/F4SE/Plugins/
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> ${FalloutPath}/Data/F4SE/Plugins/
		)
	else()
		message(
			WARNING
			"Variable ${FalloutPath} is not defined. Skipping post-build copy command."
		)
	endif()
endif()

if(DEFINED MO2_INSTALL_PATH AND NOT "${MO2_INSTALL_PATH}" STREQUAL "")
	# (MO2_INSTALL_PATH points at the mod's F4SE/Plugins dir)
	add_custom_command(
		TARGET ${PROJECT_NAME}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MO2_INSTALL_PATH}"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${MO2_INSTALL_PATH}"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${MO2_INSTALL_PATH}"
	)

	# The deployed TOML is the live-tuning surface. Off means the rig keeps whatever
	# the tester edited; on means the repo is the truth and the rig never drifts.
	if(DEPLOY_TOML)
		add_custom_command(
			TARGET ${PROJECT_NAME}
			POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Data/F4SE/Plugins/Fo4VRUpscaler.toml" "${MO2_INSTALL_PATH}"
		)
	endif()
endif()

# ---- Create Package ----
# A release archive is a GPL object-code conveyance, so the licence set travels with
# the binary. scripts/package-release.ps1 is the audited path and additionally writes
# SOURCE.txt; this target is the quick one for handing a build to a tester.
if(PACKAGE)
	set(ZIP_DIR "${CMAKE_CURRENT_BINARY_DIR}/zip")

	add_custom_target(build-time-make-directory ALL
		COMMAND ${CMAKE_COMMAND} -E rm -rf "${ZIP_DIR}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${ZIP_DIR}"
		"${ZIP_DIR}/F4SE/Plugins/"
	)

	message("Will copy mod into ${ZIP_DIR}.")
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${ZIP_DIR}/F4SE/Plugins/"
	)

	if(${CMAKE_BUILD_TYPE} STREQUAL Debug OR ${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo OR ${CMAKE_BUILD_TYPE} STREQUAL Release)
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${ZIP_DIR}/F4SE/Plugins/")
	endif()

	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Data/F4SE/Plugins/Fo4VRUpscaler.toml" "${ZIP_DIR}/F4SE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${ZIP_DIR}/"
		COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/EXCEPTIONS.md" "${ZIP_DIR}/"
		COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md" "${ZIP_DIR}/"
	)

	set(TARGET_ZIP "${PROJECT_NAME}_${PROJECT_VERSION}.7z")
	message("Will zip ${ZIP_DIR} to ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_ZIP}.")
	ADD_CUSTOM_COMMAND(
		TARGET ${PROJECT_NAME}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E tar cf ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_ZIP} --format=7zip -- .
		WORKING_DIRECTORY ${ZIP_DIR}
	)
endif()
