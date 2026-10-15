/*
gx_context.c - (Wii GX native port)
Copyright (C) 2018 a1batross
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

#define APIENTRY_LINKAGE
#include "gx_local.h"
#include "gx_export.h"

void R_ClearScreen( void )
{
	R_Set2DMode( true );

	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );

	GXColor black = { 0, 0, 0, 255 };
	GX_SetChanMatColor( GX_COLOR0A0, black );
	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );

	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0 );

	GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );
		GX_Position2f32( 0.0f, 0.0f );
		GX_Position2f32( (f32)gpGlobals->width, 0.0f );
		GX_Position2f32( (f32)gpGlobals->width, (f32)gpGlobals->height );
		GX_Position2f32( 0.0f, (f32)gpGlobals->height );
	GX_End();

	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

	// Seguimos en modo 2D: el HUD no debe pasar por el test de profundidad.
	// Antes esto reactivaba el Z-test, y todo lo que el HUD dibujaba despues
	// de un FillRGBA se comparaba con la profundidad del arma (que ocupa el
	// primer 30% del rango) y quedaba tapado por ella.
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
	R_Set2DMode( false );
}

static const byte *R_GetTextureOriginalBuffer( unsigned int idx )
{
	gl_texture_t *glt = R_GetTexture( idx );

	if( !glt || !glt->original || !glt->original->buffer )
		return NULL;

	return glt->original->buffer;
}

static void CL_FillRGBA( int rendermode, float _x, float _y, float _w, float _h, byte r, byte g, byte b, byte a )
{
	R_Set2DMode( true );

	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );

	GX_SetBlendMode( GX_BM_BLEND,
		(rendermode == kRenderTransAdd) ? GX_BL_SRCALPHA : GX_BL_SRCALPHA,
		(rendermode == kRenderTransAdd) ? GX_BL_ONE : GX_BL_INVSRCALPHA,
		GX_LO_CLEAR );
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );

	GXColor color = { r, g, b, a };
	GX_SetChanMatColor( GX_COLOR0A0, color );
	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );

	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0 );

	GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );
		GX_Position2f32( _x, _y );
		GX_Position2f32( _x + _w, _y );
		GX_Position2f32( _x + _w, _y + _h );
		GX_Position2f32( _x, _y + _h );
	GX_End();

	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
}

static qboolean Mod_LooksLikeWaterTexture( const char *name )
{
	if(( name[0] == '*' && Q_stricmp( name, REF_DEFAULT_TEXTURE )) || name[0] == '!' )
		return true;

	if( !FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
	{
		if( !Q_strncmp( name, "water", 5 ) || !Q_strnicmp( name, "laser", 5 ))
			return true;
	}

	return false;
}

static void Mod_BrushUnloadTextures( model_t *mod )
{
	for( int i = 0; i < mod->numtextures; i++ )
	{
		texture_t *tx = mod->textures[i];
		if( !tx )
			continue;

		if( tx->gl_texturenum != tr.defaultTexture )
			GX_FreeTexture( tx->gl_texturenum );

		if( !Mod_LooksLikeWaterTexture( tx->name ))
		{
			GX_FreeTexture( tx->fb_texturenum );
			GX_FreeTexture( tx->dt_texturenum );
		}
	}
}

static void Mod_UnloadTextures( model_t *mod )
{
	Assert( mod != NULL );

	switch( mod->type )
	{
	case mod_studio:
		Mod_StudioUnloadTextures( mod->cache.data );
		break;
	case mod_alias:
		Mod_AliasUnloadTextures( mod->cache.data );
		break;
	case mod_brush:
		Mod_BrushUnloadTextures( mod );
		break;
	case mod_sprite:
		break;
	default:
		Assert( 0 );
		break;
	}
}

static qboolean Mod_ProcessRenderData( model_t *mod, qboolean create, const byte *buf, size_t buffersize )
{
	qboolean loaded = false;

	if( !create )
	{
		if( gEngfuncs.drawFuncs->Mod_ProcessUserData )
			gEngfuncs.drawFuncs->Mod_ProcessUserData( mod, false, buf );
		Mod_UnloadTextures( mod );
		return true;
	}

	switch( mod->type )
	{
	case mod_studio:
	case mod_brush:
		loaded = true;
		break;
	case mod_sprite:
		loaded = true;
		break;
	case mod_alias:
		R_Mod_LoadAliasModel( mod, buf, &loaded );
		break;
	default:
		gEngfuncs.Host_Error( "%s: unsupported type %d\n", __func__, mod->type );
		return false;
	}

	if( gEngfuncs.drawFuncs->Mod_ProcessUserData )
		gEngfuncs.drawFuncs->Mod_ProcessUserData( mod, true, buf );

	return loaded;
}

static intptr_t GX_RefGetParm( int parm, int arg )
{
	switch( parm )
	{
	case PARM_TEX_WIDTH:
		return R_GetTexture( arg )->width;
	case PARM_TEX_HEIGHT:
		return R_GetTexture( arg )->height;
	case PARM_TEX_SRC_WIDTH:
		return R_GetTexture( arg )->srcWidth;
	case PARM_TEX_SRC_HEIGHT:
		return R_GetTexture( arg )->srcHeight;
	case PARM_TEX_GLFORMAT:
		return R_GetTexture( arg )->format;
	case PARM_TEX_ENCODE:
		return R_GetTexture( arg )->encode;
	case PARM_TEX_MIPCOUNT:
		return R_GetTexture( arg )->numMips;
	case PARM_TEX_DEPTH:
		return R_GetTexture( arg )->depth;
	case PARM_TEX_SKYBOX:
		Assert( arg >= 0 && arg < 6 );
		return tr.skyboxTextures[arg];
	case PARM_TEX_SKYTEXNUM:
		return tr.skytexturenum;
	case PARM_TEX_LIGHTMAP:
		arg = bound( 0, arg, MAX_LIGHTMAPS - 1 );
		return tr.lightmapTextures[arg];
	case PARM_TEX_TARGET:
		return 0x0DE1;
	case PARM_TEX_TEXNUM:
		return R_GetTexture( arg )->texnum;
	case PARM_TEX_FLAGS:
		return R_GetTexture( arg )->flags;
	case PARM_TEX_MEMORY:
		return GX_TexMemory();
	case PARM_ACTIVE_TMU:
		return glState.activeTMU;
	case PARM_LIGHTSTYLEVALUE:
		arg = bound( 0, arg, MAX_LIGHTSTYLES - 1 );
		return g_lightstylevalue[arg];
	case PARM_MAX_IMAGE_UNITS:
		return GL_MaxTextureUnits();
	case PARM_REBUILD_GAMMA:
		return glConfig.softwareGammaUpdate;
	case PARM_GL_CONTEXT_TYPE:
		return 0;
	case PARM_GLES_WRAPPER:
		return 0;
	case PARM_STENCIL_ACTIVE:
		return glState.stencilEnabled;
	case PARM_TEX_FILTERING:
		if( arg < 0 )
			return gl_texture_nearest.value == 0.0f;

		return GX_TextureFilteringEnabled( R_GetTexture( arg ));
	case PARM_GET_STUDIO_HDR:
		return (intptr_t)R_StudioGetHeader();
	default:
		return ENGINE_GET_PARM_( parm, arg );
	}
	return 0;
}

static void R_GetDetailScaleForTexture( int texture, float *xScale, float *yScale )
{
	gl_texture_t *glt = R_GetTexture( texture );

	if( xScale ) *xScale = glt->xscale;
	if( yScale ) *yScale = glt->yscale;
}

static void R_SetDetailScaleForTexture( int texture, float xScale, float yScale )
{
	gl_texture_t *glt = R_GetTexture( texture );

	glt->xscale = xScale;
	glt->yscale = yScale;
}

static void R_GetExtraParmsForTexture( int texture, byte *red, byte *green, byte *blue, byte *density )
{
	gl_texture_t *glt = R_GetTexture( texture );

	if( red ) *red = glt->fogParams[0];
	if( green ) *green = glt->fogParams[1];
	if( blue ) *blue = glt->fogParams[2];
	if( density ) *density = glt->fogParams[3];
}

static void R_SetCurrentEntity( cl_entity_t *ent )
{
	RI.currententity = ent;

	if( RI.currententity != NULL )
	{
		RI.currentmodel = RI.currententity->model;
	}
}

static void R_SetCurrentModel( model_t *mod )
{
	RI.currentmodel = mod;
}

static float R_GetFrameTime( void )
{
	return tr.frametime;
}

static const char *GX_TextureName( unsigned int texnum )
{
	return R_GetTexture( texnum )->name;
}

static const byte *GX_TextureData( unsigned int texnum )
{
	rgbdata_t *pic = R_GetTexture( texnum )->original;

	if( pic != NULL )
		return pic->buffer;
	return NULL;
}

static void R_ProcessEntData( qboolean allocate, cl_entity_t *entities, unsigned int max_entities )
{
	if( !allocate )
	{
		tr.draw_list->num_solid_entities = 0;
		tr.draw_list->num_trans_entities = 0;
		tr.draw_list->num_beam_entities = 0;

		tr.max_entities = 0;
		tr.entities = NULL;
	}
	else
	{
		tr.max_entities = max_entities;
		tr.entities = entities;
	}

	if( gEngfuncs.drawFuncs->R_ProcessEntData )
		gEngfuncs.drawFuncs->R_ProcessEntData( allocate );
}

static void GAME_EXPORT R_SetSkyCloudsTextures( int solidskyTexture, int alphaskyTexture )
{
	tr.solidskyTexture = solidskyTexture;
	tr.alphaskyTexture = alphaskyTexture;
}

static void GAME_EXPORT R_SetupSky( int *skyboxTextures )
{
	R_UnloadSkybox();

	if( !skyboxTextures )
		return;

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
		tr.skyboxTextures[i] = skyboxTextures[i];
}

static qboolean R_SetDisplayTransform( ref_screen_rotation_t rotate, int offset_x, int offset_y, float scale_x, float scale_y )
{
	qboolean ret = true;

	tr.rotation = rotate;

	if( offset_x || offset_y )
	{
		gEngfuncs.Con_Printf("offset transform not supported\n");
		ret = false;
	}

	if( scale_x != 1.0f || scale_y != 1.0f )
	{
		gEngfuncs.Con_Printf("scale transform not supported\n");
		ret = false;
	}

	return ret;
}

static void GAME_EXPORT VGUI_SetupDrawing( qboolean rect )
{
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );

	if( rect )
	{
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	}
	else
	{
		GX_SetAlphaCompare( GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	}
}

static void GAME_EXPORT R_OverrideTextureSourceSize( unsigned int texnum, uint srcWidth, uint srcHeight )
{
	gl_texture_t *tx = R_GetTexture( texnum );

	tx->srcWidth = srcWidth;
	tx->srcHeight = srcHeight;
}

static void* GAME_EXPORT R_GetProcAddress( const char *name )
{
	return NULL;
}

static const char *R_GetConfigName( void )
{
	return "opengl";
}

extern double gx_load_tex_time;
extern int gx_load_tex_count;

static void R_NewMap( void )
{
	double t_start = Platform_DoubleTime();
	double t_lm, t_vbo;
	tr.worldmodel = gp_cl->models[1];

	R_ClearDecals();

	R_StudioResetPlayerModels();

	for( int i = 0; i < WORLDMODEL->numleafs; i++ )
		WORLDMODEL->leafs[i+1].efrags = NULL;

	glState.isFogEnabled = false;
	tr.skytexturenum = -1;

	GXColor black = { 0, 0, 0, 0 };
	GX_SetFog( GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, black );

	for( int i = 0; i < WORLDMODEL->numtextures; i++ )
	{
		if( !WORLDMODEL->textures[i] )
			continue;

		texture_t *tx = WORLDMODEL->textures[i];

		if( !Q_strncmp( tx->name, "sky", 3 ) && tx->width == ( tx->height * 2 ))
			tr.skytexturenum = i;

		tx->texturechain = NULL;
	}

	t_lm = Platform_DoubleTime();
	GX_BuildLightmaps ();
	t_lm = Platform_DoubleTime() - t_lm;

	t_vbo = Platform_DoubleTime();
	R_ClearVBO();
	if( R_HasEnabledVBO( ))
		R_GenerateVBO();
	R_ResetRipples();
	t_vbo = Platform_DoubleTime() - t_vbo;

	if( gEngfuncs.drawFuncs->R_NewMap != NULL )
		gEngfuncs.drawFuncs->R_NewMap();

	/* diagnostico de tiempos de carga (siempre activo, una linea por mapa) */
	gEngfuncs.Con_Printf( "[LOAD] R_NewMap=%.0fms (lightmaps+VBO=%.0fms, otros=%.0fms) | texturas desde el mapa anterior: %d cargas, %.0fms\n",
		( Platform_DoubleTime() - t_start ) * 1000.0, ( t_lm + t_vbo ) * 1000.0,
		( Platform_DoubleTime() - t_start - t_lm - t_vbo ) * 1000.0,
		gx_load_tex_count, gx_load_tex_time * 1000.0 );
	gx_load_tex_count = 0;
	gx_load_tex_time = 0.0;
}

