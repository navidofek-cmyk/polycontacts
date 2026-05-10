cmake_minimum_required(VERSION 3.20)
project(contacts CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(contacts
    src/main.cpp
    src/contact_book.cpp
)

target_include_directories(contacts PRIVATE src)
target_compile_options(contacts PRIVATE -Wall -Wextra)
