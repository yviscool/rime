add_library(rime_action STATIC src/codec.cpp src/kernel.cpp src/dispatcher.cpp)
target_include_directories(rime_action PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rime_action PUBLIC rime_core)
target_compile_features(rime_action PUBLIC cxx_std_20)
