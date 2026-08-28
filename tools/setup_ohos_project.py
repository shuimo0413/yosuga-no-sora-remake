#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare the OpenHarmony project for a build.

Performs two steps:

1. Vendors the pinned SDL2 submodule (external/SDL) into
   ohos-project/entry/src/main/cpp/third_party/SDL and applies the
   OpenHarmony backend patch:

   * copies the XComponent/EGL video driver, the event glue and the
     filesystem hooks from ohos-project/entry/src/main/cpp/sdl_ohos into the
     SDL tree,
   * registers the driver in src/video/SDL_video.c,
   * wires the new sources and the required preprocessor definitions into
     SDL's CMakeLists.txt.

2. (Optional) Installs the data-assets.json download manifest into the
   rawfile directory so the in-game downloader knows which zip assets to
   fetch. By default the HAP does NOT bundle the multi-GiB data/ tree;
   pass --data-link to keep the old bundled-data behavior.

Usage:
    python tools/setup_ohos_project.py [--data-link] [--assets-file PATH]
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SDL_SOURCE = REPO_ROOT / "external" / "SDL"
OHOS_PROJECT = REPO_ROOT / "ohos-project"
ENTRY_CPP = OHOS_PROJECT / "entry" / "src" / "main" / "cpp"
SDL_DEST = ENTRY_CPP / "third_party" / "SDL"
DRIVER_SOURCE = ENTRY_CPP / "sdl_ohos"
DATA_DIR = REPO_ROOT / "data"
RAWFILE_DIR = OHOS_PROJECT / "entry" / "src" / "main" / "resources" / "rawfile"

VIDEO_DRIVER_FILES = [
    "SDL_ohosvideo.c",
    "SDL_ohosvideo.h",
    "SDL_ohosgl.c",
    "SDL_ohosgl.h",
    "SDL_ohosevents.c",
    "SDL_ohosevents.h",
    "sdl_ohos_bridge.h",
]

AUDIO_DRIVER_FILES = [
    "SDL_ohosaudio.c",
    "SDL_ohosaudio.h",
]

SDL_CMAKE_OHOS_BLOCK = """
# --- OpenHarmony backend (added by tools/setup_ohos_project.py) ---
if(OHOS)
  include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src/video/ohos")
  include_directories("${CMAKE_CURRENT_SOURCE_DIR}/src/audio/ohos")
  # Replace every platform filesystem implementation with the OpenHarmony one.
  list(FILTER SOURCE_FILES EXCLUDE REGEX "src/filesystem/(unix|android|windows|winrt|cocoa|psp|dummy|vita|n3ds|riscos|haiku|emscripten|os2)/SDL_sysfilesystem")
  # SDL_JOYSTICK_DISABLED / SDL_HAPTIC_DISABLED / SDL_POWER_DISABLED make
  # SDL_joystick.c / SDL_haptic.c / SDL_power.c reference the DUMMY driver
  # symbols, but OHOS matches no platform branch in SDL's CMake source
  # collection, so the dummy .c files never enter SOURCE_FILES and the link
  # fails with undefined SDL_DUMMY_*Driver. Add them explicitly.
  file(GLOB OHOS_JOYSTICK_DUMMY_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/joystick/dummy/*.c)
  file(GLOB OHOS_HAPTIC_DUMMY_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/haptic/dummy/*.c)
  file(GLOB OHOS_POWER_DUMMY_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/power/dummy/*.c)
  list(APPEND SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/ohos/SDL_ohosvideo.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/ohos/SDL_ohosevents.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video/ohos/SDL_ohosgl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/ohos/SDL_ohosaudio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/filesystem/ohos/SDL_sysfilesystem.c
    ${OHOS_JOYSTICK_DUMMY_SOURCES}
    ${OHOS_HAPTIC_DUMMY_SOURCES}
    ${OHOS_POWER_DUMMY_SOURCES}
  )
  # NOTE: do NOT define SDL_RENDER_DISABLED here - not even as 0. This SDL
  # version guards the whole render subsystem with "#ifndef SDL_RENDER_DISABLED",
  # so merely DEFINING the macro disables SDL_CreateRenderer entirely (it
  # returns "SDL not built with rendering support" and even the software
  # renderer is gone). Leave the macro undefined; SDL_RENDER_DISABLED is forced
  # OFF as a CMake variable in the entry CMakeLists, which turns the
  # corresponding SDL_config.h cmakedefine into a comment.
  add_definitions(
    -DSDL_VIDEO_DRIVER_OHOS=1
    -DSDL_AUDIO_DRIVER_OHOS=1
    -DSDL_JOYSTICK_DISABLED=1
    -DSDL_HAPTIC_DISABLED=1
    -DSDL_SENSOR_DISABLED=1
    -DSDL_POWER_DISABLED=1
    -DSDL_VIDEO_OPENGL=1
    -DSDL_VIDEO_OPENGL_EGL=1
    -DSDL_VIDEO_OPENGL_ES=1
    -DSDL_VIDEO_OPENGL_ES2=1
    -DSDL_VIDEO_RENDER_OGL=1
    -DSDL_VIDEO_RENDER_OGL_ES=1
    -DSDL_VIDEO_RENDER_OGL_ES2=1
  )
  set(SDL_VIDEO_OPENGL_ES 1 CACHE BOOL "" FORCE)
  set(SDL_VIDEO_OPENGL_ES2 1 CACHE BOOL "" FORCE)
  set(SDL_VIDEO_RENDER_OGL_ES 1 CACHE BOOL "" FORCE)
  set(SDL_VIDEO_RENDER_OGL_ES2 1 CACHE BOOL "" FORCE)
  set(SDL_RENDER_DISABLED 0 CACHE BOOL "" FORCE)
  set(HAVE_VIDEO_OPENGLES TRUE)
  set(HAVE_VIDEO_OPENGLES2 TRUE)
  set(HAVE_VIDEO_OPENGLES3 TRUE)
endif()
"""


