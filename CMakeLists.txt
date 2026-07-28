cmake_minimum_required(VERSION 3.13)

project(mii-transfer-3ds CXX)

# The devkitPro 3DS toolchain must be active; bail early with a clear message
# rather than failing later on a missing ctr_create_3dsx().
if(NOT NINTENDO_3DS)
    message(FATAL_ERROR
        "This is not being configured with the devkitPro 3DS toolchain. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/3DS.cmake")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Generate compile_commands.json.

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# -----------------------------------------------------------------------------
# App metadata shown in the Homebrew Launcher / smdh.
# -----------------------------------------------------------------------------
set(APP_NAME "Mii Transfer (3DS)")
set(APP_DESCRIPTION "Mii Transfer (3DS) - Transfer Miis to other devices")
set(APP_AUTHOR "jamesrhg/Arian Kordi")

# -----------------------------------------------------------------------------
# Dependencies. citro2d/citro3d/ctru ship as plain .a libs under libctru's own
# lib/include dirs (already on the 3DS toolchain's default search path - see
# Platform/Nintendo3DS.cmake's CTR_LINKER_FLAGS/CTR_STANDARD_INCLUDE_DIRECTORIES),
# so they're linked by name directly. libpng (for decoding the bundled PNG
# into a citro2d texture at runtime - see source/png_texture.h) is a
# devkitPro portlib with its own .pc file, resolved via pkg-config like the
# Wii U build's SDL2 dep was.
#
# No libcurl/mbedtls (3ds-curl/3ds-mbedtls) anymore: source/mii_image_fetch.cpp
# now uses libctru's own httpc.h against plain http:// (not https://) - see
# that file's own header comment for why httpc, previously ruled out for
# HTTPS specifically, is fine here now that there's no TLS handshake to fail
# in the first place. httpc ships as part of ctru itself, so this needs no
# extra portlib/pkg-config entry at all.
# -----------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

# -----------------------------------------------------------------------------
# Sources - an explicit list, not a glob of source/*.cpp, while this app is
# floored down to just CFL_DB/Ver3 parsing, PNG decompression, the
# top-screen portrait preview (image fetcher + AC connect), ACT (current
# user's Mii name only) and FRD friends, the minimal list UI, the local HTTP
# server, and SFX/BGM (ndsp) needed to exercise all of that - see main.cpp's
# own comment.
# -----------------------------------------------------------------------------
set(SOURCES
    "${CMAKE_SOURCE_DIR}/source/animated_bg.cpp"
    "${CMAKE_SOURCE_DIR}/source/base64.cpp"
    "${CMAKE_SOURCE_DIR}/source/ctr_account.cpp"
    "${CMAKE_SOURCE_DIR}/source/ctr_friends.cpp"
    "${CMAKE_SOURCE_DIR}/source/ctr_log.cpp"
    "${CMAKE_SOURCE_DIR}/source/ctr_mii_db.cpp"
    "${CMAKE_SOURCE_DIR}/source/ctr_network.cpp"
    "${CMAKE_SOURCE_DIR}/source/ctr_thread.cpp"
    "${CMAKE_SOURCE_DIR}/source/ctr_ui.cpp"
    "${CMAKE_SOURCE_DIR}/source/main.cpp"
    "${CMAKE_SOURCE_DIR}/source/mii_detail_panel.cpp"
    "${CMAKE_SOURCE_DIR}/source/mii_http_server.cpp"
    "${CMAKE_SOURCE_DIR}/source/mii_image_fetch.cpp"
    "${CMAKE_SOURCE_DIR}/source/png_texture.cpp"
    "${CMAKE_SOURCE_DIR}/source/sfx.cpp"
    "${CMAKE_SOURCE_DIR}/source/ver3_mii.cpp"
)

add_executable(mii-transfer ${SOURCES})
target_include_directories(mii-transfer PRIVATE "${CMAKE_SOURCE_DIR}/source")

target_compile_options(mii-transfer PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-missing-field-initializers
)

# Private Mii-decode library, vendored unmodified from the Wii U build (it's
# already platform-agnostic/CTR-aware - see source/lib/DbAndCreateId.hpp).
file(GLOB SOURCES_LIB CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/source/lib/*.cpp")
add_library(mii-library STATIC ${SOURCES_LIB})
target_compile_options(mii-library PRIVATE -w) # Ignore warnings (GCC-only) - generated code.
target_include_directories(mii-library PUBLIC "${CMAKE_SOURCE_DIR}/source/lib")

target_link_libraries(mii-transfer PRIVATE
    mii-library

    citro2d
    citro3d

    PkgConfig::PNG

    ctru
)

# -----------------------------------------------------------------------------
# Package the .elf into a .3dsx (the Homebrew Launcher format, direct 3DS
# analogue of the Wii U build's .wuhb) with romfs/ embedded (mounted at
# romfs:/ at runtime via romfsInit(), replacing the Wii U build's
# /vol/content/) and an smdh built from meta/icon-48x48.png.
# -----------------------------------------------------------------------------
ctr_generate_smdh(mii-transfer.smdh
    NAME "${APP_NAME}"
    DESCRIPTION "${APP_DESCRIPTION}"
    AUTHOR "${APP_AUTHOR}"
    ICON "${CMAKE_SOURCE_DIR}/meta/icon-48x48.png"
)

ctr_create_3dsx(mii-transfer
    SMDH "${CMAKE_CURRENT_BINARY_DIR}/mii-transfer.smdh"
    ROMFS "${CMAKE_SOURCE_DIR}/romfs"
)
