cmake_minimum_required(VERSION 3.24)
project(TSRealDriver VERSION 0.3.0 LANGUAGES CXX)

add_library(TSRealDriver SHARED\n    src/plugin.cpp\n    src/native_game_bridge.cpp\n)
target_compile_features(TSRealDriver PRIVATE cxx_std_20)
target_compile_definitions(TSRealDriver PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(TSRealDriver PRIVATE user32 gdi32 winmm)
set_target_properties(TSRealDriver PROPERTIES OUTPUT_NAME "TSRealDriver")

add_executable(TSRealDriverConfig WIN32 src/config_editor.cpp)
target_compile_features(TSRealDriverConfig PRIVATE cxx_std_20)
target_compile_definitions(TSRealDriverConfig PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(TSRealDriverConfig PRIVATE shell32 user32 gdi32)
