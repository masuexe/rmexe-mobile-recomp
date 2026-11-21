cmake_minimum_required(VERSION 3.16)
project(rmexe_mobile_recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(TARGET SDL2::SDL2-static AND IOS)
    set(SDL2_INCLUDE_DIRS "")
    set(SDL2_LIBRARIES SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    set(SDL2_INCLUDE_DIRS "")
    set(SDL2_LIBRARIES SDL2::SDL2)
elseif(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
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

if(NOT ANDROID AND NOT IOS)
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
macro(add_doja_recomp_target TARGET_NAME JAR_REL_PATH SP_REL_PATH WIN_TITLE)
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
    "Rockman EXE: Phantom of Network (JP Recomp)"
)

add_doja_recomp_target(
    pon_en_recomp
    "assets/PoN/bin/PoN.jar"
    "assets/PoN/sp/PoN.sp"
    "MegaMan Battle Network: Phantom of Network (EN Recomp)"
)

add_doja_recomp_target(
    lon_jp_recomp
    "assets/LoN_jp/bin/LoN.jar"
    "assets/LoN_jp/sp/LoN.sp"
    "Rockman EXE: Legend of Network (JP Recomp)"
)

add_doja_recomp_target(
    lon_en_recomp
    "assets/LoN/bin/LoN.jar"
    "assets/LoN/sp/LoN.sp"
    "MegaMan Battle Network: Legend of Network (EN Recomp)"
)
endif() # NOT ANDROID AND NOT IOS

# Unified All-in-One Collection target (Android libmain.so / iOS / Desktop launcher)
if(ANDROID OR IOS)
    set(RMEXE_COLLECTION_DEFAULT ON)
else()
    set(RMEXE_COLLECTION_DEFAULT OFF)
endif()
option(RMEXE_BUILD_COLLECTION "Build unified all-in-one PoN+LoN collection target" ${RMEXE_COLLECTION_DEFAULT})

if(RMEXE_BUILD_COLLECTION)
    set(COLLECTION_GEN_SRCS "")
    set(COLLECTION_DEFS "")

    macro(add_collection_game_unit GAME_NS DEF_MACRO JAR_REL_PATH)
        set(_JAR_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${JAR_REL_PATH}")
        set(_GEN_CPP "${CMAKE_CURRENT_BINARY_DIR}/col_${GAME_NS}_gen.cpp")
        if(EXISTS "${_JAR_ABS}")
            add_custom_command(
                OUTPUT "${_GEN_CPP}"
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp_jar.py"
                        "${_JAR_ABS}" -o "${_GEN_CPP}" --namespace "${GAME_NS}"
                DEPENDS "${_JAR_ABS}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp_jar.py"
                COMMENT "Statically recompiling ${JAR_REL_PATH} (namespace ${GAME_NS}) -> col_${GAME_NS}_gen.cpp"
                VERBATIM
            )
            if(MSVC)
                set_source_files_properties("${_GEN_CPP}" PROPERTIES COMPILE_FLAGS "/O1 /utf-8 /bigobj")
            else()
                set_source_files_properties("${_GEN_CPP}" PROPERTIES COMPILE_FLAGS "-O1")
            endif()
            list(APPEND COLLECTION_GEN_SRCS "${_GEN_CPP}")
            list(APPEND COLLECTION_DEFS "${DEF_MACRO}=1")
        endif()
    endmacro()

    add_collection_game_unit(game_pon_jp RMEXE_HAVE_PON_JP "assets/PoN_jp/bin/PoN.jar")
    add_collection_game_unit(game_pon_en RMEXE_HAVE_PON_EN "assets/PoN/bin/PoN.jar")
    add_collection_game_unit(game_lon_jp RMEXE_HAVE_LON_JP "assets/LoN_jp/bin/LoN.jar")
    add_collection_game_unit(game_lon_en RMEXE_HAVE_LON_EN "assets/LoN/bin/LoN.jar")

    if(COLLECTION_GEN_SRCS)
        if(ANDROID)
            add_library(main SHARED
                ${COLLECTION_GEN_SRCS}
                runtime/collection_main.cpp
            )
            target_compile_definitions(main PRIVATE ${COLLECTION_DEFS})
            target_link_libraries(main PRIVATE doja_runtime log android)
            message(STATUS "Enabled Android unified collection library: libmain.so (${COLLECTION_DEFS})")
        elseif(IOS)
            add_executable(RMEXECollection MACOSX_BUNDLE
                ${COLLECTION_GEN_SRCS}
                runtime/collection_main.cpp
            )
            target_compile_definitions(RMEXECollection PRIVATE ${COLLECTION_DEFS})
            set_target_properties(RMEXECollection PROPERTIES
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist"
                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.rmexe.recomp"
                XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "13.0"
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
                XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
                XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
            )
            target_link_libraries(RMEXECollection PRIVATE
                doja_runtime
                "-framework AudioToolbox"
                "-framework AVFoundation"
                "-framework CoreAudio"
                "-framework CoreGraphics"
                "-framework CoreHaptics"
                "-framework CoreMotion"
                "-framework Foundation"
                "-framework GameController"
                "-framework Metal"
                "-framework OpenGLES"
                "-framework QuartzCore"
                "-framework UIKit"
            )
            message(STATUS "Enabled iOS unified collection bundle: RMEXECollection.app (${COLLECTION_DEFS})")
        else()
            add_executable(rmexe_collection
                ${COLLECTION_GEN_SRCS}
                runtime/collection_main.cpp
            )
            target_compile_definitions(rmexe_collection PRIVATE ${COLLECTION_DEFS})
            target_link_libraries(rmexe_collection PRIVATE doja_runtime)
            message(STATUS "Enabled unified collection target: rmexe_collection (${COLLECTION_DEFS})")
        endif()
    else()
        message(WARNING "RMEXE_BUILD_COLLECTION=ON but no .jar assets found under assets/")
    endif()
endif()
