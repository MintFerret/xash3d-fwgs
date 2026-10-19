/*
gx_triapi.c - TriAPI draw methods (Wii GX native port)
Copyright (C) 2011 Uncle Mike
Copyright (C) 2019 a1batross
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
#include "const.h"

#define MAX_TRI_VERTS 8192

static struct
{
	int	renderMode;
	vec4_t	triRGBA;
	vec4_t	currentColor;
	int	primitive;
	int	numVerts;
	vec2_t	currentTex;

	struct
	{
		vec3_t pos;
		vec2_t tex;
		byte   color[4];
	} verts[MAX_TRI_VERTS];
} ds;

static inline void FloatColorToBytes( const float *f, byte *b )
{
	b[0] = (u8)(f[0] * 255.0f);
	b[1] = (u8)(f[1] * 255.0f);
	b[2] = (u8)(f[2] * 255.0f);
	b[3] = (u8)(f[3] * 255.0f);
}

static qboolean Tri_CheckVertexCount( void )
{
	int needed;

	switch( ds.primitive )
	{
	case GX_POINTS:
		needed = 1;
		break;
	case GX_LINES:
		needed = 2;
		break;
	case GX_TRIANGLES:
		needed = 3;
		break;
	case GX_QUADS:
		needed = 4;
		break;
	case GX_TRIANGLESTRIP:
	case GX_TRIANGLEFAN:
		needed = 3;
		break;
	default:
		return true;
	}

	if( ds.numVerts < needed )
	{
		gEngfuncs.Con_DPrintf( S_WARN "%s: not enough vertices for primitive (needed %d, have %d)\n",
			__func__, needed, ds.numVerts );
		return false;
	}

	switch( ds.primitive )
	{
	case GX_POINTS:
	case GX_LINES:
	case GX_TRIANGLES:
	case GX_QUADS:
		if( ds.numVerts % needed != 0 )
		{
			gEngfuncs.Con_DPrintf( S_WARN "%s: vertex count not multiple of %d for primitive\n",
				__func__, needed );
			return false;
		}
		break;
	default:
		break;
	}

	return true;
}

void TriRenderMode( int mode )
{
	// diagnostico: efectos sin test de profundidad
	if( gx_fx_noz.value && mode != kRenderNormal )
		GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );

	ds.renderMode = mode;
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

	switch( mode )
	{
	case kRenderNormal:
		R_AllowFog( true );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		break;
	case kRenderTransAlpha:
		R_AllowFog( true );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		break;
	case kRenderTransColor:
	case kRenderTransTexture:
		R_AllowFog( true );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		break;
	case kRenderGlow:
		R_AllowFog( false );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR );
		GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
		break;
	case kRenderTransAdd:
		R_AllowFog( false );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		break;
	}

	// el HUD tambien usa TriAPI: en 2D nunca hay test de profundidad
	if( glState.in2DMode )
		GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
}

void TriBegin( int mode )
{
	switch( mode )
	{
	case TRI_POINTS:         ds.primitive = GX_POINTS; break;
	case TRI_TRIANGLES:      ds.primitive = GX_TRIANGLES; break;
	case TRI_TRIANGLE_FAN:   ds.primitive = GX_TRIANGLEFAN; break;
	case TRI_QUADS:          ds.primitive = GX_QUADS; break;
	case TRI_LINES:          ds.primitive = GX_LINES; break;
	case TRI_TRIANGLE_STRIP: ds.primitive = GX_TRIANGLESTRIP; break;
	case TRI_QUAD_STRIP:     ds.primitive = GX_TRIANGLESTRIP; break; /* mismo orden de vertices que GL_QUAD_STRIP */
	case TRI_POLYGON:
	default:                 ds.primitive = GX_TRIANGLEFAN; break;
	}

	ds.numVerts = 0;
	Vector2Set( ds.currentTex, 0.0f, 0.0f );
}

void TriEnd( void )
{
	if( ds.numVerts == 0 )
		return;

	if( !Tri_CheckVertexCount() )
	{
		ds.numVerts = 0;
		return;
	}

	GX_SetChanVtxColor( true );
	GX_SetNumTevStages( 1 );
    // las coordenadas de textura salen siempre del primer juego de UVs de este vertice
    GX_SetNumTexGens( 1 );
    GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS,  GX_DIRECT );
	GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0 );

	GX_Begin( ds.primitive, GX_VTXFMT0, ds.numVerts );

	for( int i = 0; i < ds.numVerts; i++ )
	{
		GX_Position3f32( ds.verts[i].pos[0], ds.verts[i].pos[1], ds.verts[i].pos[2] );
		GX_Color4u8( ds.verts[i].color[0], ds.verts[i].color[1], ds.verts[i].color[2], ds.verts[i].color[3] );
		GX_TexCoord2f32( ds.verts[i].tex[0], ds.verts[i].tex[1] );
	}

	GX_End();

	ds.numVerts = 0;
}

void _TriColor4f( float r, float g, float b, float a )
{
ds.currentColor[0] = r;
ds.currentColor[1] = g;
ds.currentColor[2] = b;
ds.currentColor[3] = a;

GXColor color =
{
(byte)( r * 255.0f ),
(byte)( g * 255.0f ),
(byte)( b * 255.0f ),
(byte)( a * 255.0f ),
};

GX_SetChanMatColor( GX_COLOR0A0, color );
}

