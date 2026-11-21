cmake_minimum_required(VERSION 3.20)
project(rmexe_ios LANGUAGES C CXX OBJC OBJCXX)

get_filename_component(RMEXE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SDL2_SRC_DIR "${RMEXE_ROOT}/third_party/SDL2")

if(NOT EXISTS "${SDL2_SRC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SDL2 source not found at ${SDL2_SRC_DIR}. Download SDL2 2.30.x into third_party/SDL2 first.")
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
add_subdirectory("${SDL2_SRC_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sdl2_build" EXCLUDE_FROM_ALL)

set(RMEXE_BUILD_COLLECTION ON CACHE BOOL "" FORCE)
add_subdirectory("${RMEXE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/rmexe_build")
