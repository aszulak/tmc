cmake_minimum_required(VERSION 3.10)
project(tmc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Threads REQUIRED)

add_library(tmclib SHARED tmclib.c)
set_target_properties(tmclib PROPERTIES OUTPUT_NAME tmc)
target_link_libraries(tmclib PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

add_executable(tmc tmc.c)
