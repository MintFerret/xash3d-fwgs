/*
gx_draw.c - orthogonal drawing stuff (Wii GX port)
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
#include <malloc.h>
#include <gccore.h>
#include <ogc/gx.h>

extern float gldepthmin, gldepthmax;

void R_GetTextureParms( int *w, int *h, int texnum )
{
	gl_texture_t *glt = R_GetTexture( texnum );

	if( w ) *w = glt->srcWidth;
	if( h ) *h = glt->srcHeight;
}

void GX_DbgWait( const char *tag )
{
	static int calls;
	static u16 token = 0x100;
	int i;

	if( calls++ >= 8 )
		return;

	token++;
	GX_SetDrawSync( token );
	GX_Flush();

	for( i = 0; i < 200; i++ )
	{
		if( GX_GetDrawSync() == token )
		{
			gEngfuncs.Con_Printf( "[GX] %s: GPU OK (%d ms)\n", tag, i * 10 );
			return;
		}
		usleep( 10000 );
	}

	gEngfuncs.Con_Printf( "[GX] %s: GPU STALLED (want %04x, read %04x)\n", tag, token, GX_GetDrawSync() );
}

extern int gx_testmode;

void R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum )
{
	if( gx_testmode == 3 ) return; // diagnostic: no textured quads at all
	static int dsp;
	int dbg = ( dsp < 2 );

	if( dbg ) GX_DbgWait( "dsp pre-bind" );

	GX_Bind( XASH_TEXTURE0, texnum );

	if( dbg ) GX_DbgWait( "dsp post-bind" );

	// el HUD nunca pasa por el Z-test; no confiar en el estado que dejo otro dibujo
	if( glState.in2DMode )
		GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );


 // Make the whole 2D textured-quad pipeline explicit. Nothing else in the
 // renderer calls GX_SetNumTexGens, and the colour channel defaults are not
 // guaranteed: without a texgen the TEV samples with undefined coordinates
 // (flat colours / vertical streaks), and a channel sourcing vertex colour
 // with no colour in the vertex stream gives random colours.
 GX_SetNumChans( 1 );
 GX_SetNumTexGens( 1 );
 GX_SetNumTevStages( 1 );
 GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
 	GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
 GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
 GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
 GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS,  GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );

	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS,  GX_POS_XY, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0 );

	GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );

		GX_Position2f32( x, y );
		GX_TexCoord2f32( s1, t1 );

		GX_Position2f32( x + w, y );
		GX_TexCoord2f32( s2, t1 );

		GX_Position2f32( x + w, y + h );
		GX_TexCoord2f32( s2, t2 );

		GX_Position2f32( x, y + h );
		GX_TexCoord2f32( s1, t2 );

	GX_End();

	if( dbg )
	{
		GX_DbgWait( "dsp post-draw" );
		gEngfuncs.Con_Printf( "[GX] DSP %d: calling GX_DrawDone\n", dsp );
		GX_DrawDone();
		gEngfuncs.Con_Printf( "[GX] DSP %d: DrawDone returned\n", dsp );
	}
	dsp++;
}


static void GX_ConvertToRGBA8( byte *dst, const byte *src, int width, int height, pixformat_t fmt )
{
	int tw = ( width + 3 ) & ~3;
	int srcBpp = 4; /* siempre PF_RGBA_32/PF_BGRA_32 */
	int sr = ( fmt == PF_BGRA_32 ) ? 2 : 0;
	int sb = ( fmt == PF_BGRA_32 ) ? 0 : 2;
	int sg = 1;
	int sa = 3;

	if( fmt != PF_RGBA_32 && fmt != PF_BGRA_32 )
	{
		/* Fallback: asumir RGBA */
		sr = 0; sg = 1; sb = 2; sa = 3;
	}

	for( int ty = 0; ty < height; ty += 4 )
	{
		for( int tx = 0; tx < width; tx += 4 )
		{
			byte *tile = dst + ( ( ty >> 2 ) * ( tw >> 2 ) + ( tx >> 2 ) ) * 64;
			for( int y = 0; y < 4; y++ )
			{
				int sy = ty + y;
				if( sy >= height ) sy = height - 1;
				for( int x = 0; x < 4; x++ )
				{
					int sx = tx + x;
					const byte *t;
					int in;
					if( sx >= width ) sx = width - 1;
					t = src + ( sy * width + sx ) * srcBpp;
					in = ( y * 4 + x ) * 2;
					tile[in + 0] = t[sa];
					tile[in + 1] = t[sr];
					tile[32 + in + 0] = t[sg];
					tile[32 + in + 1] = t[sb];
				}
			}
		}
	}
}


