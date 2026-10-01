include(FetchContent)

set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_WERROR OFF CACHE BOOL "" FORCE)
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  quickjs_ng
  URL https://codeload.github.com/quickjs-ng/quickjs/tar.gz/6d46d07d04041b40f4f49eaa7fdebe44c314c699
  URL_HASH SHA256=a62cf1ff7d6d2f82b90a2d247a57e9eb56b81c03feb1f372a53923426e358cb0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(quickjs_ng)

add_library(rime_js STATIC
  src/host.cpp
  src/runtime.cpp
  src/module_registry.cpp
  src/subscriptions.cpp
  src/timer.cpp
  src/abi.cpp
)
target_include_directories(rime_js PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rime_js PUBLIC rime_core qjs)
target_compile_features(rime_js PUBLIC cxx_std_20)
