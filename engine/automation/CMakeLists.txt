add_library(rime_automation STATIC
  src/uia_service.cpp
  src/uia_executor.cpp
)
target_include_directories(rime_automation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rime_automation PUBLIC rime_core rime_action)
target_compile_features(rime_automation PUBLIC cxx_std_20)
target_compile_definitions(rime_automation PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(rime_automation PRIVATE ole32 oleaut32)