static void R_FillRenderAPI( render_api_t *api )
{
	api->GetExtraParmsForTexture  = R_GetExtraParmsForTexture;
	api->GetFrameTime             = R_GetFrameTime;
	api->R_SetCurrentEntity       = R_SetCurrentEntity;
	api->R_SetCurrentModel        = R_SetCurrentModel;
	api->GL_CreateTexture         = GX_CreateTexture;
	api->GL_LoadTextureArray      = GX_LoadTextureArray;
	api->GL_CreateTextureArray    = GX_CreateTextureArray;
	api->DrawSingleDecal          = DrawSingleDecal;
	api->R_DecalSetupVerts        = R_DecalSetupVerts;
	api->R_EntityRemoveDecals     = R_EntityRemoveDecals;
	api->GL_SelectTexture         = GX_SelectTexture;
	api->GL_LoadTextureMatrix     = GX_LoadTexMatrix;
	api->GL_TexMatrixIdentity     = GX_LoadIdentityTexMatrix;
	api->GL_CleanUpTextureUnits   = GX_CleanUpTextureUnits;
	api->GL_TexGen                = GX_SetTexCoordGen2f;
	api->GL_TextureTarget         = GX_SetTexCoordArrayMode;
	api->GL_TexCoordArrayMode     = GX_SetTexCoordArrayMode;
	api->GL_UpdateTexSize         = GX_UpdateTexSize;
	api->GL_DrawParticles         = CL_DrawParticlesExternal;
	api->LightVec                 = R_LightVec;
	api->StudioGetTexture         = R_StudioGetTexture;
	api->GL_GetProcAddress        = R_GetProcAddress;
}

