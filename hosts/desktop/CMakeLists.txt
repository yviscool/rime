add_executable(rime_host src/main.cpp src/host.cpp)
target_include_directories(rime_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rime_host PRIVATE rime_action rime_core)
target_compile_features(rime_host PRIVATE cxx_std_20)

# Script mode wires the full production stack (services, kernel policy, the
# 15 action executors and the five JS modules) through rime::win32::Bootstrap;
# it only exists when the QuickJS host and the Win32 JS modules are built.
if (WIN32 AND TARGET rime_win32_js)
  target_link_libraries(rime_host PRIVATE rime_win32_js rime_js)
  target_compile_definitions(rime_host PRIVATE RIME_DESKTOP_HAS_SCRIPT=1)
  if (MINGW)
    # The script path pulls in futures and the service threads; link the
    # runtime statically so the host never depends on a MinGW DLL on PATH.
    target_link_options(rime_host PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  if (RIME_BUILD_TESTS)
    # The production host runs a script through the same lifecycle the
    # bundle harness uses; ping.mjs proves runtime start/evaluate/settle/stop.
    add_test(NAME quickjs_desktop_script
             COMMAND rime_host
                     ${CMAKE_SOURCE_DIR}/tests/js/fixtures/ping.mjs)
  endif()
endif()
