add_library(rime_core STATIC
  src/event_queue.cpp
  src/error_names.cpp
  src/json.cpp
  src/lane.cpp
  src/runtime.cpp
  src/shutdown.cpp
)

target_include_directories(rime_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(rime_core PUBLIC cxx_std_20)
