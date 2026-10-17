/*
gx_video.c - video/context init, default GX state, renderer info (Wii GX native port)
Copyright (C) 2010 Uncle Mike
Ported to Wii GX by Gerardo

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "gx_local.h"

#include <gccore.h>
#include <ogc/gx.h>
#include <ogc/video.h>
#include <ogc/system.h>
#include <ogc/consol.h>
#include <sys/iosupport.h>
#include <malloc.h>

CVAR_DEFINE( gl_extensions, "gl_allow_extensions", "1", FCVAR_GLCONFIG|FCVAR_READ_ONLY, "allow gl_extensions" );
CVAR_DEFINE( gl_texture_anisotropy, "gl_anisotropy", "8", FCVAR_GLCONFIG, "textures anisotropic filter" );
CVAR_DEFINE_AUTO( gl_texture_lodbias, "0.0", FCVAR_GLCONFIG, "LOD bias for mipmapped textures (perfomance|quality)" );
CVAR_DEFINE_AUTO( gl_texture_nearest, "0", FCVAR_GLCONFIG, "disable texture filter" );
CVAR_DEFINE_AUTO( gl_lightmap_nearest, "0", FCVAR_GLCONFIG, "disable lightmap filter" );
CVAR_DEFINE_AUTO( gl_keeptjunctions, "1", FCVAR_GLCONFIG, "removing tjuncs causes blinking pixels" );
CVAR_DEFINE_AUTO( gl_check_errors, "1", FCVAR_GLCONFIG, "ignore video engine errors" );
CVAR_DEFINE_AUTO( gl_polyoffset, "2", FCVAR_GLCONFIG, "polygon offset for decals" );
CVAR_DEFINE_AUTO( gl_polyoffset_bmodels, "2", FCVAR_GLCONFIG, "polygon offset for brush models" );
CVAR_DEFINE_AUTO( gl_wireframe, "0", FCVAR_GLCONFIG|FCVAR_SPONLY, "show wireframe overlay" );
CVAR_DEFINE_AUTO( gl_finish, "0", FCVAR_GLCONFIG, "use GX_DrawDone instead of a plain flush" );
CVAR_DEFINE_AUTO( gl_nosort, "0", FCVAR_GLCONFIG, "disable sorting of translucent surfaces" );
CVAR_DEFINE_AUTO( gl_test, "0", 0, "engine developer cvar for quick testing new features" );
CVAR_DEFINE_AUTO( gl_msaa, "0", FCVAR_GLCONFIG, "enable or disable the GX copy-filter antialiasing" );
CVAR_DEFINE_AUTO( gl_stencilbits, "8", FCVAR_GLCONFIG|FCVAR_READ_ONLY, "pixelformat stencil bits (0 - auto)" );
CVAR_DEFINE_AUTO( gl_overbright, "0", FCVAR_GLCONFIG, "overbrights" );
CVAR_DEFINE_AUTO( gl_fog, "1", FCVAR_GLCONFIG, "allow for rendering fog using the GX hardware fog unit" );
CVAR_DEFINE_AUTO( gl_litwater_force, "0", FCVAR_GLCONFIG, "force enable lightmapped water, even if support not declared in the map" );
CVAR_DEFINE_AUTO( r_lighting_ambient, "0.3", FCVAR_GLCONFIG, "map ambient lighting scale" );
CVAR_DEFINE_AUTO( r_detailtextures, "0", FCVAR_GLCONFIG, "enable detail textures support" );
CVAR_DEFINE_AUTO( r_novis, "0", 0, "ignore vis information (perfomance test)" );
CVAR_DEFINE_AUTO( r_nocull, "0", 0, "ignore frustrum culling (perfomance test)" );
CVAR_DEFINE_AUTO( r_lockpvs, "0", FCVAR_CHEAT, "lockpvs area at current point (pvs test)" );
CVAR_DEFINE_AUTO( r_lockfrustum, "0", FCVAR_CHEAT, "lock frustrum area at current point (cull test)" );
CVAR_DEFINE_AUTO( r_traceglow, "0", FCVAR_GLCONFIG, "cull flares behind models" );
CVAR_DEFINE_AUTO( gl_round_down, "2", FCVAR_GLCONFIG|FCVAR_READ_ONLY, "round texture sizes to nearest POT value" );

CVAR_DEFINE( r_vbo, "gl_vbo", "0", FCVAR_GLCONFIG|FCVAR_READ_ONLY, "draw world using VBO-style indexed arrays (unused on GX)" );

CVAR_DEFINE( r_vbo_detail, "gl_vbo_detail", "0", FCVAR_GLCONFIG, "detail vbo mode (0: disable, 1: multipass, 2: singlepass, broken decal dlights)" );
CVAR_DEFINE( r_vbo_dlightmode, "gl_vbo_dlightmode", "1", FCVAR_GLCONFIG, "vbo dlight rendering mode (0-1)" );
CVAR_DEFINE( r_vbo_overbrightmode, "gl_vbo_overbrightmode", "0", FCVAR_GLCONFIG, "vbo overbright rendering mode (0-1)" );
CVAR_DEFINE_AUTO( r_ripple, "0", FCVAR_GLCONFIG, "enable software-like water texture ripple simulation" );
CVAR_DEFINE_AUTO( r_ripple_updatetime, "0.05", FCVAR_GLCONFIG, "how fast ripple simulation is" );
CVAR_DEFINE_AUTO( r_ripple_spawntime, "0.1", FCVAR_GLCONFIG, "how fast new ripples spawn" );
CVAR_DEFINE_AUTO( r_large_lightmaps, "0", FCVAR_GLCONFIG|FCVAR_LATCH, "enable larger lightmap atlas textures (might break custom renderer mods)" );

gl_globals_t	tr;
glconfig_t	glConfig;
glstate_t	glState;
glwstate_t	glw_state;

static struct
{
	GXRModeObj	*rmode;
	void		*xfb[2];
	int		fb;
	void		*fifo;
} gxvid;

static qboolean gx_video_initialized = false;

#define GX_FIFO_SIZE ( 256 * 1024 )

// Diagnostic modes, read once from sd:/xash3d/gxtest.txt (one digit):
//  1 = solid blue frame (tests EFB->XFB->TV path only)
//  2 = 3 flat colour bars, no textures (tests GX raster path)
//  3 = engine runs normally but R_DrawStretchPic draws nothing (blames textures)
int gx_testmode;

static void GX_TestBars( void )
{
	static const GXColor col[3] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } };
	float w = (float)gpGlobals->width, h = (float)gpGlobals->height;

	R_Set2DMode( true );
	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0 );

	for( int i = 0; i < 3; i++ )
	{
		float x0 = w * i / 3.0f, x1 = w * ( i + 1 ) / 3.0f;
		GX_SetChanMatColor( GX_COLOR0A0, col[i] );
		GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );
			GX_Position2f32( x0, 0.0f );
			GX_Position2f32( x1, 0.0f );
			GX_Position2f32( x1, h );
			GX_Position2f32( x0, h );
		GX_End();
	}

	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
}

void GX_Present( void )
{
	static int n;

	if( !gx_video_initialized || !gxvid.xfb[0] )
		return;

	if( gx_testmode == 1 )
	{
		GXColor blue = { 0, 0, 255, 255 };
		GXColor black = { 0, 0, 0, 255 };
		GX_DrawDone();
		GX_SetCopyClear( blue, GX_MAX_Z24 );
		GX_CopyDisp( gxvid.xfb[gxvid.fb], GX_TRUE ); // flush engine frame, clears EFB to blue
		GX_DrawDone();
		GX_CopyDisp( gxvid.xfb[gxvid.fb], GX_TRUE ); // copies the blue EFB
		GX_DrawDone();
		GX_SetCopyClear( black, GX_MAX_Z24 );
		VIDEO_SetNextFramebuffer( gxvid.xfb[gxvid.fb] );
		VIDEO_Flush();
		VIDEO_WaitVSync();
		gxvid.fb ^= 1;
		return;
	}

	if( gx_testmode == 2 )
		GX_TestBars();

	if( n < 5 ) gEngfuncs.Con_Printf( "[GX] present %d: DrawDone\n", n );
	GX_DrawDone();
	/* GX_CopyDisp(clear=TRUE) solo limpia color/alpha/Z si sus escrituras estan habilitadas en ese momento. */
	/* Si el ultimo dibujo dejo Z-write o color-update apagado, el EFB no se limpia y el frame anterior se acumula. */
	GX_SetColorUpdate( GX_TRUE );
	GX_SetAlphaUpdate( GX_TRUE );
	GX_SetZMode( GX_TRUE, GX_ALWAYS, GX_TRUE );
	if( n < 5 ) gEngfuncs.Con_Printf( "[GX] present %d: CopyDisp\n", n );
	GX_CopyDisp( gxvid.xfb[gxvid.fb], GX_TRUE );
	GX_DrawDone(); /* esperar a que la copia EFB->XFB termine antes de mostrarla (evita tearing) */

	VIDEO_SetNextFramebuffer( gxvid.xfb[gxvid.fb] );
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if( gxvid.rmode && FBitSet( gxvid.rmode->viTVMode, VI_NON_INTERLACE ))
		VIDEO_WaitVSync();
	if( n < 5 ) gEngfuncs.Con_Printf( "[GX] present %d: vsync ok\n", n );

	gxvid.fb ^= 1;
	n++;
}

