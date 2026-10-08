cmake_minimum_required(VERSION 3.24)
project(rime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RIME_BUILD_TESTS "Build runtime contract tests" ON)
option(RIME_ENABLE_QUICKJS "Build the QuickJS-ng host" OFF)
option(RIME_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(RIME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(RIME_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif()
endif()

if(MSVC)
  # Every source here is UTF-8. Without /utf-8 MSVC reads them in the system
  # code page, which makes C4819 locale-dependent (a CP936 machine rejects
  # code-page-blind UTF-8 comments and fails the /WX dev preset) and gives
  # narrow literals different bytes than the MinGW build produces.
  add_compile_options(/W4 /permissive- /utf-8)
  if(RIME_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(RIME_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

include(CTest)

# Embed the ComCtl32 v6 manifest (engine/win32/resources/visual-styles.manifest)
# into an executable that hosts the runtime. Windows only honours
# EM_SETCUEBANNER - GuiControl.SetCue - in a process declaring
# Microsoft.Windows.Common-Controls 6.0, so every exe able to run GUI code
# carries it instead of silently dropping the cue.
#
# MSVC asks link.exe to merge it (/MANIFESTINPUT) because the linker already
# generates its own manifest and a second RT_MANIFEST resource makes the
# conversion fail with CVT1100. MinGW generates no manifest, so windres
# embeds the same file as RT_MANIFEST through the .rc.
function(rime_embed_comctl32_v6 target)
  if(MSVC)
    # link.exe already writes a manifest of its own, so adding a second
    # RT_MANIFEST resource fails the conversion (CVT1100). Merge ours into
    # the finished image with the manifest tool CMake located (CMAKE_MT);
    # the argument assembly lives in a script because the ";" in
    # -outputresource:file;1 does not survive the command writer.
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
              "-DRIME_MT=${CMAKE_MT}"
              "-DRIME_MANIFEST=${CMAKE_SOURCE_DIR}/engine/win32/resources/visual-styles.manifest"
              "-DRIME_EXE=$<TARGET_FILE:${target}>"
              -P "${CMAKE_SOURCE_DIR}/engine/win32/resources/embed-manifest.cmake"
      COMMENT "Embedding ComCtl32 v6 manifest into $<TARGET_FILE:${target}>"
      VERBATIM)
  else()
    target_sources(${target} PRIVATE
      ${CMAKE_SOURCE_DIR}/engine/win32/resources/visual-styles.rc)
  endif()
endfunction()

add_subdirectory(engine)
if(RIME_ENABLE_QUICKJS)
  add_subdirectory(engine/js)
endif()
if(WIN32 AND RIME_ENABLE_QUICKJS)
  add_subdirectory(engine/win32/js)
endif()
add_subdirectory(hosts/desktop)

if(RIME_BUILD_TESTS)
  # Linked into every native test executable: assert/abort/CRT failures must
  # land on stderr with a nonzero exit code and never raise a dialog, or a
  # broken test would block ctest until someone clicks the message box.
  add_library(rime_test_support OBJECT tests/test_support.cpp)
  target_compile_features(rime_test_support PRIVATE cxx_std_20)

  add_subdirectory(tests/native)
  if(RIME_ENABLE_QUICKJS)
    add_subdirectory(tests/js)
  endif()

  if(MSVC AND RIME_ENABLE_ASAN)
    # MSVC's ASan runtime is a loader-time DLL (clang_rt.asan_dynamic). The
    # test exes import it, and a run without vcvars on PATH (a raw ctest from
    # any other shell) dies with 0xc0000135 behind a modal "DLL not found"
    # dialog that blocks ctest until someone clicks it. Copy the DLL next to
    # the test binaries so the exe-directory search always finds it.
    find_file(RIME_ASAN_RUNTIME_DLL
      NAMES clang_rt.asan_dynamic-x86_64.dll clang_rt.asan_dynamic-i386.dll
      HINTS "$ENV{VCToolsInstallDir}/bin/Hostx64/x64" "$ENV{VCToolsInstallDir}/bin/Hostx86/x86"
      NO_DEFAULT_PATH)
    if(RIME_ASAN_RUNTIME_DLL)
      foreach(_rime_asan_dir tests/native tests/js hosts/desktop)
        file(COPY "${RIME_ASAN_RUNTIME_DLL}" DESTINATION "${CMAKE_BINARY_DIR}/${_rime_asan_dir}")
      endforeach()
    else()
      message(STATUS "MSVC ASan runtime DLL not found next to tests; loader errors may block on a dialog")
    endif()
  endif()

  if(MINGW)
    # MinGW test binaries import libwinpthread (and libgcc/libstdc++ where
    # the static flags do not apply). Those DLLs only exist on the
    # toolchain PATH, so a shell without it dies with 0xc0000135 behind a
    # modal loader dialog - the same failure class as the ASan runtime
    # above. Copy them next to the test binaries at configure time.
    unset(RIME_MINGW_libwinpthread-1.dll CACHE)
    unset(RIME_MINGW_libgcc_s_seh-1.dll CACHE)
    unset(RIME_MINGW_libstdc++-6.dll CACHE)
    get_filename_component(_rime_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(_rime_mingw_dll libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll)
      if(EXISTS "${_rime_mingw_bin}/${_rime_mingw_dll}")
        foreach(_rime_mingw_dir tests/native tests/js hosts/desktop)
          file(COPY "${_rime_mingw_bin}/${_rime_mingw_dll}"
               DESTINATION "${CMAKE_BINARY_DIR}/${_rime_mingw_dir}")
        endforeach()
      endif()
    endforeach()
  endif()
endif()
