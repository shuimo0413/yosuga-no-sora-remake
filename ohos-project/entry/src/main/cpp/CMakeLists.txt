# ---------------------------------------------------------------------------
# OpenHarmony entry module: ArkTS shell + Kirikiri SDL2 engine
#
# hvigor drives this file through the OpenHarmony NDK CMake toolchain
# (ohos.toolchain.cmake) and passes OHOS_STL, OHOS_ARCH and CMAKE_BUILD_TYPE.
# Plain library names resolve inside the SDK sysroot.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.15)

# OHOS: define __OHOS__ for all targets in this build (engine + entry source use it)
add_definitions(-D__OHOS__=1)
project(yosuga_ohos_entry)

set(ENTRY_DIR ${CMAKE_CURRENT_SOURCE_DIR})
# ohos-project/entry/src/main/cpp -> repository root
get_filename_component(REPO_ROOT "${ENTRY_DIR}/../../../../.." ABSOLUTE)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_EXTENSIONS OFF)

# OpenHarmony uses musl libc: pthread is part of libc and dlopen lives in it.
# Pretending to be UNIX makes SDL2's CMake select pthread threads, unix
# timers, dlopen and standard headers.
set(UNIX ON)
set(OHOS TRUE)

# ---- Vendored SDL2 with the OpenHarmony backend ----------------------------
# tools/setup_ohos_project.py copies external/SDL here and applies the
# OpenHarmony video/filesystem backend patch.
set(SDL2_ROOT ${ENTRY_DIR}/third_party/SDL)
if(NOT EXISTS "${SDL2_ROOT}/CMakeLists.txt")
	message(FATAL_ERROR
		"SDL2 sources are missing at ${SDL2_ROOT}. Initialize the submodules "
		"(git submodule update --init) and run tools/setup_ohos_project.py.")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO_OPENGL ON CACHE BOOL "" FORCE)
set(SDL_VIDEO_OPENGL_EGL ON CACHE BOOL "" FORCE)
set(SDL_VIDEO_OPENGL_ES ON CACHE BOOL "" FORCE)
set(SDL_VIDEO_OPENGL_ES2 ON CACHE BOOL "" FORCE)
set(SDL_VIDEO_RENDER_OGL ON CACHE BOOL "" FORCE)
set(SDL_VIDEO_RENDER_OGL_ES ON CACHE BOOL "" FORCE)
set(SDL_VIDEO_RENDER_OGL_ES2 ON CACHE BOOL "" FORCE)
# OHOS: EGL/GLES are linked into the app; use link-time symbols in SDL's EGL
# loader instead of dlopen (sandbox blocks dlopen of system GLES libraries).
set(SDL_VIDEO_STATIC_ANGLE ON CACHE BOOL "" FORCE)
set(SDL_VIDEO_DRIVER_DUMMY ON CACHE BOOL "" FORCE)
set(SDL_AUDIO_DRIVER_DUMMY ON CACHE BOOL "" FORCE)
set(SDL_LOADSO_DLOPEN ON CACHE BOOL "" FORCE)
# hvigor/.cxx CMake caches persist across CI runs; a stale cache can leave
# SDL_RENDER_DISABLED set, which makes SDL_config.h define the macro and
# SDL_CreateRenderer() fail with "SDL not built with rendering support" even
# though the GLES2/software render drivers are compiled in. FORCE it OFF so
# the generated SDL_config.h always keeps the render subsystem enabled.
set(SDL_RENDER_DISABLED OFF CACHE BOOL "" FORCE)

add_subdirectory(${SDL2_ROOT} sdl2_ohos EXCLUDE_FROM_ALL)

# SDL 2.0.18+ names the static target SDL2-static; older SDL2 builds the
# SDL2 target statically when shared libraries are disabled.
if(TARGET SDL2-static)
	set(SDL2_TARGET SDL2-static)
elseif(TARGET SDL2)
	set(SDL2_TARGET SDL2)
else()
	message(FATAL_ERROR "The vendored SDL2 build produced neither SDL2 nor SDL2-static")
endif()

