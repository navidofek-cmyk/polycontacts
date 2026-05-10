cmake_minimum_required(VERSION 3.20)
project(contacts_cpp CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.14.3)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

# libpqxx ze zdroje — vyhne se ABI mismatch systémového balíčku
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SKIP_PQXX_SHARED ON CACHE BOOL "" FORCE)
FetchContent_Declare(libpqxx
  GIT_REPOSITORY https://github.com/jtv/libpqxx.git
  GIT_TAG 7.9.2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libpqxx)

add_executable(contacts_server src/main.cpp)
target_link_libraries(contacts_server PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    pqxx
    pthread)
target_compile_options(contacts_server PRIVATE -Wall -O2)

add_executable(test_contacts src/test_contacts.cpp)
target_link_libraries(test_contacts PRIVATE nlohmann_json::nlohmann_json pthread)
target_compile_options(test_contacts PRIVATE -Wall -O2)
