cmake_minimum_required(VERSION 3.26)
project(uring_file_server C)

set(CMAKE_C_STANDARD 11)

add_executable(uring_file_server main.c)
target_link_libraries(uring_file_server uring)