static size_t GX_CalcTexSizeForFormat( u8 fmt, int width, int height )
{
	int w4 = ( width + 3 ) & ~3, h4 = ( height + 3 ) & ~3;
	return (size_t)w4 * h4 * ( fmt == GX_TF_RGBA8 ? 4 : 2 );
}

/* actualiza un sub-rectangulo (RGBA) dentro de la textura nativa (RGBA8 o RGB565), como glTexSubImage2D */
void GX_UpdateTextureSub( int texnum, int x, int y, int w, int h, const byte *rgba )
{
	gl_texture_t *tex = R_GetTexture( texnum );
	int tx, ty;

	if( !tex || !tex->nativeData || !rgba )
		return;

	/* Buffer origen: RGBA (p[0]=R, p[1]=G, p[2]=B, p[3]=A) */
	if( tex->format == GX_TF_I8 )
	{
		int tw8 = ( (int)tex->width + 7 ) & ~7;
		for( ty = 0; ty < h; ty++ )
		{
			int py = y + ty;
			if( py < 0 || py >= (int)tex->height ) continue;
			for( tx = 0; tx < w; tx++ )
			{
				int px = x + tx;
				const byte *p;
				byte *tile;
				byte luma;
				if( px < 0 || px >= (int)tex->width ) continue;
				p = rgba + ( ty * w + tx ) * 4;
				luma = (byte)( ( p[0] * 77 + p[1] * 150 + p[2] * 29 ) >> 8 );
				tile = (byte *)tex->nativeData + ( ( py >> 2 ) * ( tw8 >> 3 ) + ( px >> 3 ) ) * 32;
				tile[( py & 3 ) * 8 + ( px & 7 )] = luma;
			}
		}
	}
	else if( tex->format == GX_TF_RGBA8 )
	{
		int tw = ( (int)tex->width + 3 ) & ~3;
		for( ty = 0; ty < h; ty++ )
		{
			int py = y + ty;
			if( py < 0 || py >= (int)tex->height ) continue;
			for( tx = 0; tx < w; tx++ )
			{
				int px = x + tx;
				const byte *p;
				byte *tile;
				int in;
				if( px < 0 || px >= (int)tex->width ) continue;
				p = rgba + ( ty * w + tx ) * 4;
				in = ( py & 3 ) * 4 + ( px & 3 );
				tile = (byte *)tex->nativeData + ( ( py >> 2 ) * ( tw >> 2 ) + ( px >> 2 ) ) * 64;
				tile[in * 2 + 0] = p[3];
				tile[in * 2 + 1] = p[0];
				tile[32 + in * 2 + 0] = p[1];
				tile[32 + in * 2 + 1] = p[2];
			}
		}
	}
	else if( tex->format == GX_TF_RGB565 )
	{
		int tw = ( (int)tex->width + 3 ) & ~3;
		for( ty = 0; ty < h; ty++ )
		{
			int py = y + ty;
			if( py < 0 || py >= (int)tex->height ) continue;
			for( tx = 0; tx < w; tx++ )
			{
				int px = x + tx;
				const byte *p;
				byte *tile;
				int in;
				u16 c;
				if( px < 0 || px >= (int)tex->width ) continue;
				p = rgba + ( ty * w + tx ) * 4;
				c = (u16)( ( ( p[0] >> 3 ) << 11 ) | ( ( p[1] >> 2 ) << 5 ) | ( p[2] >> 3 ) );
				in = ( py & 3 ) * 4 + ( px & 3 );
				tile = (byte *)tex->nativeData + ( ( py >> 2 ) * ( tw >> 2 ) + ( px >> 2 ) ) * 32;
				tile[in * 2 + 0] = (byte)( c >> 8 );
				tile[in * 2 + 1] = (byte)( c & 0xFF );
			}
		}
	}

	DCFlushRange( tex->nativeData, GX_CalcTexSizeForFormat( tex->format, tex->width, tex->height ) );
	GX_InvalidateTexAll();
}