static void GX_SetDefaultTexState( void )
{
	memset( glState.currentTexturesIndex, -1, MAX_TEXTURE_UNITS * sizeof( *glState.currentTexturesIndex ));
	memset( glState.currentTextures, 0, MAX_TEXTURE_UNITS * sizeof( *glState.currentTextures ));
	memset( glState.texCoordArrayMode, 0, MAX_TEXTURE_UNITS * sizeof( *glState.texCoordArrayMode ));
	memset( glState.genSTEnabled, 0, MAX_TEXTURE_UNITS * sizeof( *glState.genSTEnabled ));

	for( int i = 0; i < MAX_TEXTURE_UNITS; i++ )
		glState.texIdentityMatrix[i] = true;
}

static void GX_SetDefaultState( void )
{
	memset( &glState, 0, sizeof( glState ));
	GX_SetDefaultTexState();

	tr.draw_list = &tr.draw_stack[0];
	tr.draw_stack_pos = 0;
}

static void GX_SetDefaults( void )
{
	GXColor white = { 255, 255, 255, 255 };

	GX_DrawDone();

	GX_SetZMode( GX_FALSE, GX_LEQUAL, GX_FALSE );
	GX_SetCullMode( GX_CULL_NONE );
	GX_SetScissor( 0, 0, gpGlobals->width, gpGlobals->height );
	GX_SetChanMatColor( GX_COLOR0A0, white );

	glState.stencilEnabled = false;

	GX_CleanupAllTextureUnits();

	GX_SetBlendMode( GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );

	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );

	GX_SetPointSize( (u8)( 1.2f * 6.0f ), GX_TO_ZERO );
	GX_SetLineWidth( (u8)( 1.2f * 6.0f ), GX_TO_ZERO );

	GX_Cull( XASH_CULL_NONE );
}

