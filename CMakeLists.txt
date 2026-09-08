cmake_minimum_required(VERSION 3.20)
project(softsim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# This project does not use C++ named modules. Disable dependency scanning so
# third-party targets are compiled directly by the configured CMake toolchain.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Skip updating the external dependencies after populating them for the first time")

# 开启编译器的链接时间优化（LTO）
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# ===============================================
# third-party dependencies
# ===============================================

# fmt, dependency of ramulator2
message("Configuring fmt...")
FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        12.0.0
  SOURCE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/ext/fmt
)
FetchContent_MakeAvailable(fmt)
message("Done configuring fmt.")

# yaml-cpp, dependency of ramulator2
message("Configuring yaml-cpp...")
option(YAML_CPP_BUILD_CONTRIB "Enable yaml-cpp contrib in library" OFF)
option(YAML_CPP_BUILD_TOOLS "Enable parse tools" OFF)
option(YAML_BUILD_SHARED_LIBS "Build yaml-cpp as a shared library" OFF)
FetchContent_Declare(
  yaml-cpp                             
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        yaml-cpp-0.9.0                        
  SOURCE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/ext/yaml-cpp
)
FetchContent_MakeAvailable(yaml-cpp)
message("Done configuring yaml-cpp.")

# ramulator2
message("Configuring ramulator2...")
set(RAMULATOR_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  ramulator2
  GIT_REPOSITORY https://github.com/CMU-SAFARI/ramulator2.git
  GIT_TAG        v2.1.0
  SOURCE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/ext/ramulator2
)
FetchContent_MakeAvailable(ramulator2)
target_compile_options(
  ramulator-controller
  PRIVATE
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-self-assign-field>"
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-uninitialized;-Wno-maybe-uninitialized>"
)
set_target_properties(ramulator PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY  ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)
message("Done configuring ramulator2.")

# ===============================================
# My code
# ===============================================

add_compile_options(-Wall -Wextra)

add_subdirectory(modules/hello)
