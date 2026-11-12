cmake_minimum_required(VERSION 3.16)
project(rmexe_mobile_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
    set(SDL2_INCLUDE_DIRS "${SDL2_INCLUDE_DIR}")
    set(SDL2_LIBRARIES "${SDL2_LIBRARY}")
else()
    find_package(SDL2 REQUIRED)
endif()

if(ZLIB_SOURCE_DIR)
    add_library(zlib_bundled STATIC
        "${ZLIB_SOURCE_DIR}/adler32.c"
        "${ZLIB_SOURCE_DIR}/compress.c"
        "${ZLIB_SOURCE_DIR}/crc32.c"
        "${ZLIB_SOURCE_DIR}/deflate.c"
        "${ZLIB_SOURCE_DIR}/gzclose.c"
        "${ZLIB_SOURCE_DIR}/gzlib.c"
        "${ZLIB_SOURCE_DIR}/gzread.c"
        "${ZLIB_SOURCE_DIR}/gzwrite.c"
        "${ZLIB_SOURCE_DIR}/infback.c"
        "${ZLIB_SOURCE_DIR}/inffast.c"
        "${ZLIB_SOURCE_DIR}/inflate.c"
        "${ZLIB_SOURCE_DIR}/inftrees.c"
        "${ZLIB_SOURCE_DIR}/trees.c"
        "${ZLIB_SOURCE_DIR}/uncompr.c"
        "${ZLIB_SOURCE_DIR}/zutil.c"
    )
    target_include_directories(zlib_bundled PUBLIC "${ZLIB_SOURCE_DIR}")
    if(NOT MSVC)
        target_compile_options(zlib_bundled PRIVATE -O2 -w)
    endif()
    set(RMEXE_ZLIB_INCLUDE_DIRS "${ZLIB_SOURCE_DIR}")
    set(RMEXE_ZLIB_LIBS zlib_bundled)
else()
    find_package(ZLIB REQUIRED)
    set(RMEXE_ZLIB_INCLUDE_DIRS "${ZLIB_INCLUDE_DIRS}")
    set(RMEXE_ZLIB_LIBS ZLIB::ZLIB)
endif()

# Core DoJa + CLDC Native Runtime library
add_library(doja_runtime STATIC
    runtime/doja_runtime.cpp
)
target_include_directories(doja_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${SDL2_INCLUDE_DIRS}
    ${RMEXE_ZLIB_INCLUDE_DIRS}
)
if(MSVC)
    target_compile_options(doja_runtime PRIVATE /O2 /W3 /utf-8)
else()
    target_compile_options(doja_runtime PRIVATE -O2 -Wall -Wextra)
endif()

target_link_libraries(doja_runtime PUBLIC
    ${SDL2_LIBRARIES}
    ${RMEXE_ZLIB_LIBS}
)
if(WIN32)
    target_link_libraries(doja_runtime PUBLIC ws2_32)
else()
    target_link_libraries(doja_runtime PUBLIC ${CMAKE_DL_LIBS})
endif()

# Unified Capcom i-mode + PvP NetBattle Server (TCP HTTP/1.1 + UDP Relay on single port)
add_executable(rmexe_server
    server/rmexe_server.cpp
)
target_include_directories(rmexe_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)
if(MSVC)
    target_compile_options(rmexe_server PRIVATE /O2 /W3 /utf-8)
else()
    target_compile_options(rmexe_server PRIVATE -O2 -Wall -Wextra)
endif()
if(WIN32)
    target_link_libraries(rmexe_server PRIVATE ws2_32)
endif()

# Helper macro to register a static recompilation target if its user-supplied JAR is present
macro(add_doja_recomp_target TARGET_NAME JAR_REL_PATH SP_REL_PATH SAV_REL_PATH WIN_TITLE)
    set(JAR_ABS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${JAR_REL_PATH}")
    set(GEN_CPP_PATH "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_gen.cpp")

    if(EXISTS "${JAR_ABS_PATH}")
        add_custom_command(
            OUTPUT "${GEN_CPP_PATH}"
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp_jar.py"
                    "${JAR_ABS_PATH}" -o "${GEN_CPP_PATH}"
            DEPENDS "${JAR_ABS_PATH}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp_jar.py"
            COMMENT "Statically recompiling ${JAR_REL_PATH} -> ${TARGET_NAME}_gen.cpp"
            VERBATIM
        )

        add_executable(${TARGET_NAME}
            "${GEN_CPP_PATH}"
            runtime/main.cpp
        )
        if(MSVC)
            set_source_files_properties("${GEN_CPP_PATH}" PROPERTIES COMPILE_FLAGS "/O1 /utf-8 /bigobj")
            target_compile_options(${TARGET_NAME} PRIVATE /utf-8)
        else()
            set_source_files_properties("${GEN_CPP_PATH}" PROPERTIES COMPILE_FLAGS "-O1")
        endif()
        if(WIN32 AND NOT MSVC)
            target_link_options(${TARGET_NAME} PRIVATE -mwindows)
        endif()
        target_compile_definitions(${TARGET_NAME} PRIVATE
            DEFAULT_JAR_PATH="${JAR_REL_PATH}"
            DEFAULT_SP_PATH="${SP_REL_PATH}"
            DEFAULT_SAV_PATH="${SAV_REL_PATH}"
            DEFAULT_WINDOW_TITLE="${WIN_TITLE}"
        )
        target_link_libraries(${TARGET_NAME} PRIVATE doja_runtime)
        message(STATUS "Enabled recompilation target: ${TARGET_NAME} (${JAR_REL_PATH})")
    else()
        message(STATUS "Skipping ${TARGET_NAME} (${JAR_REL_PATH} not found; place user-supplied assets to enable)")
    endif()
endmacro()

add_doja_recomp_target(
    pon_jp_recomp
    "assets/PoN_jp/bin/PoN.jar"
    "assets/PoN_jp/sp/PoN.sp"
    "assets/PoN_jp/sp/PoN.sav"
    "Rockman EXE: Phantom of Network (JP Recomp)"
)

add_doja_recomp_target(
    pon_en_recomp
    "assets/PoN/bin/PoN.jar"
    "assets/PoN/sp/PoN.sp"
    "assets/PoN/sp/PoN.sav"
    "MegaMan Battle Network: Phantom of Network (EN Recomp)"
)

add_doja_recomp_target(
    lon_jp_recomp
    "assets/LoN_jp/bin/LoN.jar"
    "assets/LoN_jp/sp/LoN.sp"
    "assets/LoN_jp/sp/LoN.sav"
    "Rockman EXE: Legend of Network (JP Recomp)"
)

add_doja_recomp_target(
    lon_en_recomp
    "assets/LoN/bin/LoN.jar"
    "assets/LoN/sp/LoN.sp"
    "assets/LoN/sp/LoN.sav"
    "MegaMan Battle Network: Legend of Network (EN Recomp)"
)