def fail(message):
    print("setup_ohos_project.py: error: " + message, file=sys.stderr)
    sys.exit(1)


def check_sdl_source():
    if not (SDL_SOURCE / "CMakeLists.txt").is_file():
        fail(
            "external/SDL is not checked out. Initialize the submodules first:\n"
            "    git submodule update --init --recursive\n"
            "and make sure the SDL2 branch of libsdl-org/SDL is present."
        )
    if not (SDL_SOURCE / "src" / "video" / "SDL_video.c").is_file():
        fail("external/SDL does not look like an SDL2 source tree.")


def vendor_sdl():
    print("Vendoring SDL2 into %s ..." % SDL_DEST)
    if SDL_DEST.exists():
        shutil.rmtree(SDL_DEST)
    SDL_DEST.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        SDL_SOURCE,
        SDL_DEST,
        ignore=shutil.ignore_patterns(".git", ".gitmodules"),
    )

    video_dir = SDL_DEST / "src" / "video" / "ohos"
    filesystem_dir = SDL_DEST / "src" / "filesystem" / "ohos"
    video_dir.mkdir(parents=True, exist_ok=True)
    filesystem_dir.mkdir(parents=True, exist_ok=True)

    for name in VIDEO_DRIVER_FILES:
        source = DRIVER_SOURCE / name
        if not source.is_file():
            fail("missing driver source: %s" % source)
        # The OHOS CMake build compiles the same driver sources from both the
        # video and the filesystem trees; both copies come straight from the
        # same source file, and the second is kept as a hard link so the two
        # trees can never drift apart.
        shutil.copy2(source, video_dir / name)
        linked = filesystem_dir / name
        if linked.exists():
            linked.unlink()
        try:
            os.link(video_dir / name, linked)
        except OSError:
            # Cross-device or filesystems without hard links: fall back to a
            # plain copy (still byte-identical at this point).
            shutil.copy2(video_dir / name, linked)

    audio_dir = SDL_DEST / "src" / "audio" / "ohos"
    audio_dir.mkdir(parents=True, exist_ok=True)
    for name in AUDIO_DRIVER_FILES:
        source = DRIVER_SOURCE / name
        if not source.is_file():
            fail("missing driver source: %s" % source)
        shutil.copy2(source, audio_dir / name)

    filesystem_source = DRIVER_SOURCE / "SDL_sysfilesystem.c"
    if not filesystem_source.is_file():
        fail("missing driver source: %s" % filesystem_source)
    shutil.copy2(filesystem_source, filesystem_dir / "SDL_sysfilesystem.c")


