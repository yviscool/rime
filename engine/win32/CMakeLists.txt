add_library(rime_win32 STATIC
  src/ui_thread.cpp
  src/window.cpp
  src/window_groups.cpp
  src/window_set.cpp
  src/window_at.cpp
  src/window_foreground.cpp
  src/window_parse.cpp
  src/window_geometry.cpp
  src/window_match.cpp
  src/window_executor.cpp
  src/window_control.cpp
  src/control_executor.cpp
  src/input.cpp
  src/input_executor.cpp
  src/process.cpp
  src/process_executor.cpp
  src/clipboard.cpp
  src/clipboard_executor.cpp
  src/context_watcher.cpp
  src/registry.cpp
  src/registry_executor.cpp
  src/screen.cpp
  src/screen_pixels.cpp
  src/sound.cpp
  src/screen_seam.cpp
  src/image_loader.cpp
  src/storage.cpp
  src/storage_executor.cpp
)
target_include_directories(rime_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rime_win32 PUBLIC rime_core rime_action)
target_compile_features(rime_win32 PUBLIC cxx_std_20)
target_compile_definitions(rime_win32 PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# gdiplus: image_loader decodes the ImageSearch needle; it ships with Windows
# and adds no third-party dependency.
target_link_libraries(rime_win32 PRIVATE user32 gdi32 gdiplus)
# Registry ops are advapi32; PRIVATE on a static library still propagates the
# link line to every consumer (tests included), so the test's own raw Win32
# reads resolve too.
target_link_libraries(rime_win32 PRIVATE advapi32)
# Adapter addresses for SysGetIPAddresses (GetAdaptersAddresses), read by the
# service and cross-checked by the screen test against GetIpAddrTable.
target_link_libraries(rime_win32 PRIVATE iphlpapi)
# winmm: SoundBeep (Beep) and SoundPlay (mciSendString/mciGetErrorString) go
# through the multimedia API; MessageBeep resolves in user32, already linked.
target_link_libraries(rime_win32 PRIVATE winmm)
# Storage service: WinHTTP download, shell recycle/shortcut/eject and the
# dialog/shortcut GUIDs (CLSID_FileOpenDialog, CLSID_ShellLink, ...).
target_link_libraries(rime_win32 PRIVATE winhttp shell32 ole32 uuid)