static void R_FillTriAPI( triangleapi_t *api )
{
	api->TexCoord2f    = TriTexCoord2f;
	api->Fog           = TriFog;
	api->ScreenToWorld = R_ScreenToWorld;
	api->GetMatrix     = TriGetMatrix;
	api->FogParams     = TriFogParams;
}

/*
===============
GX_R_Init

GX-specific initialisation. Named GX_R_Init (not R_Init) to avoid
colliding with libref_soft.a's R_Init, which the linker was pulling
in because libref_gx.a did not define its own R_Init. That made
gReffuncs.R_Init resolve to the software renderer's setup, which
immediately failed (no software framebuffer) and killed the process.
===============
*/
qboolean GX_R_Init( void )
{
	Con_Printf( "[GX] GX_R_Init entered\n" );
	GL_InitRandomTable();

	r_temppool = Mem_AllocPool( "Render Zone" );

	// Bring up the video backend through the engine
	if( !gEngfuncs.R_Init_Video( REF_GL ))
	{
		gEngfuncs.R_Free_Video();
		Mem_FreePool( &r_temppool );
		return false;
	}
	GX_SetupAttributes( 0 );
	GX_OnContextCreated();
	// Fill render globals (same as GL/soft do)
	tr.world = (struct world_static_s *)ENGINE_GET_PARM( PARM_GET_WORLD_PTR );
	tr.palette = (color24 *)ENGINE_GET_PARM( PARM_GET_PALETTE_PTR );
	tr.viewent = (cl_entity_t *)ENGINE_GET_PARM( PARM_GET_VIEWENT_PTR );
	tr.texgammatable = (byte *)ENGINE_GET_PARM( PARM_GET_TEXGAMMATABLE_PTR );
	tr.lightgammatable = (uint *)ENGINE_GET_PARM( PARM_GET_LIGHTGAMMATABLE_PTR );
	tr.screengammatable = (uint *)ENGINE_GET_PARM( PARM_GET_SCREENGAMMATABLE_PTR );
	tr.lineargammatable = (uint *)ENGINE_GET_PARM( PARM_GET_LINEARGAMMATABLE_PTR );
	tr.elights = (dlight_t *)ENGINE_GET_PARM( PARM_GET_ELIGHTS_PTR );

	// Init GX subsystems
	R_InitImages();
	R_StudioInit();
	R_AliasInit();
	R_ClearDecals();
	R_ClearScene();

	return true;
}

