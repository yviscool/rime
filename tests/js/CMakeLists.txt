add_executable(rime_js_smoke js_smoke.cpp)
target_link_libraries(rime_js_smoke PRIVATE rime_js rime_test_support)
target_compile_features(rime_js_smoke PRIVATE cxx_std_20)
if (MINGW)
  target_link_options(rime_js_smoke PRIVATE -static-libstdc++ -static-libgcc)
endif()
add_test(NAME quickjs_native_binding_smoke
         COMMAND rime_js_smoke ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

# The bundle harness registers the Win32 native modules the Rim bundle
# imports, so it exists only on Windows with rime_win32_js available.
if (WIN32 AND TARGET rime_win32_js)
  add_executable(rime_js_bundle js_bundle.cpp)
  target_link_libraries(rime_js_bundle PRIVATE rime_js rime_win32_js rime_action rime_test_support)
  target_compile_features(rime_js_bundle PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_js_bundle PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  # rime_js_bundle starts WindowService like the window slice tests, so it
  # needs the same desktop environment as the window tests below.
  add_test(NAME quickjs_bundle_smoke
           COMMAND rime_js_bundle ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ping.mjs)
  # The exit path needs the same desktop env as quickjs_bundle_smoke: the
  # harness still boots WindowService before the script runs. The second
  # argument is the runtime.exit() code the bundle must come back with.
  add_test(NAME quickjs_exit_basic
           COMMAND rime_js_bundle ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit-basic.mjs 7)
  add_test(NAME quickjs_exit_pending
           COMMAND rime_js_bundle ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/exit-pending.mjs 3)
endif()

add_executable(rime_js_runtime runtime_smoke.cpp)
target_link_libraries(rime_js_runtime PRIVATE rime_js rime_test_support)
target_compile_features(rime_js_runtime PRIVATE cxx_std_20)
if (MINGW)
  target_link_options(rime_js_runtime PRIVATE -static-libstdc++ -static-libgcc)
endif()
add_test(NAME quickjs_threaded_runtime_smoke COMMAND rime_js_runtime)

if (WIN32 AND TARGET rime_win32_js)
  add_executable(rime_vertical_slice vertical_slice.cpp)
  target_link_libraries(rime_vertical_slice PRIVATE rime_js rime_win32_js rime_test_support)
  target_compile_features(rime_vertical_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_vertical_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_vertical_slice COMMAND rime_vertical_slice)

  add_executable(rime_input_slice input_slice.cpp)
  target_link_libraries(rime_input_slice PRIVATE rime_js rime_win32_js rime_test_support)
  target_compile_features(rime_input_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_input_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_input_slice COMMAND rime_input_slice)

  add_executable(rime_events_slice events_slice.cpp)
  # imm32: the slice detaches the desktop IME from its edit control so the
  # typed hotstring trigger lands as ASCII instead of a pinyin composition.
  target_link_libraries(rime_events_slice PRIVATE rime_js rime_win32_js rime_action rime_test_support imm32)
  target_compile_features(rime_events_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_events_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_events_slice COMMAND rime_events_slice)

  add_executable(rime_breadth_slice breadth_slice.cpp)
  target_link_libraries(rime_breadth_slice PRIVATE rime_js rime_win32_js rime_test_support)
  target_compile_features(rime_breadth_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_breadth_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_breadth_slice COMMAND rime_breadth_slice)

  add_executable(rime_automation_slice automation_slice.cpp)
  target_link_libraries(rime_automation_slice PRIVATE rime_js rime_automation rime_win32_js rime_test_support)
  target_compile_features(rime_automation_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_automation_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_automation_slice COMMAND rime_automation_slice)
endif()
