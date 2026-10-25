/*
filesystem.c - game filesystem based on DP fs
Copyright (C) 2003-2006 Mathieu Olivier
Copyright (C) 2000-2007 DarkPlaces contributors
Copyright (C) 2007 Uncle Mike
Copyright (C) 2015-2023 Xash3D FWGS contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#if XASH_SDL == 2
#include <SDL.h> // SDL_GetBasePath
#elif XASH_SDL == 3
#include <SDL3/SDL.h>
#endif

#include <errno.h>
#include "common.h"
#include "library.h"
#include "platform/platform.h"

#if XASH_OGC
#include <fat.h>
#include <ogc/usbstorage.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if XASH_WIN32
#include <direct.h>
#endif

static CVAR_DEFINE_AUTO( fs_mount_hd, "0", FCVAR_PRIVILEGED, "mount high definition content folder" );
static CVAR_DEFINE_AUTO( fs_mount_lv, "0", FCVAR_PRIVILEGED, "mount low violence models content folder" );
static CVAR_DEFINE_AUTO( fs_mount_addon, "0", FCVAR_PRIVILEGED, "mount addon content folder" );
static CVAR_DEFINE_AUTO( fs_mount_l10n, "0", FCVAR_PRIVILEGED, "mount localization content folder" );
static CVAR_DEFINE_AUTO( ui_language, "english", FCVAR_PRIVILEGED, "selected game language" );

fs_api_t g_fsapi;
#if XASH_OGC
// perfil de lecturas (wii_prof): se envuelven LoadFile y Read del filesystem
static byte *(*fs_real_LoadFile)( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly );
static fs_offset_t (*fs_real_Read)( file_t *file, void *buffer, size_t buffersize );
static file_t *(*fs_real_Open)( const char *filepath, const char *mode, qboolean gamedironly );
static int (*fs_real_Seek)( file_t *file, fs_offset_t offset, int whence );

static byte *OGC_FS_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
    double t0 = Platform_DoubleTime();
    fs_offset_t size = 0;
    byte *buf = fs_real_LoadFile( path, &size, gamedironly );

    if( filesizeptr ) *filesizeptr = size;
    OGC_ProfAdd( 8, Platform_DoubleTime() - t0 );
    if( buf ) OGC_ProfBytes( 8, (double)size );
    return buf;
}

static fs_offset_t OGC_FS_Read( file_t *file, void *buffer, size_t buffersize )
{
    double t0 = Platform_DoubleTime();
    fs_offset_t n = fs_real_Read( file, buffer, buffersize );

    OGC_ProfAdd( 9, Platform_DoubleTime() - t0 );
    if( n > 0 ) OGC_ProfBytes( 9, (double)n );
    return n;
}

static file_t *OGC_FS_Open( const char *filepath, const char *mode, qboolean gamedironly )
{
    double t0 = Platform_DoubleTime();
    file_t *f = fs_real_Open( filepath, mode, gamedironly );

    OGC_ProfAdd( 12, Platform_DoubleTime() - t0 );
    return f;
}

static int OGC_FS_Seek( file_t *file, fs_offset_t offset, int whence )
{
    double t0 = Platform_DoubleTime();
    int ret = fs_real_Seek( file, offset, whence );

    OGC_ProfAdd( 13, Platform_DoubleTime() - t0 );
    return ret;
}

static void OGC_HookFS( void )
{
    fs_real_LoadFile = g_fsapi.LoadFile;
    fs_real_Read = g_fsapi.Read;
    fs_real_Open = g_fsapi.Open;
    fs_real_Seek = g_fsapi.Seek;
    g_fsapi.LoadFile = OGC_FS_LoadFile;
    g_fsapi.Read = OGC_FS_Read;
    g_fsapi.Open = OGC_FS_Open;
    g_fsapi.Seek = OGC_FS_Seek;
}
#endif
fs_globals_t *FI;

static pfnCreateInterface_t fs_pfnCreateInterface;
static HINSTANCE fs_hInstance;

#if !XASH_OGC
search_t *FS_Search( const char *pattern, int caseinsensitive, int gamedironly )
{
	return g_fsapi.Search( pattern, caseinsensitive, gamedironly );
}

int FS_Close( file_t *file )
{
	return g_fsapi.Close( file );
}

file_t *FS_Open( const char *filepath, const char *mode, qboolean gamedironly )
{
	return g_fsapi.Open( filepath, mode, gamedironly );
}

byte *FS_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
    return g_fsapi.LoadFile( path, filesizeptr, gamedironly );
}

byte *FS_LoadDirectFile( const char *path, fs_offset_t *filesizeptr )
{
	return g_fsapi.LoadDirectFile( path, filesizeptr );
}
#endif
static void COM_StripDirectorySlash( char *pname )
{
	size_t len = Q_strlen( pname );
	if( len > 0 && pname[len - 1] == '/' )
		pname[len - 1] = 0;
}

void *FS_GetNativeObject( const char *obj )
{
	if( fs_pfnCreateInterface )
		return fs_pfnCreateInterface( obj, NULL );

	return NULL;
}

static uint32_t FS_MountFlags( void )
{
	uint32_t flags = 0;

	// FIXME: VFS shouldn't care about this, allow engine to mount gamedirs
	if( fs_mount_lv.value ) SetBits( flags, FS_MOUNT_LV );
	if( fs_mount_hd.value ) SetBits( flags, FS_MOUNT_HD );
	if( fs_mount_addon.value ) SetBits( flags, FS_MOUNT_ADDON );
	if( fs_mount_l10n.value ) SetBits( flags, FS_MOUNT_L10N );

	return flags;
}

void FS_Rescan_f( void )
{
	g_fsapi.Rescan( FS_MountFlags(), ui_language.string );
}

static void FS_LoadVFSConfig( const char *gamedir )
{
	string parm;

	if( Host_IsDedicated( ))
		return;

	Cbuf_AddTextf( "exec %s/vfs.cfg\n", gamedir );
	Cbuf_Execute();

	if( Sys_GetParmFromCmdLine( "-language", parm ))
	{
		Cvar_DirectSet( &ui_language, parm );
		Cvar_DirectSet( &fs_mount_l10n, "1" );
	}

	ClearBits( fs_mount_hd.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_lv.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_l10n.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_addon.flags, FCVAR_CHANGED );
	ClearBits( ui_language.flags, FCVAR_CHANGED );
}

void FS_SaveVFSConfig( void )
{
	const qboolean force_save = !FS_FileExists( "vfs.cfg", true );

	if( !force_save && !FBitSet( fs_mount_hd.flags|fs_mount_lv.flags|fs_mount_l10n.flags|fs_mount_addon.flags|ui_language.flags, FCVAR_CHANGED ))
	{
		Con_Reportf( "%s: no need to save vfs.cfg\n", __func__ );
		return;
	}

	Con_Printf( "%s()\n", __func__ );

	file_t *f = FS_Open( "vfs.cfg.new", "w", true );
	if( !f )
	{
		Con_Printf( S_ERROR "%s: can't open %s for write\n", __func__, "vfs.cfg.new" );
		return;
	}

	FS_Printf( f, "%s \"%d\"\n", fs_mount_hd.name, (int)fs_mount_hd.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_lv.name, (int)fs_mount_lv.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_l10n.name, (int)fs_mount_l10n.value );
	FS_Printf( f, "%s \"%d\"\n", fs_mount_addon.name, (int)fs_mount_addon.value );
	FS_Printf( f, "%s \"%s\"\n", ui_language.name, ui_language.string );

	Host_FinalizeConfig( f, "vfs.cfg" );

	ClearBits( fs_mount_hd.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_lv.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_l10n.flags, FCVAR_CHANGED );
	ClearBits( fs_mount_addon.flags, FCVAR_CHANGED );
	ClearBits( ui_language.flags, FCVAR_CHANGED );
}

void COM_FS_LoadGameInfo( void )
{
	FS_LoadVFSConfig( g_fsapi.Gamedir( ));

	g_fsapi.LoadGameInfo( FS_MountFlags(), ui_language.string );
}

static void FS_ClearPaths_f( void )
{
	FS_ClearSearchPath();
}

static void FS_Path_f_( void )
{
	FS_Path_f();
}

static void FS_FindFile_f_( void )
{
	if( Cmd_Argc() < 2 )
	{
		Con_Printf( S_USAGE "fs_find <filepath>\n" );
		return;
	}
	g_fsapi.FindFile_f( Cmd_Argv( 1 ));
}

static void FS_MakeGameInfo_f( void )
{
	g_fsapi.MakeGameInfo();
}

static const fs_interface_t fs_memfuncs =
{
	Con_Printf,
	Con_DPrintf,
	Con_Reportf,
	Sys_Error,

	_Mem_AllocPool,
	_Mem_FreePool,
	_Mem_Alloc,
	_Mem_Realloc,
	_Mem_Free,

	Sys_GetNativeObject,
};

static void FS_UnloadProgs( void )
{
	if( fs_hInstance )
	{
		COM_FreeLibrary( fs_hInstance );
		fs_hInstance = 0;
	}
}

#ifdef XASH_INTERNAL_GAMELIBS
#define FILESYSTEM_STDIO_DLL "filesystem_stdio"
#elif XASH_ANDROID
#define FILESYSTEM_STDIO_DLL "libfilesystem_stdio.so"
#else
#define FILESYSTEM_STDIO_DLL "filesystem_stdio." OS_LIB_EXT
#endif

static qboolean FS_LoadProgs( void )
{
	const char *name = FILESYSTEM_STDIO_DLL;

	fs_hInstance = COM_LoadLibrary( name, false, true );

	if( !fs_hInstance )
	{
		Sys_Error( "%s: can't load filesystem library %s: %s\n", __func__, name, COM_GetLibraryError() );
		return false;
	}

	FSAPI GetFSAPI;
	if( !( GetFSAPI = (FSAPI)COM_GetProcAddress( fs_hInstance, GET_FS_API )))
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't find GetFSAPI entry point in %s\n", __func__, name );
		return false;
	}

	if( GetFSAPI( FS_API_VERSION, &g_fsapi, &FI, &fs_memfuncs ) != FS_API_VERSION )
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't initialize filesystem API: wrong version\n", __func__ );
		return false;
	}

#if XASH_OGC
	OGC_HookFS();
#endif

	if( !( fs_pfnCreateInterface = (pfnCreateInterface_t)COM_GetProcAddress( fs_hInstance, "CreateInterface" )))
	{
		FS_UnloadProgs();
		Sys_Error( "%s: can't find CreateInterface entry point in %s\n", __func__, name );
		return false;
	}

	Con_DPrintf( "%s: filesystem_stdio successfully loaded\n", __func__ );
	return true;
}

#if XASH_OGC
/*
==================
OGC_MountSD

libfat has to be up before anything opens a file, and both the logfile and the
base directory lookup need it. Whichever runs first does the mount; the other
gets the cached answer.
==================
*/
// carpeta de datos (sd:/xash3d o usb:/xash3d) elegida al montar
static char ogc_basedir[32] = "sd:/xash3d";
static const char *ogc_argv0;

