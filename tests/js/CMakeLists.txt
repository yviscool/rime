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
  # Residency (AHK Persistent) fixtures: the second argument is the
  # runtime.exit() code the bundle must come back with, so 0 here means the
  # pump saw residency, kept the script alive, and the guard flag was
  # cleared before the main check. Same desktop env as the exit tests.
  add_test(NAME quickjs_persist_force
           COMMAND rime_js_bundle ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/persist-force.mjs 0)
  add_test(NAME quickjs_persist_hotkey
           COMMAND rime_js_bundle ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/persist-hotkey.mjs 0)
  add_test(NAME quickjs_persist_timer
           COMMAND rime_js_bundle ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/persist-timer.mjs 0)
  # Golden failure layer as a QuickJS consumer: the generated fixture lists
  # every contracts/golden failure entry as executed or skipped-with-reason
  # (bun tools/golden-fixture.ts) and asserts the executed ones. --production
  # grants production_capabilities(), so a capability denial can never be what
  # the assertions observe. Same desktop env as the bundle tests above.
  add_test(NAME quickjs_golden_failures
           COMMAND rime_js_bundle --production
                   ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden-violations.generated.mjs)
  # Rejection contract for the `unsupported-by-policy` rows (M8 item 3): the
  # production module surface must expose none of the 36 stdlib.md §5 /
  # future-runtime §10 names (25 core-builtins + 11 coverage), and the
  # alternatives §2.4 documents instead
  # must exist. --production keeps a capability grant out of the assertion,
  # the golden fixture's argument order, and its desktop environment.
  add_test(NAME quickjs_policy_refusal
           COMMAND rime_js_bundle --production
                   ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/policy-refusal.mjs)
  # M6 batch 1 grant: production_capabilities() must really carry ui.create,
  # proved by a real (invisible) rime:ui call under --production. Same
  # desktop env as the bundle tests above.
  add_test(NAME quickjs_ui_grant
           COMMAND rime_js_bundle --production
                   ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ui-grant.mjs)
  # Reload (AHK Reload): the harness returns the code the reloaded pass
  # asked for, so the expected code proves the host loop re-read the file and
  # evaluated it a second time in a fresh runtime (count carried across).
  add_test(NAME quickjs_reload_basic
           COMMAND rime_js_bundle ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reload-basic.mjs 5)
  # Exit outranks a reload that is already being torn down: the teardown
  # handler observed reason "reload" (4) instead of anything else (6), and no
  # second pass ran (9). Same desktop env as quickjs_bundle_smoke.
  add_test(NAME quickjs_reload_exit_priority
           COMMAND rime_js_bundle
                   ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reload-exit-priority.mjs 4)
  # Exit first: a reload attempted from the teardown of a pending exit is
  # refused, so the requested code (4) stands and no second pass ran (9).
  add_test(NAME quickjs_reload_exit_first
           COMMAND rime_js_bundle
                   ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/reload-exit-first.mjs 4)
  # Reload slice: production host loop (reload ceiling + on-disk replacement
  # + handshake), the refusal boundary and the reload flag, all in one binary.
  add_executable(rime_reload_slice reload_slice.cpp)
  target_link_libraries(rime_reload_slice PRIVATE rime_js rime_win32_js rime_action rime_test_support)
  target_compile_features(rime_reload_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_reload_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_reload_slice
           COMMAND rime_reload_slice ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
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

  add_executable(rime_control_slice control_slice.cpp)
  target_link_libraries(rime_control_slice PRIVATE rime_js rime_win32 rime_win32_js rime_test_support comctl32)
  target_compile_features(rime_control_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_control_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_control_slice COMMAND rime_control_slice)

  add_executable(rime_registry_slice registry_slice.cpp)
  target_link_libraries(rime_registry_slice PRIVATE rime_js rime_win32_js rime_test_support)
  target_compile_features(rime_registry_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_registry_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_registry_slice COMMAND rime_registry_slice)

  add_executable(rime_screen_slice screen_slice.cpp)
  target_link_libraries(rime_screen_slice PRIVATE rime_js rime_win32_js rime_test_support)
  target_compile_features(rime_screen_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_screen_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_screen_slice COMMAND rime_screen_slice)

  add_executable(rime_sound_slice sound_slice.cpp)
  target_link_libraries(rime_sound_slice PRIVATE rime_js rime_win32_js rime_test_support)
  target_compile_features(rime_sound_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_sound_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_sound_slice COMMAND rime_sound_slice)

  # M6 batch 1: the rime:ui module through the JS promise pipeline. Real
  # modal dialogs (watcher proves the #32770 window appeared), real
  # tooltip windows and the real tray icon, plus the capability-less gate.
  # Needs the same interactive desktop as the bundle/window tests.
  add_executable(rime_ui_slice ui_slice.cpp)
  # Same manifest as rime_host: ToolTip, MsgBox/InputBox and the tray paths
  # this slice proves at L5 must run in the configuration the host ships.
  rime_embed_comctl32_v6(rime_ui_slice)
  target_link_libraries(rime_ui_slice PRIVATE rime_js rime_win32_js rime_test_support)
  target_compile_features(rime_ui_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_ui_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_ui_slice COMMAND rime_ui_slice)

  # M6 batch 2: the Gui/GuiControl object family behind rime:ui. Real window
  # materialization, real Win32 cross-checks (GetWindowTextW / GetWindowRect
  # against the mirror), Resize and Close driven by this test's own
  # SetWindowPos and PostMessageW, and the Destroy / capability teardown
  # paths. Same interactive desktop requirement as the ui/window slices.
  add_executable(rime_gui_object_slice gui_object_slice.cpp)
  # ComCtl32 v6 for this process: EM_SETCUEBANNER (GuiControl.SetCue) is
  # only honoured when the host declares it, so the slice has to run in the
  # same manifest the production host ships or the cue observation below
  # would be measuring nothing.
  rime_embed_comctl32_v6(rime_gui_object_slice)
  target_link_libraries(rime_gui_object_slice PRIVATE rime_js rime_win32_js rime_test_support)
  target_compile_features(rime_gui_object_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_gui_object_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_gui_object_slice COMMAND rime_gui_object_slice)

  add_executable(rime_storage_slice storage_slice.cpp)
  target_link_libraries(rime_storage_slice PRIVATE rime_js rime_win32_js rime_test_support ole32)
  target_compile_features(rime_storage_slice PRIVATE cxx_std_20)
  if (MINGW)
    target_link_options(rime_storage_slice PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME quickjs_storage_slice COMMAND rime_storage_slice)
endif()
