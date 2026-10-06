add_library(rime_win32_js STATIC
  src/window_module.cpp
  src/input_module.cpp
  src/events_module.cpp
  src/process_module.cpp
  src/clipboard_module.cpp
  src/registry_module.cpp
  src/screen_module.cpp
  src/storage_module.cpp
  src/automation_module.cpp
  src/bootstrap.cpp
)
target_include_directories(rime_win32_js PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rime_win32_js PUBLIC rime_win32 rime_automation rime_js rime_action)
target_compile_features(rime_win32_js PUBLIC cxx_std_20)
