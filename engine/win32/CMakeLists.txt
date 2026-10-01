add_library(rime_win32 STATIC
  src/ui_thread.cpp
  src/window.cpp
  src/window_executor.cpp
  src/input.cpp
  src/process.cpp
  src/process_executor.cpp
  src/clipboard.cpp
  src/clipboard_executor.cpp
)
target_include_directories(rime_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rime_win32 PUBLIC rime_core rime_action)
target_compile_features(rime_win32 PUBLIC cxx_std_20)
target_compile_definitions(rime_win32 PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(rime_win32 PRIVATE user32)