static void R_RenderInfo( qboolean startup )
{
	gEngfuncs.Con_Printf( "\n" );
	gEngfuncs.Con_Printf( "GX_VENDOR: Nintendo\n" );
	gEngfuncs.Con_Printf( "GX_RENDERER: Hollywood (Flipper-derived), libogc GX\n" );
	gEngfuncs.Con_Printf( "GX_VERSION: native (fixed-function, no shader stages)\n" );

	gEngfuncs.Con_Printf( "GX_MAX_TEXTURE_SIZE: %i\n", glConfig.max_2d_texture_size );
	gEngfuncs.Con_Printf( "GX_MAX_TEXTURE_UNITS: %i\n", glConfig.max_texture_units );
	gEngfuncs.Con_Printf( "GX_MAX_TEV_STAGES: %i\n", glConfig.max_texture_units );

	gEngfuncs.Con_Printf( "\n" );
	gEngfuncs.Con_Printf( "MODE: %ix%i (%s, %s%s)\n", gpGlobals->width, gpGlobals->height,
		gxvid.rmode && ( gxvid.rmode->viTVMode >> 2 ) == VI_PAL ? "PAL" : "NTSC",
		GL_Support( GX_HW_PROGRESSIVE ) ? "progressive" : "interlaced",
		GL_Support( GX_HW_WIDESCREEN ) ? ", 16:9" : "" );
	gEngfuncs.Con_Printf( "\n" );

	gEngfuncs.Con_Printf( "Color %d bits, Alpha %d bits, Depth %d bits, Stencil %d bits\n", glConfig.color_bits,
		glConfig.alpha_bits, glConfig.depth_bits, glConfig.stencil_bits );
}