void _TriColor4ub( byte r, byte g, byte b, byte a )
{
ds.currentColor[0] = r * (1.0f / 255.0f);
ds.currentColor[1] = g * (1.0f / 255.0f);
ds.currentColor[2] = b * (1.0f / 255.0f);
ds.currentColor[3] = a * (1.0f / 255.0f);

GXColor color = { r, g, b, a };
GX_SetChanMatColor( GX_COLOR0A0, color );
}

void TriColor4ub( byte r, byte g, byte b, byte a )
{
	ds.triRGBA[0] = r * (1.0f / 255.0f);
	ds.triRGBA[1] = g * (1.0f / 255.0f);
	ds.triRGBA[2] = b * (1.0f / 255.0f);
	ds.triRGBA[3] = a * (1.0f / 255.0f);

	_TriColor4f( ds.triRGBA[0], ds.triRGBA[1], ds.triRGBA[2], 1.0f );
}

void TriColor4f( float r, float g, float b, float a )
{
	if( ds.renderMode == kRenderTransAlpha )
		TriColor4ub( r * 255.9f, g * 255.9f, b * 255.9f, a * 255.0f );
	else
		_TriColor4f( r * a, g * a, b * a, 1.0f );

	ds.triRGBA[0] = r;
	ds.triRGBA[1] = g;
	ds.triRGBA[2] = b;
	ds.triRGBA[3] = a;
}

void TriTexCoord2f( float u, float v )
{
	ds.currentTex[0] = u;
	ds.currentTex[1] = v;
}

void TriVertex3fv( const float *v )
{
	if( ds.numVerts >= MAX_TRI_VERTS )
	{
		gEngfuncs.Con_Reportf( S_ERROR "%s: vertex buffer overflow\n", __func__ );
		return;
	}

	VectorCopy( v, ds.verts[ds.numVerts].pos );
	Vector2Copy( ds.currentTex, ds.verts[ds.numVerts].tex );
	FloatColorToBytes( ds.currentColor, ds.verts[ds.numVerts].color );

	ds.numVerts++;
}

void TriVertex3f( float x, float y, float z )
{
	vec3_t v = { x, y, z };
	TriVertex3fv( v );
}

int TriWorldToScreen( const float *world, float *screen )
{
	int retval = R_WorldToScreen( world, screen );

	screen[0] =  0.5f * screen[0] * (float)RI.rvp.viewport[2];
	screen[1] = -0.5f * screen[1] * (float)RI.rvp.viewport[3];
	screen[0] += 0.5f * (float)RI.rvp.viewport[2];
	screen[1] += 0.5f * (float)RI.rvp.viewport[3];

	return retval;
}

int TriSpriteTexture( model_t *pSpriteModel, int frame )
{
	if( !pSpriteModel || pSpriteModel->type != mod_sprite || !pSpriteModel->cache.data )
		return 0;

	int gl_texturenum = gEngfuncs.R_GetSpriteFrame( pSpriteModel, frame, 0.0f )->gl_texturenum;
	if( gl_texturenum == 0 )
		return 0;

	if( gl_texturenum <= 0 || gl_texturenum >= MAX_TEXTURES )
		gl_texturenum = tr.defaultTexture;

	GX_Bind( XASH_TEXTURE0, gl_texturenum );

	return 1;
}

void TriFog( float flFogColor[3], float flStart, float flEnd, int bOn )
{
	if( RI.fogEnabled || !gl_fog.value )
		return;
	RI.fogCustom = bOn;

	if( flEnd <= flStart )
	{
		glState.isFogEnabled = RI.fogCustom = false;
		GX_SetFog( GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, (GXColor){0,0,0,0} );
		return;
	}

	RI.fogColor[0] = flFogColor[0] / 255.0f;
	RI.fogColor[1] = flFogColor[1] / 255.0f;
	RI.fogColor[2] = flFogColor[2] / 255.0f;
	RI.fogColor[3] = 1.0f;

	RI.fogStart = flStart;
	RI.fogEnd = flEnd;

	GXColor fogColor = {
		(u8)(RI.fogColor[0] * 255.0f),
		(u8)(RI.fogColor[1] * 255.0f),
		(u8)(RI.fogColor[2] * 255.0f),
		(u8)(RI.fogColor[3] * 255.0f)
	};

	if( RI.fogDensity > 0.0f )
	{
		float endZ = ( RI.fogDensity > 0.0f ) ? ( 4.605f / RI.fogDensity ) : 8192.0f;
		GX_SetFog( GX_FOG_EXP2, 0.0f, endZ, 0.0f, 8192.0f, fogColor );
	}
	else
	{
		GX_SetFog( GX_FOG_LIN, flStart, flEnd, 0.0f, 8192.0f, fogColor );
	}

	glState.isFogEnabled = RI.fogCustom;
}

void TriGetMatrix( const int pname, float *matrix )
{
}

void TriFogParams( float flDensity, int iFogSkybox )
{
	RI.fogDensity = flDensity;
	RI.fogSkybox = iFogSkybox;
}

void TriCullFace( TRICULLSTYLE mode )
{
	int glMode;

	switch( mode )
	{
	case TRI_FRONT:
		glMode = XASH_CULL_FRONT;
		break;
	default:
		glMode = XASH_CULL_NONE;
		break;
	}

	GX_Cull( glMode );
}

void TriBrightness( float brightness )
{
	float r = ds.triRGBA[0] * ds.triRGBA[3] * brightness;
	float g = ds.triRGBA[1] * ds.triRGBA[3] * brightness;
	float b = ds.triRGBA[2] * ds.triRGBA[3] * brightness;

	_TriColor4f( r, g, b, 1.0f );
}