void OGC_SetArgv0( const char *argv0 )
{
	ogc_argv0 = argv0;
}

const char *OGC_BaseDir( void )
{
	return ogc_basedir;
}

static qboolean OGC_IsDir( const char *path )
{
	struct stat st;

	return stat( path, &st ) == 0 && S_ISDIR( st.st_mode );
}

// busca xash3d/valve (o al menos xash3d) en los dispositivos, en el orden dado
static qboolean OGC_FindBase( const char *first, const char *second )
{
	const char *devs[2] = { first, second };
	int pass, i;
	char path[40];

	for( pass = 0; pass < 2; pass++ )
	{
		for( i = 0; i < 2; i++ )
		{
			Q_snprintf( path, sizeof( path ), "%s:/xash3d%s", devs[i], pass == 0 ? "/valve" : "" );

			if( OGC_IsDir( path ))
			{
				Q_snprintf( ogc_basedir, sizeof( ogc_basedir ), "%s:/xash3d", devs[i] );
				return true;
			}
		}
	}

	return false;
}

qboolean OGC_MountSD( void )
{
    static int mounted = -1;

    if( mounted < 0 )
    {
        const qboolean from_usb = ogc_argv0 && !Q_strnicmp( ogc_argv0, "usb", 3 );
        const char *first = from_usb ? "usb" : "sd";
        const char *second = from_usb ? "sd" : "usb";
        int tries;

        mounted = fatInit( 32, true ) ? 1 : 0; // cache grande, ver sys_debuglog.c

        if( !OGC_FindBase( first, second ))
        {
            // fatInitDefault no siempre deja el USB listo a la primera
            for( tries = 0; tries < 8; tries++ )
            {
                if( OGC_IsDir( "usb:/" ) || fatMountSimple( "usb", &__io_usbstorage ))
                {
                    if( OGC_FindBase( first, second ))
                    {
                        mounted = 1;
                        break;
                    }
                }

                usleep( 500000 );
            }
        }
    }

    return mounted == 1 ? true : false;
}
#endif