const ref_interface_t gReffuncs =
{
	GX_R_Init,
	R_Shutdown,
	R_GetConfigName,
	R_SetDisplayTransform,

	GX_SetupAttributes,
	GX_InitExtensions,
	GX_ClearExtensions,

	R_GammaChanged,
	R_BeginFrame,
	R_RenderScene,
	R_EndFrame,
	R_PushScene,
	R_PopScene,
	GX_BackendStartFrame,
	GX_BackendEndFrame,

	R_ClearScreen,
	R_AllowFog,
	GX_SetRenderMode,

	R_AddEntity,
	R_ProcessEntData,

	R_ShowTextures,

	R_GetTextureOriginalBuffer,
	GX_LoadTextureFromBuffer,
	GX_ProcessTexture,
	R_SetupSky,

	R_Set2DMode,
	R_DrawStretchPic,
	CL_FillRGBA,
	R_WorldToScreen,

	VID_ScreenShot,
	VID_CubemapShot,

	R_LightPoint,

	R_DecalShoot,
	R_DecalRemoveAll,
	R_CreateDecalList,
	R_ClearAllDecals,

	R_StudioEstimateFrame,
	R_StudioLerpMovement,
	R_StudioFillAPI,
	R_StudioSetDrawInterface,

	R_SetSkyCloudsTextures,
	GX_SubdivideSurface,
	CL_RunLightStyles,

	Mod_ProcessRenderData,
	Mod_StudioLoadTextures,

	CL_DrawParticles,
	CL_DrawTracers,
	CL_DrawBeams,

	GX_RefGetParm,

	R_GetDetailScaleForTexture,
	R_SetDetailScaleForTexture,

	GX_CreateTexture,
	GX_FindTexture,
	GX_TextureName,
	GX_TextureData,
	GX_LoadTexture,
	GX_FreeTexture,
	R_OverrideTextureSourceSize,

	GX_UpdateTexture,

	GX_Bind,

	R_RenderFrame,
	Mod_SetOrthoBounds,
	R_SpeedsMessage,
	Mod_GetCurrentVis,
	R_NewMap,
	R_ClearScene,

	TriRenderMode,
	TriBegin,
	TriEnd,
	_TriColor4f,
	_TriColor4ub,
	TriVertex3fv,
	TriVertex3f,
	TriCullFace,

	R_FillRenderAPI,
	R_FillTriAPI,

	VGUI_SetupDrawing,
};