static void R_RenderInfo_f( void )
{
	R_RenderInfo( false );
}

void GX_InitExtensions( void )
{
	memset( glConfig.extension, 0, sizeof( glConfig.extension ));

	glConfig.extension[GX_HW_BASE] = true;
	glConfig.extension[GX_HW_EFB_TO_TEXTURE] = true;
	glConfig.extension[GX_HW_Z_COMPRESSION] = true;
	glConfig.extension[GX_HW_MULTISAMPLE] = gl_msaa.value ? true : false;

	glConfig.extension[GX_HW_WIDESCREEN] = ( CONF_GetAspectRatio() == CONF_ASPECT_16_9 ) ? true : false;
	glConfig.extension[GX_HW_PROGRESSIVE] = ( gxvid.rmode && FBitSet( gxvid.rmode->viTVMode, VI_NON_INTERLACE )) ? true : false;

	glConfig.extensions_string = "gx_efb_to_texture gx_z_compression gx_copy_filter_aa";
	glConfig.vendor_string = "Nintendo";
	glConfig.renderer_string = "Hollywood (GX)";
	glConfig.version_string = "native";
	glConfig.hardware_type = GXHW_DEVKIT;

	glConfig.max_texture_units = MAX_TEXTURE_UNITS;
	glConfig.max_texture_coords = MAX_TEXTURE_UNITS;
	glConfig.max_teximage_units = MAX_TEXTURE_UNITS;
	glConfig.max_2d_texture_size = 1024;
	glConfig.max_2d_rectangle_size = 1024;
	glConfig.max_2d_texture_layers = 1;
	glConfig.max_3d_texture_size = 0;
	glConfig.max_cubemap_size = 1024;

	glConfig.max_texture_anisotropy = 1.0f;
	glConfig.max_texture_lod_bias = 0.0f;

	glConfig.max_vertex_uniforms = 0;
	glConfig.max_vertex_attribs = MAX_TEXTURE_UNITS + 2;

	glConfig.max_multisamples = glConfig.extension[GX_HW_MULTISAMPLE] ? 1 : 0;
}

void GX_ClearExtensions( void )
{
	memset( glConfig.extension, 0, sizeof( glConfig.extension ));
}

/* Regiones TMEM propias: la callback por defecto de libogc da solo 32KB por texmap,
 * y una textura 256x256 RGB565 (128KB) pisa el TMEM del texmap vecino (lightmap) => arcoiris. */
static GXTexRegion gx_texregions[8];

static GXTexRegion *GX_TexRegionCB( GXTexObj *obj, u8 mapid )
{
	(void)obj;
	return &gx_texregions[mapid & 7];
}

