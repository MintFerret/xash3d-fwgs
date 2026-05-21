cmake_minimum_required(VERSION 3.13)
project(xash3d C CXX)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
add_compile_options(-G0)

find_package(PkgConfig REQUIRED)
find_package(SDL2 REQUIRED)
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
)

file(GLOB PUBLIC_SOURCES public/*)
list(REMOVE_ITEM PUBLIC_SOURCES "public/build_vcs.c")
add_library(public STATIC ${PUBLIC_SOURCES})
target_include_directories(public PUBLIC 3rdparty/library_suffix/include common engine public)

#add_library(build_vcs STATIC public/build_vcs.c)

add_subdirectory(3rdparty/library_suffix)

set(OPUS_CUSTOM_MODES ON CACHE BOOL "" FORCE)
set(OPUS_FLOAT_APPROX ON CACHE BOOL "" FORCE)
set(OPUS_HARDENING ON CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/opus/opus)

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
	-DXASH_STATIC_LIBS=1
	-D_Mem_Free=FS_Mem_Free
	-D_Mem_Alloc=FS_Mem_Alloc
)
target_compile_options(filesystem_stdio PRIVATE
-MMD
-MP
-fno-omit-frame-pointer -ffunction-sections -fno-asynchronous-unwind-tables
-fno-exceptions -fpermissive
-fno-PIC
-mno-sdata
-G0
)
target_link_libraries(filesystem_stdio public library_suffix)

#cl_dll, dlls, menu goes here
include(makelibrary.txt)

file(GLOB REF_GLGX_SOURCES
	ref/common/*
	ref/gl/*
	)
add_library(ref_gl STATIC ${REF_GLGX_SOURCES})
target_include_directories(ref_gl PUBLIC
 	3rdparty/library_suffix/include
 	ref/common
 	ref/gl
 	common
 	engine
 	engine/common
 	engine/common/imagelib
 	public
 	pm_shared
 	filesystem
 	)
 target_compile_definitions(ref_gl PRIVATE
 	-DXASH_LOW_MEMORY=2
 	-DXASH_STATIC_LIBS=1
 	-DREF_DLL=1
 	-DXASH_GL_STATIC=1
 	)
 target_compile_options(ref_gl PRIVATE -fpermissive -fno-exceptions -mno-sdata -G0)
 target_link_libraries(ref_gl public)

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

list(REMOVE_ITEM ENGINE_SOURCES
	"engine/client/cl_steam.c"
	"engine/client/input/in_touch.c"
)
#include(enginesources.txt)
#set(ENGINE_SOURCES ${XASH_CLIENT_SOURCES} ${XASH_SOURCES} ${XASH_SERVER_SOURCES} ${XASH_PLATFORM_SOURCES})

add_executable(xash ${ENGINE_SOURCES}
engine/platform/posix/sys_posix.c
engine/common/http/net_http_xash.c
)
target_compile_definitions(xash PUBLIC
#	-DXASH_DEDICATED=1
	-DXASH_GAMEDIR="valve"
	-DHAVE_LARGEFILE=1
	-DHAVE_OGG=1
	-DHAVE_OPUSFILE=1
	-DHAVE_VORBIS=1
	-DHAVE_VORBISFILE=1
	-DHAVE_OPUS=1
	-DXASH_LOW_MEMORY=2
	-DXASH_SDL=2
	-DHAVE_GL=1
	-DGL_STATIC=1
	-DXASH_NO_LIBDL=1
	-DXASH_REF_GL_ENABLED=1
	-DSTATIC_LINKING=1
	-DXASH_STATIC_LIBS=1
	-DNO_VGUI=1
	-DENGINE_DLL=1
	-DXASH_ENABLE_MAIN=1
	-DFRAME_POINTERS=1
	-DBIGENDIAN=1
	)

pkg_check_modules(DEPS REQUIRED sdl2 opusfile vorbis vorbisfile opengl) #bzip2 #freetype2 #zlib

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


#add_custom_command(TARGET xash PRE_LINK COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_BINARY_DIR}/public/*.o")

target_link_libraries(xash PUBLIC filesystem_stdio hl cl_dll ref_gl menu
SDL2::SDL2main Opus::opus
library_suffix pthread bz2 ${DEPS_LIBRARIES}
MultiEmulator
opengx
PkgConfig::OPENGL
wiisocket
)
dkp_target_generate_symbol_list(xash)
ogc_create_dol(xash)

target_compile_options(xash PUBLIC
	-mno-sdata
	-std=gnu11
#	-Os
	-fno-unwind-tables
	-fno-asynchronous-unwind-tables
	-fno-omit-frame-pointer
	-fno-strict-aliasing
	-fno-stack-protector
	-ffunction-sections
#	-fdata-sections
	-fno-exceptions
	-freorder-blocks-algorithm=simple
	#-freorder-blocks
#	-fvisibility=hidden
	#-flto=auto
	-Wno-implicit-function-declaration
	-Wno-int-conversion
	-Wno-unused-function
	#-fwhole-program
#	-fno‑data‑sections
	-fno-PIC
	-G0
)
target_link_options(xash PUBLIC
#	-fcommon
#	 -Wl,--allow-multiple-definition
	#-fno-inline
	-Wl,--gc-sections
#	-fwhole-program
	-Wl,--build-id=none

)