static __attribute__((unused)) void GX_ConvertToRGB565Tiles( byte *dst, const byte *src, int width, int height, pixformat_t fmt )
{
	int tw = ( width + 3 ) & ~3;
	int srcBpp;
	int sr, sg, sb;

	switch( fmt )
	{
	case PF_RGBA_32: srcBpp = 4; sr = 0; sg = 1; sb = 2; break;
	case PF_BGRA_32: srcBpp = 4; sr = 2; sg = 1; sb = 0; break;
	case PF_RGB_24:  srcBpp = 3; sr = 0; sg = 1; sb = 2; break;
	case PF_BGR_24:  srcBpp = 3; sr = 2; sg = 1; sb = 0; break;
	case PF_LUMINANCE: srcBpp = 1; sr = sg = sb = 0; break;
	default: srcBpp = 4; sr = 0; sg = 1; sb = 2; break;
	}

	for( int ty = 0; ty < height; ty += 4 )
	{
		for( int tx = 0; tx < width; tx += 4 )
		{
			byte *tile = dst + ( ( ty >> 2 ) * ( tw >> 2 ) + ( tx >> 2 ) ) * 32;
			for( int y = 0; y < 4; y++ )
			{
				int sy = ty + y;
				if( sy >= height ) sy = height - 1;
				for( int x = 0; x < 4; x++ )
				{
					int sx = tx + x;
					const byte *t;
					int in;
					byte r, g, b;
					u16 c;
					if( sx >= width ) sx = width - 1;
					t = src + ( sy * width + sx ) * srcBpp;
					r = t[sr]; g = t[sg]; b = t[sb];
					c = (u16)( ( ( r >> 3 ) << 11 ) | ( ( g >> 2 ) << 5 ) | ( b >> 3 ) );
					in = ( y * 4 + x ) * 2;
					tile[in + 0] = (byte)( c >> 8 );
					tile[in + 1] = (byte)( c & 0xFF );
				}
			}
		}
	}
}
static void GX_ConvertToRGBA8Tiles( byte *dst, const byte *src, int width, int height, pixformat_t fmt )
{
	int tw = ( width + 3 ) & ~3;
	int srcBpp, sr, sg, sb, sa;

	switch( fmt )
	{
	case PF_BGRA_32: srcBpp = 4; sr = 2; sg = 1; sb = 0; sa = 3; break;
	case PF_RGB_24:  srcBpp = 3; sr = 0; sg = 1; sb = 2; sa = -1; break;
	case PF_BGR_24:  srcBpp = 3; sr = 2; sg = 1; sb = 0; sa = -1; break;
	case PF_LUMINANCE: srcBpp = 1; sr = sg = sb = 0; sa = -1; break;
	default: srcBpp = 4; sr = 0; sg = 1; sb = 2; sa = 3; break;
	}

	for( int ty = 0; ty < height; ty += 4 )
	{
		for( int tx = 0; tx < width; tx += 4 )
		{
			byte *tile = dst + ( ( ty >> 2 ) * ( tw >> 2 ) + ( tx >> 2 ) ) * 64;
			for( int y = 0; y < 4; y++ )
			{
				int sy = ty + y;
				if( sy >= height ) sy = height - 1;
				for( int x = 0; x < 4; x++ )
				{
					int sx = tx + x;
					const byte *t;
					int in = y * 4 + x;
					if( sx >= width ) sx = width - 1;
					t = src + ( sy * width + sx ) * srcBpp;
					tile[in * 2 + 0] = ( sa >= 0 ) ? t[sa] : 255;
					tile[in * 2 + 1] = t[sr];
					tile[32 + in * 2 + 0] = t[sg];
					tile[32 + in * 2 + 1] = t[sb];
				}
			}
		}
	}
}
void GX_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt )
{
	switch( fmt )
	{
	case PF_RGBA_32:
	case PF_BGRA_32:
	case PF_RGB_24:
	case PF_BGR_24:
	case PF_LUMINANCE:
		break;
	default:
		gEngfuncs.Con_DPrintf( S_ERROR "%s: unsupported pixel format %i\n", __func__, fmt );
		return;
	}

	width  = ( width  + 3 ) & ~3;
	height = ( height + 3 ) & ~3;

	byte *raw;
	if( cols != width || rows != height )
	{
		raw = GX_ResampleTexture( buffer, cols, rows, width, height, false );
		cols = width;
		rows = height;
	}
	else
		raw = (byte *)buffer;

	if( cols > glConfig.max_2d_texture_size )
		gEngfuncs.Host_Error( "%s: size %i exceeds hardware limits\n", __func__, cols );
	if( rows > glConfig.max_2d_texture_size )
		gEngfuncs.Host_Error( "%s: size %i exceeds hardware limits\n", __func__, rows );

	gl_texture_t *tex = R_GetTexture( texnum );

	size_t nativeSize = (size_t)( cols * rows ) * 4; /* RGBA8 = 4 bytes/pixel */

	if( cols == (int)tex->width && rows == (int)tex->height && tex->nativeData != NULL && tex->format == GX_TF_RGBA8 )
	{
		GX_ConvertToRGBA8Tiles( (byte *)tex->nativeData, raw, cols, rows, fmt );
		DCFlushRange( tex->nativeData, nativeSize );
		GX_InvalidateTexAll();
	}
	else
	{
		if( tex->nativeData != NULL )
		{
			free( tex->nativeData );
			tex->nativeData = NULL;
		}

		tex->nativeData = memalign( 32, nativeSize );
		tex->width  = cols;
		tex->height = rows;

		GX_ConvertToRGBA8Tiles( (byte *)tex->nativeData, raw, cols, rows, fmt );
		DCFlushRange( tex->nativeData, nativeSize );

		tex->format = GX_TF_RGBA8;

		GX_InitTexObj( &tex->texObj, tex->nativeData, (u16)cols, (u16)rows,
			GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE );
	}

	GX_ApplyTextureParams( tex );
}

