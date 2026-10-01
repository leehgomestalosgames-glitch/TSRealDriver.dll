cmake_minimum_required(VERSION 3.24)
project(TSRealDriver VERSION 0.1.0 LANGUAGES CXX)

add_library(TSRealDriver SHARED src/plugin.cpp)
target_compile_features(TSRealDriver PRIVATE cxx_std_20)
target_compile_definitions(TSRealDriver PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(TSRealDriver PROPERTIES OUTPUT_NAME "TSRealDriver")