# OHOS: EGL/GLES are linked into the app; make SDL's EGL loader use link-time
# symbols (LOAD_FUNC(NAME) = NAME) instead of dlopen, which the app sandbox
# blocks for system GLES libraries ("Could not initialize OpenGL / GLES library").
target_compile_definitions(${SDL2_TARGET} PRIVATE
	SDL_VIDEO_STATIC_ANGLE=1
	# OHOS platform is not detected by SDL's CMake, so it never sets the
	# OpenGLES render flags in SDL_config.h; without them SDL_CreateRenderer(
	# SDL_RENDERER_ACCELERATED) finds no render driver and falls back to the
	# software surface renderer (the screen then never updates). Force the
	# EGL/GLES2 render driver macros so the opengles2 renderer is compiled in.
	SDL_VIDEO_OPENGL=1
	SDL_VIDEO_OPENGL_EGL=1
	SDL_VIDEO_OPENGL_ES=1
	SDL_VIDEO_OPENGL_ES2=1
	SDL_VIDEO_RENDER_OGL=1
	SDL_VIDEO_RENDER_OGL_ES=1
	SDL_VIDEO_RENDER_OGL_ES2=1)

# Help FAudio compile its SDL2 backend against the vendored SDL2. When the
# target exposes no interface include directories the hint is a no-op and
# FAudio falls back to its built-in audio backend.
get_target_property(SDL2_INC_DIRS ${SDL2_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
if(NOT SDL2_INC_DIRS OR SDL2_INC_DIRS MATCHES "NOTFOUND")
	set(SDL2_INC_DIRS ${SDL2_ROOT}/include)
endif()
set(SDL2_INCLUDE_DIRS ${SDL2_INC_DIRS} CACHE PATH "SDL2 include directories" FORCE)
set(SDL2_LIBRARIES ${SDL2_TARGET} CACHE STRING "SDL2 libraries" FORCE)

# ---- Kirikiri SDL2 engine (repository root CMakeLists) ---------------------
set(_DISABLE_INSTALLATION TRUE CACHE BOOL "" FORCE)
set(OPTION_ENABLE_EXTERNAL_PLUGINS OFF CACHE BOOL "" FORCE)
set(OPTION_ENABLE_CANVAS OFF CACHE BOOL "" FORCE)
set(KRKRSDL2_GENERATE_CONTENT_MANIFEST OFF CACHE BOOL "" FORCE)
set(KRKRSDL2_ENABLE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT} krkrsdl2_ohos EXCLUDE_FROM_ALL)

# krkrsdl2.so uses SDL's EGL loader with link-time symbols (SDL_VIDEO_STATIC_ANGLE);
# it must link EGL itself or the egl* references stay undefined. Plain signature
# to match the existing target_link_libraries calls inside the repo CMakeLists.
target_link_libraries(krkrsdl2 EGL GLESv3 libhilog_ndk.z.so libnative_window.so libohaudio.so)
# OHOS: define __OHOS__ for the krkrsdl2 target (engine source uses it)
target_compile_definitions(krkrsdl2 PRIVATE __OHOS__=1)

# ---- The NAPI entry library (libentry.so) ----------------------------------
add_library(entry SHARED
	napi_init.cpp
	krkrsdl2_ohos_entry.cpp
	ohos_video_player.cpp
	ohos_data_extract.cpp
	# Compiled here as well so the touch/surface bridge symbols are provided
	# by libentry.so itself (the SDL static archive is consumed by the
	# libkrkrsdl2.so link and its members are not pulled in a second time).
	${SDL2_ROOT}/src/video/ohos/SDL_ohosevents.c
)

target_include_directories(entry PRIVATE
	${ENTRY_DIR}
	${ENTRY_DIR}/sdl_ohos
)

target_link_libraries(entry PUBLIC
	krkrsdl2
	${SDL2_TARGET}
	libace_napi.z.so
	libace_ndk.z.so
	libnative_window.so
	libnative_buffer.so
	librawfile.z.so
	libhilog_ndk.z.so
	libavplayer.so
	libnative_media_core.so
	EGL
	GLESv3
	# Provides SDL's generated-config include paths and compile definitions
	# for the SDL_ohosevents.c file compiled into this target above.
	sdl-build-options
)
