cmake_minimum_required(VERSION 3.16)

set(HUNTER_PACKAGES abseil GTest gflags)
include(FetchContent)
FetchContent_Declare(SetupHunter GIT_REPOSITORY https://github.com/cpp-pm/gate)
FetchContent_MakeAvailable(SetupHunter)

project(Atlas LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror -O3")

find_package(absl CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(gflags CONFIG REQUIRED)

set(JEMALLOC_LIB_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/jemalloc/lib/")
add_subdirectory(third_party)
link_directories(${JEMALLOC_LIB_PATH})

include_directories(bks_module/include)
include_directories(runtime/include)
include_directories(third_party/jemalloc/include)

add_subdirectory(runtime)
add_subdirectory(tests)