static qboolean FS_DetermineRootDirectory( char *out, size_t size )
{
	const char *path = getenv( "XASH3D_BASEDIR" );

	if( !COM_StringEmptyOrNULL( path ))
	{
		Q_strncpy( out, path, size );
		return true;
	}

#if XASH_IOS
	Q_strncpy( out, IOS_GetDocsDir(), size );
	return true;
#elif XASH_PSVITA
	if( PSVita_GetBasePath( out, size ))
		return true;
	Sys_Error( "couldn't find %s data directory", XASH_ENGINE_NAME );
	return false;
#elif XASH_OGC
	if( OGC_MountSD( ))
	{
		Q_strncpy( out, OGC_BaseDir(), size );
		return true;
	}
	Sys_Error( "couldn't find %s data directory", XASH_ENGINE_NAME );
	return false;
#elif ( XASH_SDL >= 2 ) && !XASH_NSWITCH // GetBasePath not impl'd in switch-sdl2
	path = SDL_GetBasePath();

#if XASH_APPLE
	if( path != NULL && Q_stristr( path, ".app" ))
	{
		SDL_free((void *)path );
		path = SDL_GetPrefPath( NULL, XASH_ENGINE_NAME );
	}
#endif

	if( path != NULL )
	{
		Q_strncpy( out, path, size );
		SDL_free((void *)path );
		return true;
	}

#if XASH_POSIX || XASH_WIN32
	if( getcwd( out, size ))
		return true;
	Sys_Error( "couldn't determine current directory: %s, getcwd: %s", SDL_GetError(), strerror( errno ));
#else // !( XASH_POSIX || XASH_WIN32 )
	Sys_Error( "couldn't determine current directory: %s", SDL_GetError( ));
#endif // !( XASH_POSIX || XASH_WIN32 )
	return false;
#else // generic case
	if( getcwd( out, size ))
		return true;

	Sys_Error( "couldn't determine current directory: %s", strerror( errno ));
	return false;
#endif // generic case
}