void GX_ReadPixelsRGBA( int x, int y, int w, int h, byte *out )
{
    (void)x; (void)y;

    int tw = ( w + 3 ) & ~3;
    int th = ( h + 3 ) & ~3;

    size_t tileSize = (size_t)tw * th * 4;

    void *tileBuf = memalign( 32, tileSize );
    if( !tileBuf )
    {
        gEngfuncs.Con_DPrintf( S_ERROR "%s: out of memory (%zu bytes)\n", __func__, tileSize );
        return;
    }

    GX_DrawDone();
    /* GX_CopyTex copia el rectangulo y formato definidos con GX_SetTexCopySrc/Dst. En todo el
       codigo nunca se llamaban, asi que la captura salia con tamano/formato de destino sin
       definir (capturas corruptas en Load Game). EFB: origen arriba a la izquierda. */
    GX_SetTexCopySrc( 0, 0, (u16)tw, (u16)th );
    GX_SetTexCopyDst( (u16)tw, (u16)th, GX_TF_RGBA8, GX_FALSE );
    GX_CopyTex( tileBuf, GX_FALSE );
    GX_PixModeSync();
    GX_DrawDone();
    DCInvalidateRange( tileBuf, tileSize );

    const byte *src = (const byte *)tileBuf;

    for( int ty = 0; ty < th; ty += 4 )
    {
        for( int tx = 0; tx < tw; tx += 4 )
        {
            const byte *arBlock = src;
            const byte *gbBlock = src + 32;

            for( int row = 0; row < 4; row++ )
            {
                int py = ty + row;

                for( int col = 0; col < 4; col++ )
                {
                    int px = tx + col;
                    int i  = row * 4 + col;

                    byte a = arBlock[i * 2 + 0];
                    byte r = arBlock[i * 2 + 1];
                    byte g = gbBlock[i * 2 + 0];
                    byte b = gbBlock[i * 2 + 1];

                    if( px < w && py < h )
                    {
                        byte *dst = out + ( py * w + px ) * 4;
                        dst[0] = r;
                        dst[1] = g;
                        dst[2] = b;
                        dst[3] = 255; /* el alfa del EFB (RGB8/RGB565) no es fiable */
                    }
                }
            }

            src += 64;
        }
    }

    free( tileBuf );
}

