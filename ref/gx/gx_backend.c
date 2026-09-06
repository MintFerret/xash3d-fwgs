/*
gx_backend.c - rendering backend (Wii GX native port)
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
#include "xash3d_mathlib.h"
#include <string.h>

// Definición de la variable global (solo estaba declarada en el header)
float gx_pending_texcoord[MAX_TEXTURE_UNITS][2];

static inline int tmu_to_gx_texmap( int tmu )
{
	return ( GX_TEXMAP0 + tmu );
}

static char r_speeds_msg[MAX_SYSPATH];
ref_speeds_t r_stats;

qboolean R_SpeedsMessage( char *out, size_t size )
{
	if( gEngfuncs.drawFuncs->R_SpeedsMessage != NULL )
	{
		if( gEngfuncs.drawFuncs->R_SpeedsMessage( out, size ))
			return true;
	}

	if( r_speeds->value <= 0 ) return false;
	if( !out || !size ) return false;

	Q_strncpy( out, r_speeds_msg, size );

	return true;
}

void GX_BackendStartFrame( void )
{
	r_speeds_msg[0] = '\0';
}

void GX_BackendEndFrame( void )
{
	if( r_speeds->value <= 0 || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	mleaf_t *curleaf = RI.viewleaf ? RI.viewleaf : WORLDMODEL->leafs;

	switch( (int)r_speeds->value )
	{
	case 1:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ), "%3i wpoly, %3i apoly\n%3i epoly, %3i spoly",
			r_stats.c_world_polys, r_stats.c_alias_polys, r_stats.c_studio_polys, r_stats.c_sprite_polys );
		break;
	case 2:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ),
			"Renderer: ^1Wii GX^7\n\n"
			"visible leafs:\n%3i leafs\ncurrent leaf %3i\n"
			"RecursiveWorldNode: %3lf secs\nDrawTextureChains %lf",
			r_stats.c_world_leafs, (int)( curleaf - WORLDMODEL->leafs ), r_stats.t_world_node, r_stats.t_world_draw );
		break;
	case 3:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ), "%3i alias models drawn\n%3i studio models drawn\n%3i sprites drawn",
			r_stats.c_alias_models_drawn, r_stats.c_studio_models_drawn, r_stats.c_sprite_models_drawn );
		break;
	case 4:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ), "%3i static entities\n%3i normal entities\n%3i server entities",
			r_numStatics, r_numEntities - r_numStatics, (int)ENGINE_GET_PARM( PARM_NUMENTITIES ));
		break;
	case 5:
		Q_snprintf( r_speeds_msg, sizeof( r_speeds_msg ), "%3i tempents\n%3i viewbeams\n%3i particles",
			r_stats.c_active_tents_count, r_stats.c_view_beams_count, r_stats.c_particle_count );
		break;
	}

	memset( &r_stats, 0, sizeof( r_stats ));
}

void GX_LoadTexMatrix( int tmu, const float *matrix )
{
	Assert( matrix != NULL );
	Assert( tmu >= 0 && tmu < MAX_TEXTURE_UNITS );

	Mtx34 gxmtx;
	for( int r = 0; r < 3; r++ )
		for( int c = 0; c < 4; c++ )
			gxmtx[r][c] = matrix[c * 4 + r];   // transpuesta respecto a column-major

	int slot = GX_TEXMTX0 + tmu * 3;
	GX_LoadTexMtxImm( gxmtx, slot, GX_MTX3x4 );

	glState.texIdentityMatrix[tmu] = false;
}

void GX_LoadMatrix( const matrix4x4 source )
{
	Mtx gxmtx;
	for( int r = 0; r < 3; r++ )
		for( int c = 0; c < 4; c++ )
			gxmtx[c][r] = source[r][c];   // misma transpuesta que en LoadTexMatrix

	GX_LoadPosMtxImm( gxmtx, GX_PNMTX0 );
}

void GX_LoadIdentityTexMatrix( void )
{
	int tmu = glState.activeTMU;

	if( glState.texIdentityMatrix[tmu] )
		return;

	GX_SetTexCoordGen2( GX_TEXCOORD0 + tmu, GX_TG_MTX2x4, GX_TG_TEX0 + tmu, GX_IDENTITY, GX_FALSE, GX_DTTIDENTITY );

	glState.texIdentityMatrix[tmu] = true;
}

void GX_SelectTexture( int tmu )
{
	if( tmu < 0 )
		return;

	if( tmu >= glConfig.max_texture_units )
	{
		gEngfuncs.Con_Reportf( S_ERROR "%s: bad tmu state %i\n", __func__, tmu );
		return;
	}

	glState.activeTMU = tmu;
}

void GX_Bind( int tmu, unsigned int texnum )
{
	if( texnum <= 0 || texnum >= MAX_TEXTURES )
	{
		if( texnum != 0 )
			gEngfuncs.Con_DPrintf( S_ERROR "%s: invalid texturenum %d\n", __func__, texnum );
		texnum = tr.defaultTexture;
	}

	if( tmu != GL_KEEP_UNIT )
		GX_SelectTexture( tmu );
	else
		tmu = glState.activeTMU;

	const gl_texture_t *texture = R_GetTexture( texnum );

	if( glState.currentTexturesIndex[tmu] == (int)texnum )
		return;

	int texmap = tmu_to_gx_texmap( tmu );
	GX_LoadTexObj( (GXTexObj *)&texture->texObj, texmap );

	glState.currentTextures[tmu] = (GXTexObj *)&texture->texObj;
	glState.currentTexturesIndex[tmu] = texnum;
	glState.currentTextureFormats[tmu] = texture->format;
}

void GX_EnableTextureUnit( int tmu, qboolean enable )
{
	if( tmu < 0 || tmu >= glConfig.max_texture_units )
		return;

	int coord = GX_TEXCOORD0 + tmu;
	int texmap = tmu_to_gx_texmap( tmu );
	int stage  = GX_TEVSTAGE0 + tmu;

	if( enable )
	{
		GX_SetTevOrder( stage, coord, texmap, GX_COLOR0A0 );
		GX_SetTevOp( stage, GX_MODULATE );
	}
	else
	{
		GX_SetTevOrder( stage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
		GX_SetTevOp( stage, GX_PASSCLR );
	}
}

void GX_CleanUpTextureUnits( int last )
{
	for( int i = glState.activeTMU; i > (last - 1); i-- )
	{
		if( glState.currentTexturesIndex[i] != 0 )
		{
			GX_EnableTextureUnit( i, false );
			glState.currentTextures[i] = NULL;
			glState.currentTexturesIndex[i] = 0;
			glState.currentTextureFormats[i] = 0;
		}

		GX_SelectTexture( i );
		GX_LoadIdentityTexMatrix();
		GX_DisableAllTexGens();

		if( i > 0 )
			GX_SelectTexture( i - 1 );
	}
}

void GX_CleanupAllTextureUnits( void )
{
	if( !glw_state.initialized ) return;
	GX_SelectTexture( glConfig.max_texture_units - 1 );
	GX_CleanUpTextureUnits( 0 );
}

void GX_MultiTexCoord2f( int tmu, float s, float t )
{
	if( tmu < 0 || tmu >= MAX_TEXTURE_UNITS )
		return;

	gx_pending_texcoord[tmu][0] = s;
	gx_pending_texcoord[tmu][1] = t;
}

void GX_DisableAllTexGens( void )
{
	int tmu = glState.activeTMU;

	GX_SetTexCoordGen2( GX_TEXCOORD0 + tmu, GX_TG_MTX2x4, GX_TG_TEX0 + tmu, GX_IDENTITY, GX_FALSE, GX_DTTIDENTITY );

	glState.genSTEnabled[tmu] = 0;
}

void GX_SetTexCoordArrayMode( int tmu, qboolean enable )
{
	int bit = enable ? 1 : 0;
	int cmode = glState.texCoordArrayMode[tmu];

	if( cmode == bit )
		return;

	int coord = GX_TEXCOORD0 + tmu;

	if( enable )
	{
		GX_SetTexCoordGen2( coord, GX_TG_MTX2x4, GX_TG_TEX0 + tmu, GX_IDENTITY, GX_FALSE, GX_DTTIDENTITY );
	}
	else
	{
		GX_SetTexCoordGen2( coord, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_DTTIDENTITY );
	}

	glState.texCoordArrayMode[tmu] = bit;
}

void GX_Cull( int cull )
{
	u8 gxMode;

	if( cull == XASH_CULL_NONE )
	{
		gxMode = GX_CULL_NONE;
	}
	else if( cull == XASH_CULL_FRONT )
	{
		gxMode = GX_CULL_FRONT;
	}
	else
	{
		gxMode = GX_CULL_BACK;
	}

	GX_SetCullMode( gxMode );
	glState.faceCull = cull;
}

void GX_DrawIndexedPrimitive( u8 primType, const void *verts, int numVerts, const u16 *indices, int numIndices )
{
	const drawvert_t *v = (const drawvert_t *)verts;

	GX_Begin( primType, GX_VTXFMT0, numIndices );

	for( int i = 0; i < numIndices; i++ )
	{
		const drawvert_t *dv = &v[indices[i]];
		GX_Position3f32( dv->xyz[0], dv->xyz[1], dv->xyz[2] );
		GX_TexCoord2f32( dv->st[0],  dv->st[1]  );
	}

	GX_End();
}

void GX_SetRenderMode( int mode )
{
	switch( mode )
	{
	case kRenderNormal:
	default:
		R_AllowFog( true );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		break;

	case kRenderTransColor:
	case kRenderTransTexture:
		R_AllowFog( true );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		break;

	case kRenderTransAlpha:
		R_AllowFog( true );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetAlphaCompare( GX_GEQUAL, 128, GX_AOP_AND, GX_ALWAYS, 0 );
		break;

	case kRenderGlow:
	case kRenderTransAdd:
		R_AllowFog( false );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR );
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		break;

	case kRenderScreenFadeModulate:
		R_AllowFog( true );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_ZERO, GX_BL_SRCCLR, GX_LO_CLEAR );
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		break;
	}
}

void GX_PushPolygonOffset( float factor, float units )
{
	if( glState.num_polyoffsets >= ARRAYSIZE( glState.polyoffset_state ))
	{
		gEngfuncs.Con_Reportf( "%s: overflow\n", __func__ );
		return;
	}

	glState.polyoffset_state[glState.num_polyoffsets].factor = factor;
	glState.polyoffset_state[glState.num_polyoffsets].units  = units;
	glState.num_polyoffsets++;

	const float z_bias_scale = 1.0f / 65536.0f;
	float total_bias = 0.0f;
	for( int i = 0; i < glState.num_polyoffsets; i++ )
		total_bias += glState.polyoffset_state[i].units * z_bias_scale;

	GX_ReloadProjectionMtx( total_bias );
}

void GX_PopPolygonOffset( void )
{
	if( glState.num_polyoffsets <= 0 )
	{
		gEngfuncs.Con_Reportf( "%s: underflow\n", __func__ );
		return;
	}

	glState.num_polyoffsets--;

	float total_bias = 0.0f;
	const float z_bias_scale = 1.0f / 65536.0f;
	for( int i = 0; i < glState.num_polyoffsets; i++ )
		total_bias += glState.polyoffset_state[i].units * z_bias_scale;

	GX_ReloadProjectionMtx( total_bias );
}

typedef struct envmap_s
{
	vec3_t	angles;
	int	flags;
} envmap_t;

static const envmap_t r_skyBoxInfo[6] =
{
{{   0, 270, 180}, IMAGE_FLIP_X },
{{   0,  90, 180}, IMAGE_FLIP_X },
{{ -90,   0, 180}, IMAGE_FLIP_X },
{{  90,   0, 180}, IMAGE_FLIP_X },
{{   0,   0, 180}, IMAGE_FLIP_X },
{{   0, 180, 180}, IMAGE_FLIP_X },
};

static const envmap_t r_envMapInfo[6] =
{
{{  0,   0,  90}, 0 },
{{  0, 180, -90}, 0 },
{{  0,  90,   0}, 0 },
{{  0, 270, 180}, 0 },
{{-90, 180, -90}, 0 },
{{ 90,   0,  90}, 0 }
};

qboolean VID_ScreenShot( const char *filename, int shot_type )
{
	uint flags = IMAGE_FLIP_Y;
	int  width = 0, height = 0;

	rgbdata_t *r_shot = Mem_Calloc( r_temppool, sizeof( rgbdata_t ));
	r_shot->width  = (gpGlobals->width  + 3) & ~3;
	r_shot->height = (gpGlobals->height + 3) & ~3;
	r_shot->flags  = IMAGE_HAS_COLOR;
	r_shot->type   = PF_RGBA_32;
	r_shot->size   = r_shot->width * r_shot->height * gEngfuncs.Image_GetPFDesc( r_shot->type )->bpp;
	r_shot->palette = NULL;

	r_shot->buffer = (byte *)memalign( 32, r_shot->size );
	if( !r_shot->buffer )
	{
		Mem_Free( r_shot );
		return false;
	}

	GX_ReadPixelsRGBA( 0, 0, r_shot->width, r_shot->height, r_shot->buffer );

	switch( shot_type )
	{
	case VID_SCREENSHOT:
		break;
	case VID_SNAPSHOT:
		gEngfuncs.fsapi->AllowDirectPaths( true );
		break;
	case VID_LEVELSHOT:
	case VID_MINISHOT:
		flags |= IMAGE_RESAMPLE;
		height = shot_type == VID_MINISHOT ? 200 : 480;
		width  = Q_rint( height * ((double)r_shot->width / r_shot->height));
		break;
	case VID_MAPSHOT:
		flags |= IMAGE_RESAMPLE | IMAGE_QUANTIZE;
		height = 768;
		width  = 1024;
		break;
	}

	gEngfuncs.Image_Process( &r_shot, width, height, flags, 0.0f );

	qboolean result = gEngfuncs.FS_SaveImage( filename, r_shot );
	gEngfuncs.fsapi->AllowDirectPaths( false );
	free( r_shot->buffer );
	r_shot->buffer = NULL;
	gEngfuncs.FS_FreeImage( r_shot );

	return result;
}

qboolean VID_CubemapShot( const char *base, uint size, const float *vieworg, qboolean skyshot )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) || !WORLDMODEL )
		return false;

	int i = 1;
	while( i < (int)size ) i <<= 1;
	if( i != (int)size ) return false;
	if( (int)size > gpGlobals->width || (int)size > gpGlobals->height )
		return false;

	byte     *temp   = (byte *)memalign( 32, size * size * 4 );
	byte     *buffer = (byte *)Mem_Malloc( r_temppool, size * size * 3 * 6 );
	rgbdata_t *r_shot = Mem_Calloc( r_temppool, sizeof( rgbdata_t ));
	rgbdata_t *r_side = Mem_Calloc( r_temppool, sizeof( rgbdata_t ));

	if( !vieworg ) vieworg = RI.rvp.vieworigin;

	for( i = 0; i < 6; i++ )
	{
		R_Set2DMode( false );

		int flags;
		if( skyshot )
		{
			R_DrawCubemapView( vieworg, r_skyBoxInfo[i].angles, size );
			flags = r_skyBoxInfo[i].flags;
		}
		else
		{
			R_DrawCubemapView( vieworg, r_envMapInfo[i].angles, size );
			flags = r_envMapInfo[i].flags;
		}

		GX_ReadPixelsRGBA( 0, 0, size, size, temp );

		byte *dst = buffer + (size * size * 3 * i);
		byte *src = temp;
		for( uint p = 0; p < size * size; p++, src += 4, dst += 3 )
		{
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
		}

		r_side->flags  = IMAGE_HAS_COLOR;
		r_side->width  = r_side->height = size;
		r_side->type   = PF_RGB_24;
		r_side->size   = size * size * 3;
		r_side->buffer = buffer + (size * size * 3 * i);

		if( flags ) gEngfuncs.Image_Process( &r_side, 0, 0, flags, 0.0f );
	}

	r_shot->flags   = IMAGE_HAS_COLOR | (skyshot ? IMAGE_SKYBOX : IMAGE_CUBEMAP);
	r_shot->width   = size;
	r_shot->height  = size;
	r_shot->type    = PF_RGB_24;
	r_shot->size    = size * size * 3 * 6;
	r_shot->palette = NULL;
	r_shot->buffer  = buffer;

	string basename;
	Q_strncpy( basename, base, sizeof( basename ));
	COM_ReplaceExtension( basename, ".tga", sizeof( basename ));

	int result = gEngfuncs.FS_SaveImage( basename, r_shot );

	free( temp );
	gEngfuncs.FS_FreeImage( r_shot );
	gEngfuncs.FS_FreeImage( r_side );

	return result;
}

void SCR_TimeRefresh_f( void )
{
	if( ENGINE_GET_PARM( PARM_CONNSTATE ) != ca_active )
		return;

	double start = gEngfuncs.pfnTime();

	if( gEngfuncs.Cmd_Argc() == 1 )
	{
		for( int i = 0; i < 128; i++ )
		{
			gpGlobals->viewangles[1] = i / 128.0f * 360.0f;
			R_RenderScene();
		}
		GX_DrawDone();
		R_EndFrame();
	}
	else
	{
		for( int i = 0; i < 128; i++ )
		{
			R_BeginFrame( true );
			gpGlobals->viewangles[1] = i / 128.0f * 360.0f;
			R_RenderScene();
			R_EndFrame();
		}
	}

	double stop = gEngfuncs.pfnTime();
	double time = (stop - start);
	gEngfuncs.Con_Printf( "%f seconds (%f fps)\n", time, 128 / time );
}