def insert_after(lines, anchor, block, label, target):
    """Insert block after the first line that starts with anchor."""
    for index, line in enumerate(lines):
        if line.startswith(anchor):
            lines[index:index + 1] = [line] + block
            print("Patched %s (%s)" % (target.name, label))
            return
    fail("could not find anchor %r in %s" % (anchor, target))


def patch_sdl():
    print("Patching vendored SDL2 ...")

    # Register the driver declaration. Recent SDL2 keeps the bootstrap
    # externs in SDL_sysvideo.h; older trees declare them in SDL_video.c.
    extern_patched = False
    for relative in ("src/video/SDL_sysvideo.h", "src/video/SDL_video.c"):
        target = SDL_DEST / relative
        text = target.read_text(encoding="utf-8", errors="replace")
        if "OHOS_bootstrap" in text:
            print("%s already patched; skipping." % relative)
            extern_patched = True
            break
        if "extern VideoBootStrap Android_bootstrap;" in text:
            lines = text.splitlines(keepends=True)
            insert_after(
                lines,
                "extern VideoBootStrap Android_bootstrap;",
                ["extern VideoBootStrap OHOS_bootstrap;\n"],
                "bootstrap extern",
                target,
            )
            target.write_text("".join(lines), encoding="utf-8")
            extern_patched = True
            break
    if not extern_patched:
        fail("could not find 'extern VideoBootStrap Android_bootstrap;' in "
             "the vendored SDL video headers")

    # Add the driver to the bootstrap table in SDL_video.c.
    video_c = SDL_DEST / "src" / "video" / "SDL_video.c"
    text = video_c.read_text(encoding="utf-8", errors="replace")
    if "&OHOS_bootstrap," not in text:
        lines = text.splitlines(keepends=True)
        # The OHOS entry MUST be a standalone #if block AFTER the Android
        # block's #endif. Inserting right after "&Android_bootstrap," nests it
        # INSIDE "#ifdef SDL_VIDEO_DRIVER_ANDROID", which the OpenHarmony build
        # skips (Android macro unset) - OHOS_bootstrap never enters the bootstrap
        # table and SDL falls back to offscreen. (Seen as a HAP shipping with
        # only dummy/offscreen video drivers.)
        inserted = False
        for index, line in enumerate(lines):
            if line.strip() == "&Android_bootstrap,":
                for j in range(index, min(index + 12, len(lines))):
                    if lines[j].strip() == "#endif":
                        block = ["#if SDL_VIDEO_DRIVER_OHOS\n", "    &OHOS_bootstrap,\n", "#endif\n"]
                        lines[j+1:j+1] = block
                        inserted = True
                        break
                break
        if not inserted:
            fail("could not find '&Android_bootstrap,' + '#endif' in the vendored "
                 "SDL video bootstrap table")
        video_c.write_text("".join(lines), encoding="utf-8")
    else:
        print("SDL_video.c already patched; skipping.")

    # Register the audio driver declaration in SDL_sysaudio.h.
    sysaudio = SDL_DEST / "src" / "audio" / "SDL_sysaudio.h"
    text = sysaudio.read_text(encoding="utf-8", errors="replace")
    if "OHOSAUDIO_bootstrap" in text:
        print("SDL_sysaudio.h already patched; skipping.")
    else:
        lines = text.splitlines(keepends=True)
        insert_after(
            lines,
            "extern AudioBootStrap OS2AUDIO_bootstrap;",
            ["#ifdef SDL_AUDIO_DRIVER_OHOS\n", "extern AudioBootStrap OHOSAUDIO_bootstrap;\n", "#endif\n"],
            "audio bootstrap extern",
            sysaudio,
        )
        sysaudio.write_text("".join(lines), encoding="utf-8")

    # Add the audio driver to the bootstrap table in SDL_audio.c.
    # The table lines are indented with 4 spaces, so the anchor must
    # include the indentation (insert_after uses startswith).
    audio_c = SDL_DEST / "src" / "audio" / "SDL_audio.c"
    text = audio_c.read_text(encoding="utf-8", errors="replace")
    if "&OHOSAUDIO_bootstrap," not in text:
        lines = text.splitlines(keepends=True)
        insert_after(
            lines,
            "    &ANDROIDAUDIO_bootstrap,",
            ["#endif\n", "#ifdef SDL_AUDIO_DRIVER_OHOS\n", "    &OHOSAUDIO_bootstrap,\n"],
            "audio bootstrap table",
            audio_c,
        )
        audio_c.write_text("".join(lines), encoding="utf-8")
    else:
        print("SDL_audio.c already patched; skipping.")

    cmake_file = SDL_DEST / "CMakeLists.txt"
    text = cmake_file.read_text(encoding="utf-8", errors="replace")
    if "SDL_VIDEO_DRIVER_OHOS" in text:
        print("SDL CMakeLists.txt already patched; skipping.")
    else:
        lines = text.splitlines(keepends=True)
        inserted = False
        # The OHOS block MUST live at the top level, outside if(SDL_SHARED)/
        # if(SDL_STATIC). The OpenHarmony build forces SDL_SHARED=OFF and
        # SDL_STATIC=ON; a block inserted before "add_library(SDL2 SHARED"
        # lands INSIDE if(SDL_SHARED) and is skipped entirely, so
        # SDL_ohosvideo.c is never added to SOURCE_FILES and
        # SDL_VIDEO_DRIVER_OHOS stays undefined - the HAP then ships with
        # only dummy/offscreen video drivers and SDL_CreateWindow fails with
        # "eglQueryDevicesEXT is missing". Insert before if(SDL_SHARED) instead.
        for index, line in enumerate(lines):
            if line.lstrip().startswith("if(SDL_SHARED)"):
                block = SDL_CMAKE_OHOS_BLOCK.splitlines(keepends=True)
                lines[index:index] = block
                cmake_file.write_text("".join(lines), encoding="utf-8")
                print("Patched CMakeLists.txt (OpenHarmony block before if(SDL_SHARED))")
                inserted = True
                break
        if not inserted:
            for anchor in ("add_library(SDL2 SHARED", "sdl_add_library(", "add_library(SDL2 STATIC"):
                for index, line in enumerate(lines):
                    if line.lstrip().startswith(anchor):
                        block = SDL_CMAKE_OHOS_BLOCK.splitlines(keepends=True)
                        lines[index:index] = block
                        cmake_file.write_text("".join(lines), encoding="utf-8")
                        print("Patched CMakeLists.txt (OpenHarmony block before %r)" % anchor)
                        inserted = True
                        break
                if inserted:
                    break
        if not inserted:
            fail("could not find an SDL library creation anchor in the vendored "
                 "SDL CMakeLists.txt")

    # SDL_egl.c: the OpenHarmony system EGL does not expose
    # EGL_EXT_device_enumeration / EGL_EXT_platform_base, so eglQueryDevicesEXT
    # and eglGetPlatformDisplayEXT resolve to NULL and SDL_EGL_InitializeOffscreen()
    # fails with "eglQueryDevicesEXT is missing". With SDL_VIDEO_STATIC_ANGLE the
    # core LOAD_FUNC links symbols directly, but LOAD_FUNC_EGLEXT still uses
    # eglGetProcAddress() (system EGL), which never sees our link-time symbol.
    # Make LOAD_FUNC_EGLEXT a link-time reference too so our OHOS_EGL stubs
    # (SDL_ohosgl.c) resolve.
    egl_c = SDL_DEST / "src" / "video" / "SDL_egl.c"
    egl_text = egl_c.read_text(encoding="utf-8", errors="replace")
    if "OHOS_LOAD_FUNC_EGLEXT" in egl_text:
        print("SDL_egl.c already patched; skipping.")
    else:
        lines = egl_text.splitlines(keepends=True)
        patched = False
        for index, line in enumerate(lines):
            if line.strip().startswith("#define LOAD_FUNC_EGLEXT(NAME)"):
                # Consume the whole multi-line macro (the definition plus its
                # continuation to the terminating ';'), so the stale original
                # body (eglGetProcAddress) does not remain alongside ours.
                end = index
                while end < len(lines):
                    if lines[end].rstrip().endswith("\\"):
                        end += 1
                    else:
                        end += 1
                        break
                # Skip any trailing comment line above the macro.
                replacement_index = index
                if replacement_index > 0 and lines[replacement_index-1].lstrip().startswith("/* it is allowed"):
                    replacement_index -= 1
                # Replace the LOAD_FUNC_EGLEXT macro body with a link-time ref.
                # Keep the multi-line macro: on static-ANGLE, reference NAME.
                block = [
                    "/* OHOS (tools/setup_ohos_project.py): resolve EGL extensions so the SDL_EGL_InitializeOffscreen\n",
                    " * path does not fail. SDL_EGL_GetProcAddress falls back to SDL_LoadFunction(), and with\n",
                    " * SDL_VIDEO_STATIC_ANGLE opengl_dll_handle stays NULL so SDL_LoadFunction dlsyms the global\n",
                    " * namespace - our link-time eglQueryDevicesEXT / eglGetPlatformDisplayEXT stubs resolve. */\n",
                    "#define LOAD_FUNC_EGLEXT(NAME) \\\n",
                    "    _this->egl_data->NAME = (void *)SDL_EGL_GetProcAddress(_this, #NAME);\n",
                ]
                # Replace the whole macro range (from the comment line down to
                # the macro's terminating ';' line) with our block.
                lines[replacement_index:end] = block
                egl_c.write_text("".join(lines), encoding="utf-8")
                print("Patched SDL_egl.c (LOAD_FUNC_EGLEXT link-time ref)")
                patched = True
                break
        if not patched:
            fail("could not find LOAD_FUNC_EGLEXT macro in vendored SDL_egl.c")