static void GX_InitTexRegions( void )
{
	for( int i = 0; i < 8; i++ )
	{
		u32 off = ( i & 3 ) * 0x20000;
		GX_InitTexCacheRegion( &gx_texregions[i], GX_FALSE,
			off, GX_TEXCACHE_128K,
			0x80000 + off, GX_TEXCACHE_128K );
	}
	GX_SetTexRegionCallback( GX_TexRegionCB );
	GX_InvalidateTexAll();
}
static void GX_InitCommands( void )
{
	gx_verbose = gEngfuncs.Cvar_Get( "gx_verbose", "0", FCVAR_ARCHIVE, "GX debug logs (0=off, 1=on)" );
	gEngfuncs.Cvar_RegisterVariable( &r_lighting_ambient );
	gEngfuncs.Cvar_RegisterVariable( &r_novis );
	gEngfuncs.Cvar_RegisterVariable( &r_nocull );
	gEngfuncs.Cvar_RegisterVariable( &r_detailtextures );
	gEngfuncs.Cvar_RegisterVariable( &r_lockpvs );
	gEngfuncs.Cvar_RegisterVariable( &r_lockfrustum );
	gEngfuncs.Cvar_RegisterVariable( &r_traceglow );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_sort_textures );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_drawelements );
	gEngfuncs.Cvar_RegisterVariable( &r_studio_builtin_renderer );
	gEngfuncs.Cvar_RegisterVariable( &r_ripple );
	gEngfuncs.Cvar_RegisterVariable( &r_ripple_updatetime );
	gEngfuncs.Cvar_RegisterVariable( &r_ripple_spawntime );
	gEngfuncs.Cvar_RegisterVariable( &r_shadows );
	gEngfuncs.Cvar_RegisterVariable( &r_vbo );
	gEngfuncs.Cvar_RegisterVariable( &r_vbo_dlightmode );
	gEngfuncs.Cvar_RegisterVariable( &r_vbo_overbrightmode );
	gEngfuncs.Cvar_RegisterVariable( &r_vbo_detail );
	gEngfuncs.Cvar_RegisterVariable( &r_large_lightmaps );

	gEngfuncs.Cvar_RegisterVariable( &gl_extensions );
	gEngfuncs.Cvar_RegisterVariable( &gl_texture_nearest );
	gEngfuncs.Cvar_RegisterVariable( &gl_lightmap_nearest );
	gEngfuncs.Cvar_RegisterVariable( &gl_check_errors );
	gEngfuncs.Cvar_RegisterVariable( &gl_texture_anisotropy );
	gEngfuncs.Cvar_RegisterVariable( &gl_texture_lodbias );
	gEngfuncs.Cvar_RegisterVariable( &gl_keeptjunctions );
	gEngfuncs.Cvar_RegisterVariable( &gl_finish );
	gEngfuncs.Cvar_RegisterVariable( &gl_nosort );
	gEngfuncs.Cvar_RegisterVariable( &gl_test );
	gEngfuncs.Cvar_RegisterVariable( &gl_wireframe );
	gEngfuncs.Cvar_RegisterVariable( &gl_msaa );
	gEngfuncs.Cvar_RegisterVariable( &gl_stencilbits );
	gEngfuncs.Cvar_RegisterVariable( &gl_round_down );
	gEngfuncs.Cvar_RegisterVariable( &gl_overbright );
	gEngfuncs.Cvar_RegisterVariable( &gl_fog );
	gEngfuncs.Cvar_RegisterVariable( &gl_litwater_force );

	gEngfuncs.Cvar_RegisterVariable( &gl_polyoffset );
	gEngfuncs.Cvar_RegisterVariable( &gl_polyoffset_bmodels );

	/* r_dynamic es FCVAR_ARCHIVE: un 0 guardado en un config.cfg viejo apaga
	   lightstyles y luces dinamicas para siempre (el log mostraba r_dynamic=0).
	   El puerto Wii lo fuerza a 1 en cada arranque. */
	gEngfuncs.Cvar_SetValue( "r_dynamic", 1.0f );

	gEngfuncs.Cmd_AddCommand( "r_info", R_RenderInfo_f, "display renderer info" );
	gEngfuncs.Cmd_AddCommand( "timerefresh", SCR_TimeRefresh_f, "turn quickly and print rendering statistcs" );
}

static void R_CheckVBO( void )
{
	gEngfuncs.Cvar_FullSet( "r_studio_drawelements", "0", FCVAR_READ_ONLY );
}

static void GX_RemoveCommands( void )
{
	gEngfuncs.Cmd_RemoveCommand( "r_info" );
	gEngfuncs.Cmd_RemoveCommand( "timerefresh" );
}

void GX_SetupAttributes( int safegl )
{
	if( gx_video_initialized )
		return;

	GX_InitCommands();
	VIDEO_Init();

	gxvid.rmode = VIDEO_GetPreferredMode( NULL );
}

