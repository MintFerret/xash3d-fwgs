cmake_minimum_required(VERSION 3.13)
project(xash3d C CXX)


# Level 2 is what this port can currently ship. It costs the menu art -
# mainui drops backgrounds, button bitmaps and sounds whenever this is
# nonzero - but 0 crashes on map load with a NULL callback in libogc's tick
# task, so something is corrupting memory once the engine runs at full
# limits. The protocol limits OGC uses are set unconditionally in
# protocol.h, so this does not affect server compatibility either way.
set(XASH_LOW_MEMORY "2" CACHE STRING "engine memory profile: 0 normal, 1 low engine limits, 2 low protocol limits")

find_package(PkgConfig REQUIRED)
find_package(SDL2 REQUIRED)
find_package(BZip2 REQUIRED)
pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
pkg_check_modules(OPENGL IMPORTED_TARGET opengl)

add_definitions(
    -DXASH_BUILD_COMMIT="1242"
    -DXASH_BUILD_BRANCH="ogc"
    -DXASH_BUILD_COMMIT_DATE="2026-05-12"
    -D_GNU_SOURCE=1
    -DALLOCA_H=<alloca.h>
    -DHAVE_TGMATH_H=1
    -DHAVE_STRNCASECMP=1
    -DHAVE_STRCASECMP=1
    -DHAVE_STRCASESTR=1
    -DHAVE_STRCHRNUL=1
    -DHAVE_STRLCPY=1
    -DHAVE_STRLCAT=1
    -DHAVE_STRNLEN=1
    -DCUSTOM_MODES=1
    -DDEFAULT_DEV=2
)
file(GLOB PUBLIC_SOURCES public/*)
list(REMOVE_ITEM PUBLIC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/public/build_vcs.c")
add_library(public STATIC ${PUBLIC_SOURCES})
target_include_directories(public PUBLIC 3rdparty/library_suffix/include common engine public)

add_subdirectory(3rdparty/opus/opus)

add_subdirectory(3rdparty/library_suffix)

file(GLOB MULTIEMU_SOURCES 3rdparty/MultiEmulator/src/*)
add_library(MultiEmulator STATIC ${MULTIEMU_SOURCES})
target_include_directories(MultiEmulator PRIVATE
	3rdparty/MultiEmulator/include
	3rdparty/library_suffix/include
	common
	engine
	engine/common
	engine/client
	public
	pm_shared
	filesystem
)

file(GLOB FILESYSTEM_SOURCES filesystem/*)
add_library(filesystem_stdio STATIC ${FILESYSTEM_SOURCES})
target_include_directories(filesystem_stdio PUBLIC
	3rdparty/library_suffix/include
	common
	engine
	engine/common
	engine/client
	public
	pm_shared
	filesystem
)
target_compile_definitions(filesystem_stdio PRIVATE
	-DXASH_GAMEDIR="valve"
	-DXASH_LOW_MEMORY=${XASH_LOW_MEMORY}
	-DSTDINT_H=<stdint.h>
	-DHAVE_STRNICMP=0
	-DHAVE_STRICMP=0
	-DXASH_STATIC_LIBS=1
)
target_compile_options(filesystem_stdio PRIVATE
 -fno-omit-frame-pointer -ffunction-sections -fno-asynchronous-unwind-tables
 -fno-exceptions -fpermissive
)

#cl_dll, dlls, menu goes here
include(makelibrary.txt)

# ------------------------------------------------------------------------------
# Renderer selection
# ------------------------------------------------------------------------------
set(XASH_RENDERER "gl" CACHE STRING "renderer to link: gl (GX) or soft (software)")
set_property(CACHE XASH_RENDERER PROPERTY STRINGS gl soft)

if(XASH_RENDERER STREQUAL "gl")
    set(XASH_REF_LIB ref_gx)
    set(XASH_REF_DEFINE XASH_REF_GL_ENABLED=1)
else()
    set(XASH_REF_LIB ref_soft)
    set(XASH_REF_DEFINE XASH_REF_SOFT_ENABLED=1)
endif()
message(STATUS "Renderer: ${XASH_REF_LIB}")

# ------------------------------------------------------------------------------
# GX renderer (for Wii)
# ------------------------------------------------------------------------------
file(GLOB REF_GX_SOURCES
    ref/common/*
    ref/gx/*
)
add_library(ref_gx STATIC ${REF_GX_SOURCES})

target_include_directories(ref_gx PUBLIC
    3rdparty/library_suffix/include
    ref/common
    ref/gx
    common
    engine
    engine/common
    engine/common/imagelib
    public
    pm_shared
    filesystem
)

target_compile_definitions(ref_gx PRIVATE
    -DXASH_LOW_MEMORY=${XASH_LOW_MEMORY}
    -DXASH_STATIC_LIBS=1
    -DREF_DLL=1
    -DXASH_GL_STATIC=1
)

target_compile_options(ref_gx PRIVATE
    -fpermissive
    -fno-exceptions
    -fno-strict-aliasing
)

if(XASH_ZEROINIT OR XASH_ZEROINIT_REF)
    target_compile_options(ref_gx PRIVATE -ftrivial-auto-var-init=zero)
endif()

target_link_libraries(ref_gx PUBLIC ogc)

# ------------------------------------------------------------------------------
# Software renderer (CPU rasteriser)
# ------------------------------------------------------------------------------
file(GLOB REF_SOFT_SOURCES
    ref/common/*
    ref/soft/*
)
add_library(ref_soft STATIC ${REF_SOFT_SOURCES})

target_include_directories(ref_soft PUBLIC
    3rdparty/library_suffix/include
    ref/common
    ref/soft
    common
    engine
    engine/common
    engine/common/imagelib
    public
    pm_shared
    filesystem
)

target_compile_definitions(ref_soft PRIVATE
    -DXASH_LOW_MEMORY=${XASH_LOW_MEMORY}
    -DXASH_STATIC_LIBS=1
    -DREF_DLL=1
)

target_compile_options(ref_soft PRIVATE
    -fpermissive
    -fno-exceptions
    -fno-strict-aliasing
)

# ------------------------------------------------------------------------------
# Engine main executable
# ------------------------------------------------------------------------------
file(GLOB ENGINE_SOURCES
    engine/client/vgui/*
    engine/client/avi/*
    engine/client/soundlib/libmpg/*
    engine/client/soundlib/*
    engine/client/input/*
    engine/client/dll_int/*
    engine/client/parse/*
    engine/client/sound/*
    engine/client/*

    engine/common/imagelib/*
    engine/common/soundlib/*
    engine/common/*
    public/*

    engine/server/*

    engine/platform/posix/*
    engine/platform/sdl2/*
    engine/platform/ogc/*
    engine/platform/stub/*
    engine/platform/misc/lib_static.c
    )

# Exclude opus (we use the external library)
list(REMOVE_ITEM ENGINE_SOURCES
"${CMAKE_CURRENT_SOURCE_DIR}/engine/client/soundlib/snd_ogg_vorbis.c"
"${CMAKE_CURRENT_SOURCE_DIR}/engine/client/soundlib/snd_ogg_opus.c"
"${CMAKE_CURRENT_SOURCE_DIR}/engine/client/soundlib/ogg_filestream.c"
)

add_executable(xash ${ENGINE_SOURCES}
engine/platform/posix/sys_posix.c
engine/common/http/net_http_xash.c
)

# Default resolution per renderer
if(XASH_RENDERER STREQUAL "gl")
    set(XASH_OGC_DEFAULT_MODE -DDEFAULT_MODE_WIDTH=640 -DDEFAULT_MODE_HEIGHT=480)
else()
    set(XASH_OGC_DEFAULT_MODE -DDEFAULT_MODE_WIDTH=320 -DDEFAULT_MODE_HEIGHT=240)
endif()

target_compile_definitions(xash PUBLIC
    -DXASH_GAMEDIR="valve"
    -DXASH_NO_OGG_CONTAINER=1
    -DHAVE_OGG=0
    -DHAVE_OPUSFILE=0
    -DHAVE_VORBIS=0
    -DHAVE_VORBISFILE=0
    -DHAVE_OPUS=1
    -DXASH_LOW_MEMORY=${XASH_LOW_MEMORY}
    -DXASH_SDL=2
    -DGL_STATIC=1
    -DXASH_NO_LIBDL=1
    -DXASH_REF_GX_ENABLED=1
    -DSTATIC_LINKING=1
    -DXASH_STATIC_LIBS=1
    -DNO_VGUI=1
    -DENGINE_DLL=1
    -DXASH_ENABLE_MAIN=1
    -DFRAME_POINTERS=1
    -DBIGENDIAN=1
    -DDEFAULT_FPS_MAX="60"
    ${XASH_OGC_DEFAULT_MODE}
    -DXASH_OGC_GECKO=1
    -DDEBUG_LOOKUPS_COUNT=1
    -D_EXI_DEBUG=1
)

pkg_check_modules(DEPS REQUIRED sdl2 zlib)

target_include_directories(xash PUBLIC
    ${DEPS_INCLUDE_DIRS}
    common
    engine/server
    engine/client/vgui
    engine/client/avi
    engine/client
    engine
    engine/common
    engine/common/soundlib
    engine/common/imagelib
    filesystem
    public
    pm_shared
    engine/platform
    engine/platform/posix
    engine/platform/ogc
    engine/platform/stub
    3rdparty/library_suffix/include
    3rdparty/MultiEmulator/include
    3rdparty/opus/opus/include
)

target_link_libraries(xash PUBLIC
    filesystem_stdio
    hl
    cl_dll
    ${XASH_REF_LIB}
    menu
    SDL2::SDL2main
    BZip2::BZip2
    Opus::opus
    library_suffix
    pthread
    ${DEPS_LIBRARIES}
    MultiEmulator
)

target_compile_definitions(xash PUBLIC -D${XASH_REF_DEFINE})

# ------------------------------------------------------------------------------
# Diagnostic options
# ------------------------------------------------------------------------------
set(XASH_OGC_TEXDELAY "" CACHE STRING "microseconds to stall before each GX texture upload (diagnostic)")
if(XASH_OGC_TEXDELAY AND TARGET ref_gx)
    target_compile_definitions(ref_gx PUBLIC -DXASH_OGC_TEXDELAY=${XASH_OGC_TEXDELAY})
endif()

option(XASH_OGC_HEAPPROBE "report free memory, pool totals and texture memory at each map load" OFF)
if(XASH_OGC_HEAPPROBE)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_HEAPPROBE=1)
    if(TARGET ref_gx)
        target_compile_definitions(ref_gx PUBLIC -DXASH_OGC_HEAPPROBE=1)
    endif()
endif()

option(XASH_OGC_MEMCHECK "walk every memory pool sentinel each frame" OFF)
if(XASH_OGC_MEMCHECK)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_MEMCHECK=1)
    if(TARGET ref_gx)
        target_compile_definitions(ref_gx PUBLIC -DXASH_OGC_MEMCHECK=1)
    endif()
endif()

option(XASH_OGC_STACKWATCH "paint the main stack and report its high water mark" OFF)
if(XASH_OGC_STACKWATCH)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_STACKWATCH=1)
endif()

option(XASH_OGC_LMTRACE "log lightmap build progress (diagnostic)" OFF)
if(XASH_OGC_LMTRACE AND TARGET ref_gx)
    target_compile_definitions(ref_gx PUBLIC -DXASH_OGC_LMTRACE=1)
endif()

set(XASH_OGC_SLOTWATCH "" CACHE STRING "log texture slot/GL-name allocation (diagnostic)")
if(XASH_OGC_SLOTWATCH AND TARGET ref_gx)
    target_compile_definitions(ref_gx PUBLIC -DXASH_OGC_SLOTWATCH=${XASH_OGC_SLOTWATCH})
endif()

option(XASH_OGC_INPUTTEST "build with wii_showinput on by default" OFF)
if(XASH_OGC_INPUTTEST)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_INPUTTEST=1)
endif()

option(XASH_OGC_PADTRACE "log raw WPAD/PAD state every second (diagnostic)" OFF)
if(XASH_OGC_PADTRACE)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_PADTRACE=1)
endif()

option(XASH_OGC_FONTDUMP "draw the console font atlas over the screen (diagnostic)" OFF)
if(XASH_OGC_FONTDUMP)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_FONTDUMP=1)
endif()

option(XASH_OGC_FMTTRACE "log the internal format chosen for every texture" OFF)
if(XASH_OGC_FMTTRACE AND TARGET ref_gx)
    target_compile_definitions(ref_gx PUBLIC -DXASH_OGC_FMTTRACE=1)
endif()

option(XASH_OGC_TEXTRACE "log arena free space on every GX texture upload" OFF)
if(XASH_OGC_TEXTRACE AND TARGET ref_gx)
    target_compile_definitions(ref_gx PUBLIC -DXASH_OGC_TEXTRACE=1)
endif()

option(XASH_OGC_AIMPROOF "log the pointer aim offset at both ends of the client/server chain" OFF)
if(XASH_OGC_AIMPROOF)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_AIMPROOF=1)
endif()

option(XASH_OGC_AIMTEST "self-test the pointer aim maths at startup" OFF)
if(XASH_OGC_AIMTEST)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_AIMTEST=1)
endif()

option(XASH_OGC_MONITOR "report progress from a watchdog thread" OFF)
if(XASH_OGC_MONITOR)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_MONITOR=1)
    if(TARGET ref_gx)
        target_compile_definitions(ref_gx PRIVATE -DXASH_OGC_MONITOR=1)
    endif()
endif()

option(XASH_ZEROINIT "zero-initialise all automatic variables" OFF)
option(XASH_ZEROINIT_REF "zero-initialise automatics in the renderer only" OFF)
option(XASH_ZEROINIT_ENGINE "zero-initialise automatics in the engine only" OFF)
if(XASH_ZEROINIT OR XASH_ZEROINIT_ENGINE)
    target_compile_options(xash PUBLIC -ftrivial-auto-var-init=zero)
endif()

option(XASH_OGC_GECKO_WAIT "wait at boot for a USB Gecko listener" OFF)
if(XASH_OGC_GECKO_WAIT)
    target_compile_definitions(xash PUBLIC -DXASH_OGC_GECKO_WAIT=15)
endif()

# Wii specific: generate symbol list and .dol
dkp_target_generate_symbol_list(xash)
ogc_create_dol(xash)

# Optimization flags for the engine
target_compile_options(xash PUBLIC
    -std=gnu11
    -fno-strict-aliasing
    -fno-stack-protector
    -ffunction-sections
    -fdata-sections
    -fno-exceptions
    -freorder-blocks
    -fvisibility=hidden
    -Wno-implicit-function-declaration
    -Wno-int-conversion
    -Wno-unused-function
)

target_link_options(xash PUBLIC
    -Wl,--gc-sections
    -fwhole-program
    -Wl,--build-id=none
)