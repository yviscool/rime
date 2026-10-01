add_executable(rime_host src/main.cpp src/host.cpp)
target_include_directories(rime_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rime_host PRIVATE rime_action rime_core)
target_compile_features(rime_host PRIVATE cxx_std_20)