void GX_OnContextCreated( void )
{
	if( gx_video_initialized )
		return;

	GXRModeObj *rmode = gxvid.rmode;
	GXColor black = { 0, 0, 0, 0xff };

	gxvid.xfb[0] = MEM_K0_TO_K1( SYS_AllocateFramebuffer( rmode ));
	gxvid.xfb[1] = MEM_K0_TO_K1( SYS_AllocateFramebuffer( rmode ));
	gxvid.fb = 0;

	{
		FILE *tf = fopen( "sd:/xash3d/gxtest.txt", "r" );
		if( tf )
		{
			int c = 0;
			if( fscanf( tf, "%d", &c ) == 1 && c >= 0 && c < 100 ) gx_testmode = c;
			fclose( tf );
		}
		gEngfuncs.Con_Printf( "[GX] testmode = %d\n", gx_testmode );
	}

	// console_init() replaces devoptab stdout/stderr with libogc's on-screen
	// console, which draws text with the CPU straight into xfb[0]. GX_Present
	// flips between xfb[0] and xfb[1], so every printf would paint over the
	// rendered frame. Keep the console initialised (it is harmless) but give
	// stdout/stderr back to the SD log.
	const devoptab_t *old_out = devoptab_list[STD_OUT];
	const devoptab_t *old_err = devoptab_list[STD_ERR];

	console_init( gxvid.xfb[0], 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ );
	devoptab_list[STD_OUT] = old_out;
	devoptab_list[STD_ERR] = old_err;

	// An all-zero XFB is green in YCbCr; start the second buffer black.
	VIDEO_ClearFrameBuffer( rmode, gxvid.xfb[1], COLOR_BLACK );


	VIDEO_Configure( rmode );
	VIDEO_SetNextFramebuffer( gxvid.xfb[gxvid.fb] );
	VIDEO_SetBlack( FALSE );
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if( FBitSet( rmode->viTVMode, VI_NON_INTERLACE ))
		VIDEO_WaitVSync();

	gpGlobals->width = rmode->fbWidth;
	gpGlobals->height = rmode->efbHeight;
	gpGlobals->desktopBitsPixel = 24;

	gxvid.fifo = memalign( 32, GX_FIFO_SIZE );
	memset( gxvid.fifo, 0, GX_FIFO_SIZE );
	GX_Init( gxvid.fifo, GX_FIFO_SIZE );
	GX_InitTexRegions();

	GX_SetCopyClear( black, GX_MAX_Z24 );
	GX_SetViewport( 0.0f, 0.0f, (f32)rmode->fbWidth, (f32)rmode->efbHeight, 0.0f, 1.0f );
	GX_SetDispCopyYScale( (f32)rmode->xfbHeight / (f32)rmode->efbHeight );
	GX_SetScissor( 0, 0, rmode->fbWidth, rmode->efbHeight );
	GX_SetDispCopySrc( 0, 0, rmode->fbWidth, rmode->efbHeight );
	GX_SetDispCopyDst( rmode->fbWidth, rmode->xfbHeight );
	GX_SetCopyFilter( rmode->aa, rmode->sample_pattern, gl_msaa.value ? GX_TRUE : GX_FALSE, rmode->vfilter );
	GX_SetFieldMode( rmode->field_rendering,
		(( rmode->viHeight == 2 * rmode->xfbHeight ) ? GX_ENABLE : GX_DISABLE ));

	if( rmode->aa )
		GX_SetPixelFmt( GX_PF_RGB565_Z16, GX_ZC_LINEAR );
	else
		GX_SetPixelFmt( GX_PF_RGB8_Z24, GX_ZC_LINEAR );

	GX_SetDispCopyGamma( GX_GM_1_0 );

	glConfig.color_bits = 24;
	glConfig.alpha_bits = rmode->aa ? 0 : 8;
	glConfig.depth_bits = rmode->aa ? 16 : 24;
	glConfig.stencil_bits = 0;
	glState.stencilEnabled = false;

	glConfig.msaasamples = rmode->aa ? 1 : 0;
	glConfig.version_major = 1;
	glConfig.version_minor = 0;

	glw_state.initialized = true;
	gx_video_initialized = true;

	GX_InitExtensions();
	GX_SetDefaultState();
	GX_SetDefaults();
	gEngfuncs.Con_Printf( "[GX] defaults OK\n" );
}

/* Neutralizado: duplicado con gx_rmain.c */

/* Neutralizado: duplicado con gx_rmain.c */
