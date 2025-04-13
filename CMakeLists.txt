cmake_minimum_required(VERSION 3.10)
project(vpn LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2)

add_executable(${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} PRIVATE include)

file(GLOB SOURCES "src/*.c")
target_sources(${PROJECT_NAME} PRIVATE ${SOURCES})