void R_Set2DMode( qboolean enable )
{
	static u8 savedProjType = GX_PERSPECTIVE;

	if( enable )
	{
		if( glState.in2DMode )
			return;

		savedProjType = FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ) ? GX_ORTHOGRAPHIC : GX_PERSPECTIVE;

		matrix4x4 projection_matrix;

		switch( tr.rotation )
		{
		case REF_ROTATE_CW:
			GX_SetViewport( 0, 0, gpGlobals->height, gpGlobals->width, 0.0f, 1.0f );
			Matrix4x4_CreateOrtho( projection_matrix, 0, gpGlobals->height, gpGlobals->width, 0, -99999, 99999 );
			Matrix4x4_ConcatRotate( projection_matrix, 90, 0, 0, 1 );
			Matrix4x4_ConcatTranslate( projection_matrix, 0, -gpGlobals->height, 0 );
			break;
		case REF_ROTATE_CCW:
			GX_SetViewport( 0, 0, gpGlobals->height, gpGlobals->width, 0.0f, 1.0f );
			Matrix4x4_CreateOrtho( projection_matrix, 0, gpGlobals->height, gpGlobals->width, 0, -99999, 99999 );
			Matrix4x4_ConcatRotate( projection_matrix, -90, 0, 0, 1 );
			Matrix4x4_ConcatTranslate( projection_matrix, -gpGlobals->width, 0, 0 );
			break;
		default:
			GX_SetViewport( 0, 0, gpGlobals->width, gpGlobals->height, 0.0f, 1.0f );
			Matrix4x4_CreateOrtho( projection_matrix, 0, gpGlobals->width, gpGlobals->height, 0, -99999, 99999 );
			break;
		}

		Mtx44 gxProj;
		Matrix4x4_ToMtx44( gxProj, projection_matrix );
		GX_LoadProjectionMtx( gxProj, GX_ORTHOGRAPHIC );
		GX_SaveProjectionMtx( gxProj, GX_ORTHOGRAPHIC );

		matrix4x4 worldview_matrix;
		Matrix4x4_LoadIdentity( worldview_matrix );

		Mtx gxMv;
		Matrix4x4_ToMtx( gxMv, worldview_matrix );
		GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );

		GX_SetCullMode( GX_CULL_NONE );
		GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
		GX_SetAlphaCompare( GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0 );

		GXColor white = { 255, 255, 255, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, white );

		glState.in2DMode = true;
		RI.currententity = NULL;
		RI.currentmodel = NULL;
	}
	else
	{
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		glState.in2DMode = false;

		Mtx44 gxProj;
		Matrix4x4_ToMtx44( gxProj, RI.projectionMatrix );
		GX_LoadProjectionMtx( gxProj, savedProjType );
		GX_SaveProjectionMtx( gxProj, savedProjType );

		Mtx gxMv;
		Matrix4x4_ToMtx( gxMv, RI.worldviewMatrix );
		GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );

		GX_SetCullMode( GX_CULL_FRONT );
	}
}