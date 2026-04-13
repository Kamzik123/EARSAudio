cmake_minimum_required(VERSION 3.15)
project(ears LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_library(ears SHARED src/ears.c src/ealayer3.c)
target_include_directories(ears PUBLIC include)
target_include_directories(ears PRIVATE third_party)
target_compile_definitions(ears PRIVATE EARS_BUILD_DLL)

add_executable(ears_cli src/cli.c)
target_link_libraries(ears_cli PRIVATE ears)

set_target_properties(ears PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set_target_properties(ears_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