static qboolean FS_DetermineReadOnlyRootDirectory( char *out, size_t size )
{
	const char *env_rodir = getenv( "XASH3D_RODIR" );

	if( _Sys_GetParmFromCmdLine( "-rodir", out, size ))
		return true;

	if( !COM_StringEmptyOrNULL( env_rodir ))
	{
		Q_strncpy( out, env_rodir, size );
		return true;
	}

#if XASH_IOS
	Q_strncpy( out, IOS_GetExecDir(), size );
	return true;
#endif

	return false;
}

/*
================
FS_Init
================
*/
void FS_Init( void )
{
	string gamedir;
	char rodir[MAX_OSPATH], rootdir[MAX_OSPATH];
	rodir[0] = rootdir[0] = 0;

	if( !FS_DetermineRootDirectory( rootdir, sizeof( rootdir )) || COM_StringEmpty( rootdir ))
	{
		Sys_Error( "couldn't determine current directory (empty string)" );
		return;
	}
	COM_FixSlashes( rootdir );
	COM_StripDirectorySlash( rootdir );

	FS_DetermineReadOnlyRootDirectory( rodir, sizeof( rodir ));
	COM_FixSlashes( rodir );
	COM_StripDirectorySlash( rodir );

	if( !Sys_GetParmFromCmdLine( "-game", gamedir ))
	{
		char *env = getenv( "XASH3D_GAME" );
		if( env )
			Q_strncpy( gamedir, env, sizeof( gamedir ));
		else
			Q_strncpy( gamedir, host.default_gamedir, sizeof( gamedir )); // gamedir == basedir
	}

	FS_LoadProgs();

	// TODO: this function will cause engine to stop in case of fail
	// when it will have an option to return string error, restore Sys_Error
	// FIXME: why do we call this function before InitStdio?
	// because InitStdio immediately scans all available game directories
	// and this better be reworked at some point
	g_fsapi.SetCurrentDirectory( rootdir );

	if( !g_fsapi.InitStdio( true, rootdir, host.default_gamedir, gamedir, rodir ))
	{
		Sys_Error( "Can't init filesystem_stdio!\n" );
		return;
	}

	Cmd_AddRestrictedCommand( "fs_rescan", FS_Rescan_f, "rescan filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_path", FS_Path_f_, "show filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_find", FS_FindFile_f_, "find file across search pathes and show all occurences" );
	Cmd_AddRestrictedCommand( "fs_clearpaths", FS_ClearPaths_f, "clear filesystem search pathes" );
	Cmd_AddRestrictedCommand( "fs_make_gameinfo", FS_MakeGameInfo_f, "create gameinfo.txt for current running game" );

	Cvar_RegisterVariable( &fs_mount_hd );
	Cvar_RegisterVariable( &fs_mount_lv );
	Cvar_RegisterVariable( &fs_mount_addon );
	Cvar_RegisterVariable( &fs_mount_l10n );
	Cvar_RegisterVariable( &ui_language );

	if( !Sys_GetParmFromCmdLine( "-dll", host.gamedll ))
		host.gamedll[0] = 0;

	if( !Sys_GetParmFromCmdLine( "-clientlib", host.clientlib ))
		host.clientlib[0] = 0;

	if( !Sys_GetParmFromCmdLine( "-menulib", host.menulib ))
		host.menulib[0] = 0;
}

/*
================
FS_Shutdown
================
*/
void FS_Shutdown( void )
{
	if( g_fsapi.ShutdownStdio )
		g_fsapi.ShutdownStdio();

	FS_UnloadProgs();
}
