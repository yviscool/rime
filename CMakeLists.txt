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
  add_compile_options(/W4 /permissive-)
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
endif()