def install_assets(assets_file):
    RAWFILE_DIR.mkdir(parents=True, exist_ok=True)
    target = RAWFILE_DIR / "data-assets.json"
    if assets_file.is_file():
        shutil.copy2(assets_file, target)
        print("Installed rawfile data-assets.json (%d bytes)" % target.stat().st_size)
    else:
        fail("data-assets manifest not found: %s" % assets_file)


def link_data():
    data_link = RAWFILE_DIR / "data"
    RAWFILE_DIR.mkdir(parents=True, exist_ok=True)

    if not (DATA_DIR / "startup.tjs").is_file():
        fail("data/startup.tjs is missing; download the Git LFS content first (git lfs pull).")

    if data_link.exists() or data_link.is_symlink():
        print("rawfile data link already exists: %s" % data_link)
        return

    if os.name == "nt":
        command = ["cmd", "/c", "mklink", "/J", str(data_link), str(DATA_DIR.resolve())]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            try:
                os.symlink(DATA_DIR.resolve(), data_link, target_is_directory=True)
            except OSError as error:
                fail(
                    "could not create the rawfile data junction (%s). Try running "
                    "this script from an elevated prompt, or create the junction "
                    "manually:\n    mklink /J %s %s"
                    % (error, data_link, DATA_DIR.resolve())
                )
    else:
        relative = os.path.relpath(DATA_DIR.resolve(), RAWFILE_DIR.resolve())
        os.symlink(relative, data_link, target_is_directory=True)

    print("Linked rawfile data: %s -> %s" % (data_link, DATA_DIR))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-link", action="store_true",
                        help="bundle the full data/ tree as rawfile (large HAP)")
    parser.add_argument("--assets-file", type=Path, default=None,
                        help="copy this data-assets.json into the rawfile directory")
    args = parser.parse_args()

    check_sdl_source()
    vendor_sdl()
    patch_sdl()
    if args.assets_file is not None:
        install_assets(args.assets_file)
    if args.data_link:
        link_data()
    print("OpenHarmony project is ready.")
    print("Next: install DevEco Studio (OpenHarmony, API 12) and build "
          "ohos-project/ with hvigor, or push a tag to let the CI workflow build the HAP.")


if __name__ == "__main__":
    main()
