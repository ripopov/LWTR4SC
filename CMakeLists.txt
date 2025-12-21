###############################################################################
# 
###############################################################################
cmake_minimum_required(VERSION 3.20)
project(lwtr4sc VERSION 1.0.0)

if(CMAKE_PROJECT_NAME STREQUAL "lwtr4sc")
  include(FetchContent)
  set(FMT_INSTALL ON CACHE BOOL "Enable fmt install")
  FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 9.1.0
  )
  FetchContent_MakeAvailable(fmt)
  FetchContent_Declare(lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    SOURCE_SUBDIR build/cmake
  )
  set(LZ4_BUILD_CLI OFF CACHE BOOL "")
  set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "")
  FetchContent_MakeAvailable(lz4)
  # LZ4 creates lz4_static target, but we need lz4::lz4 for consistency
  if(NOT TARGET lz4::lz4 AND TARGET lz4_static)
    add_library(lz4::lz4 ALIAS lz4_static)
  endif()
  FetchContent_Declare(
      scp_git
      GIT_REPOSITORY  "https://github.com/accellera-official/systemc-common-practices.git"
      GIT_TAG         "origin/main"
      GIT_SHALLOW    ON
      UPDATE_DISCONNECTED NOT ${UPDATE_SCC_SRC} # When enabled, this option causes the update step to be skipped.
  )
  FetchContent_Populate(scp_git)
endif()


add_subdirectory(${scp_git_SOURCE_DIR}/report ${CMAKE_BINARY_DIR}/scp)
add_subdirectory(src)
add_subdirectory(example)
add_subdirectory(${CMAKE_SOURCE_DIR}/test EXCLUDE_FROM_ALL)
