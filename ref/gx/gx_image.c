/*
gx_image.c - texture uploading and processing (Wii GX native port)
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

#include <stdarg.h>
#include <malloc.h>
#include "gx_local.h"
#include <ogc/system.h>

// ---- memory accounting (diagnostic) ----
static size_t gx_tex_bytes;
static int    gx_tex_count;
static size_t gx_tex_next_report = 2 * 1024 * 1024;

static void GX_TexAccount( const gl_texture_t *tex, size_t bytes, int sign )
{
    if( sign > 0 )
    {
        gx_tex_bytes += bytes;
        gx_tex_count++;
        if( bytes >= 256 * 1024 || gx_tex_bytes >= gx_tex_next_report )
        {
            GX_LOG( "[GXMEM] +%s %ux%u %u KB | textures total %u KB in %d\n",
                tex->name, (unsigned)tex->width, (unsigned)tex->height,
                (unsigned)( bytes / 1024 ), (unsigned)( gx_tex_bytes / 1024 ), gx_tex_count );
            while( gx_tex_bytes >= gx_tex_next_report )
                gx_tex_next_report += 2 * 1024 * 1024;
        }
    }
    else
    {
        gx_tex_bytes = ( gx_tex_bytes > bytes ) ? gx_tex_bytes - bytes : 0;
        if( gx_tex_count > 0 ) gx_tex_count--;
    }
}

void GX_MemReport( const char *tag )
{
    struct mallinfo mi = mallinfo();

    GX_LOG( "[GXMEM] %s: heap in use %u KB, free inside heap %u KB, sbrk'd %u KB | "
        "arena2 left %u KB | textures %u KB in %d\n", tag,
        (unsigned)mi.uordblks / 1024, (unsigned)mi.fordblks / 1024, (unsigned)mi.arena / 1024,
        (unsigned)(( u32 )SYS_GetArena2Hi() - ( u32 )SYS_GetArena2Lo()) / 1024,
        (unsigned)( gx_tex_bytes / 1024 ), gx_tex_count );
}

#include "crclib.h"

static int g_diagTexIdx;

static int g_diagFmtCount;

static size_t GX_CalcTextureSize( u8 gxFormat, int width, int height, int depth );



#define TEXTURES_HASH_SIZE  (MAX_TEXTURES >> 2)

// Allocated on the heap (MEM2): MAX_TEXTURES entries would add ~1 MB of .bss,
// and MEM1 is where the executable lives and it is nearly full.
static gl_texture_t    *gl_textures;
static gl_texture_t    *gl_texturesHashTable[TEXTURES_HASH_SIZE];
static uint             gl_numTextures;

extern int gx_testmode;

#define IsLightMap( tex ) ( FBitSet(( tex )->flags, TF_ATLAS_PAGE ))

static void        GX_SetTextureDimensions( gl_texture_t *tex, int width, int height, int depth );
static int         GX_CalcMipmapCount( gl_texture_t *tex, qboolean haveBuffer );
static void        GX_ProcessImage( gl_texture_t *tex, rgbdata_t *pic );
static qboolean    GX_UploadTexture( gl_texture_t *tex, rgbdata_t *pic );
static gl_texture_t *GL_TextureForName( const char *name );
static gl_texture_t *GL_AllocTexture( const char *name, texFlags_t flags );
static void        GL_DeleteTexture( gl_texture_t *tex );
static void        GX_UpdateTextureParams( int iTexture );
static void        GX_ConvertToRGBA8( byte *dst, const byte *src, int width, int height, pixformat_t fmt );
byte       *GX_ResampleTexture( const byte *in, int inw, int inh, int outw, int outh, qboolean isNormal );
static qboolean    GX_CheckTexName( const char *name );

static void GX_EnsureTextureTable( void )
{
	if( !gl_textures )
	{
		gl_textures = calloc( MAX_TEXTURES, sizeof( *gl_textures ));
		if( !gl_textures )
			gEngfuncs.Host_Error( "GX: out of memory for the texture table\n" );
	}
}

gl_texture_t *R_GetTexture( unsigned int texnum )
{
	GX_EnsureTextureTable();
	if( texnum >= MAX_TEXTURES )
	{
		gEngfuncs.Host_Error( "%s: texnum (%d) >= MAX_TEXTURES (%d)", __func__, texnum, MAX_TEXTURES );
		texnum = 0;
	}
	return &gl_textures[texnum];
}

qboolean GX_TextureFilteringEnabled( const gl_texture_t *tex )
{
	if( FBitSet( tex->flags, TF_NEAREST ))
		return false;
	if( FBitSet( tex->flags, TF_DEPTHMAP ))
		return true;
	if( FBitSet( tex->flags, TF_NOMIPMAP ) || tex->numMips <= 1 )
	{
		if( FBitSet( tex->flags, TF_ATLAS_PAGE ))
			return gl_lightmap_nearest.value == 0.0f;
		if( FBitSet( tex->flags, TF_ALLOW_NEAREST ))
			return gl_texture_nearest.value == 0.0f;
		return true;
	}
	return gl_texture_nearest.value == 0.0f;
}

void GX_ApplyTextureParams( gl_texture_t *tex )
{
	if( !glw_state.initialized )
		return;
	Assert( tex != NULL );

	if( FBitSet( tex->flags, TF_MULTISAMPLE | TF_DEPTHMAP ))
		return;

	qboolean nomipmap = tex->numMips <= 1 || FBitSet( tex->flags, TF_NOMIPMAP | TF_DEPTHMAP );

	u8 minFilt, magFilt;
	if( !GX_TextureFilteringEnabled( tex ))
	{
		minFilt = nomipmap ? GX_NEAR : GX_NEAR_MIP_NEAR;
		magFilt = GX_NEAR;
	}
	else
	{
		minFilt = nomipmap ? GX_LINEAR : GX_LIN_MIP_LIN;
		magFilt = GX_LINEAR;
	}

	u8 wrapS, wrapT;
	if( FBitSet( tex->flags, TF_BORDER | TF_CLAMP ))
	{
		wrapS = GX_CLAMP;
		wrapT = GX_CLAMP;
	}
	else
	{
		wrapS = GX_REPEAT;
		wrapT = GX_REPEAT;
	}

	tex->minFilter = minFilt;
	tex->magFilter = magFilt;
	tex->wrapS     = wrapS;
	tex->wrapT     = wrapT;

	if( tex->nativeData )
	{
		GX_InitTexObj( &tex->texObj, tex->nativeData,
			(u16)tex->width, (u16)tex->height,
			tex->format,
			wrapS, wrapT,
			tex->numMips > 1 && !FBitSet( tex->flags, TF_NOMIPMAP ) ? GX_TRUE : GX_FALSE );

		GX_InitTexObjLOD( &tex->texObj,
			minFilt, magFilt,
			0.0f, (float)( tex->numMips - 1 ),
			0.0f,
			GX_FALSE, GX_FALSE, GX_ANISO_1 );

		DCFlushRange( tex->nativeData,
			GX_CalcTextureSize( tex->format, tex->width, tex->height, 1 ));
		GX_InvalidateTexAll();

		/* el texObj cambio: forzar que GX_Bind lo recargue al hardware */
		for( int u = 0; u < MAX_TEXTURE_UNITS; u++ )
			if( glState.currentTexturesIndex[u] == (int)( tex - gl_textures ))
				glState.currentTexturesIndex[u] = 0;
	}
}

static void GX_UpdateTextureParams( int iTexture )
{
	gl_texture_t *tex = &gl_textures[iTexture];
	if( !tex->texnum ) return;
	GX_Bind( XASH_TEXTURE0, iTexture );
	GX_ApplyTextureParams( tex );
}

void R_SetTextureParameters( void )
{
	if( 0 )  /* GX: no anisotropy extension */
	{
		if( gl_texture_anisotropy.value > glConfig.max_texture_anisotropy )
			gEngfuncs.Cvar_SetValue( "gl_anisotropy", glConfig.max_texture_anisotropy );
		else if( gl_texture_anisotropy.value < 1.0f )
			gEngfuncs.Cvar_SetValue( "gl_anisotropy", 1.0f );
	}
	if( 0 )  /* GX: no LOD bias extension */
	{
		if( gl_texture_lodbias.value < -glConfig.max_texture_lod_bias )
			gEngfuncs.Cvar_SetValue( "gl_texture_lodbias", -glConfig.max_texture_lod_bias );
		else if( gl_texture_lodbias.value > glConfig.max_texture_lod_bias )
			gEngfuncs.Cvar_SetValue( "gl_texture_lodbias", glConfig.max_texture_lod_bias );
	}

	ClearBits( gl_texture_anisotropy.flags, FCVAR_CHANGED );
	ClearBits( gl_texture_lodbias.flags,    FCVAR_CHANGED );
	ClearBits( gl_texture_nearest.flags,    FCVAR_CHANGED );
	ClearBits( gl_lightmap_nearest.flags,   FCVAR_CHANGED );

	for( int i = 0; i < (int)gl_numTextures; i++ )
		GX_UpdateTextureParams( i );
}

static int GX_CalcTextureSamples( int flags )
{
	if( FBitSet( flags, IMAGE_HAS_COLOR ))
		return FBitSet( flags, IMAGE_HAS_ALPHA ) ? 4 : 3;
	return FBitSet( flags, IMAGE_HAS_ALPHA ) ? 2 : 1;
}

static size_t GX_CalcTextureSize( u8 gxFormat, int width, int height, int depth )
{
	depth = Q_max( 1, depth );
	int w4 = ( width  + 3 ) & ~3;
	int h4 = ( height + 3 ) & ~3;
	int w8 = ( width  + 7 ) & ~7;
	int h8 = ( height + 7 ) & ~7;

	switch( (int)gxFormat )
	{
	case GX_TF_RGB565:
	case GX_TF_RGB5A3:
	case GX_TF_IA8:     return (size_t)w4 * h4 * 2 * depth;
	case GX_TF_I8:
	case GX_TF_IA4:     return (size_t)w8 * h4 * depth;
	case GX_TF_I4:      return (size_t)w8 * h8 / 2 * depth;
	case GX_TF_RGBA8:
	default:            return (size_t)w4 * h4 * 4 * depth;
	}
}

static int GX_CalcMipmapCount( gl_texture_t *tex, qboolean haveBuffer )
{
	if( !haveBuffer ) return 1;
	if( FBitSet( tex->flags, TF_NOMIPMAP )) return 1;
	int mipcount;
	for( mipcount = 0; mipcount < 16; mipcount++ )
	{
		int w = Q_max( 1, ( tex->width  >> mipcount ));
		int h = Q_max( 1, ( tex->height >> mipcount ));
		if( w == 1 && h == 1 )
			break;
	}
	return mipcount + 1;
}

// solo se activa durante la subida de una imagen real (no en GX_UpdateTexSize)
static qboolean s_allow_half;

static void GX_SetTextureDimensions( gl_texture_t *tex, int width, int height, int depth )
{
	Assert( tex != NULL );
	int maxTextureSize = glConfig.max_2d_texture_size;

	tex->srcWidth  = (word)width;
	tex->srcHeight = (word)height;

	int step = (int)gl_round_down.value;

	int scaled_width;
	for( scaled_width = 1; scaled_width < width; scaled_width <<= 1 );
	if( step > 0 && width < scaled_width && ( step == 1 || ( scaled_width - width ) > ( scaled_width >> step )))
		scaled_width >>= 1;

	int scaled_height;
	for( scaled_height = 1; scaled_height < height; scaled_height <<= 1 );
	if( step > 0 && height < scaled_height && ( step == 1 || ( scaled_height - height ) > ( scaled_height >> step )))
		scaled_height >>= 1;

	width  = scaled_width;
	height = scaled_height;

	while( width > maxTextureSize || height > maxTextureSize )
	{
		width  >>= 1;
		height >>= 1;
	}

	// gx_texhalf: mitad de resolucion en texturas con mipmaps (mundo y modelos); las de
	// interfaz, fuentes y lightmaps (TF_NOMIPMAP) se quedan como estan
	if( s_allow_half && gx_texhalf.value && !FBitSet( tex->flags, TF_NOMIPMAP ) && width >= 64 && height >= 64 )
	{
		width  >>= 1;
		height >>= 1;
	}

	tex->width  = (word)Q_max( 1, width );
	tex->height = (word)Q_max( 1, height );
	tex->depth  = (word)Q_max( 1, depth );
}

#define GX_TARGET_NONE  0xFF

static void GX_SetTextureTarget( gl_texture_t *tex, rgbdata_t *pic )
{
	Assert( pic != NULL );
	Assert( tex != NULL );

	pic->depth  = Q_max( 1, pic->depth );
	tex->numMips = 0;
	pic->numMips = Q_max( 1, pic->numMips );

	if( FBitSet( pic->flags, IMAGE_CUBEMAP ))      { tex->format = GX_TARGET_NONE; return; }
	if( FBitSet( pic->flags, IMAGE_MULTILAYER ))   { tex->format = GX_TARGET_NONE; return; }
	if( pic->depth > 1 )                           { tex->format = GX_TARGET_NONE; return; }
	if( FBitSet( tex->flags, TF_MULTISAMPLE ))     { tex->format = GX_TARGET_NONE; return; }
	if( FBitSet( tex->flags, TF_DEPTHMAP ))        { tex->format = GX_TARGET_NONE; return; }
}

static void GX_BoxFilter3x3( byte *out, const byte *in, int w, int h, int x, int y )
{
	int r = 0, g = 0, b = 0, a = 0;
	int acount = 0;
	for( int i = 0; i < 3; i++ )
	{
		int u = ( i - 1 ) + x;
		for( int j = 0; j < 3; j++ )
		{
			int v = ( j - 1 ) + y;
			if( u >= 0 && u < w && v >= 0 && v < h )
			{
				const byte *pixel = &in[( u + v * w ) * 4];
				if( pixel[3] != 0 )
				{
					r += pixel[0]; g += pixel[1]; b += pixel[2]; a += pixel[3];
					acount++;
				}
			}
		}
	}
	if( acount == 0 ) acount = 1;
	out[0] = r / acount;
	out[1] = g / acount;
	out[2] = b / acount;
}

static byte *GX_ApplyFilter( const byte *source, int width, int height )
{
	byte *in  = (byte *)source;
	byte *out = (byte *)source;
	if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) || glConfig.max_multisamples > 1 )
		return in;
	for( int i = 0; source && i < width * height; i++, in += 4 )
	{
		if( in[0] == 0 && in[1] == 0 && in[2] == 0 && in[3] == 0 )
			GX_BoxFilter3x3( in, source, width, height, i % width, i / width );
	}
	return out;
}

static void GX_BuildMipMap( byte *in, int srcWidth, int srcHeight, int srcDepth, int flags )
{
	if( !in ) return;
	byte *out       = in;
	int instride    = ALIGN( srcWidth * 4, 1 );
	int mipWidth    = Q_max( 1, ( srcWidth  >> 1 ));
	int mipHeight   = Q_max( 1, ( srcHeight >> 1 ));
	int outpadding  = ALIGN( mipWidth * 4, 1 ) - mipWidth * 4;

	if( FBitSet( flags, TF_ALPHACONTRAST ))
	{
		memset( in, mipWidth, mipWidth * mipHeight * 4 );
		return;
	}

	for( int z = 0; z < srcDepth; z++ )
	{
		if( FBitSet( flags, 0 ))
		{
			for( int y = 0; y < mipHeight; y++, in += instride * 2, out += outpadding )
			{
				byte *next = ((( y << 1 ) + 1 ) < srcHeight ) ? ( in + instride ) : in;
				int row = 0;
				for( int x = 0; x < mipWidth; x++, row += 8, out += 4 )
				{
					vec3_t normal;
					if((( x << 1 ) + 1 ) < srcWidth )
					{
						normal[0] = MAKE_SIGNED( in[row+0] ) + MAKE_SIGNED( in[row+4] )
						          + MAKE_SIGNED( next[row+0] ) + MAKE_SIGNED( next[row+4] );
						normal[1] = MAKE_SIGNED( in[row+1] ) + MAKE_SIGNED( in[row+5] )
						          + MAKE_SIGNED( next[row+1] ) + MAKE_SIGNED( next[row+5] );
						normal[2] = MAKE_SIGNED( in[row+2] ) + MAKE_SIGNED( in[row+6] )
						          + MAKE_SIGNED( next[row+2] ) + MAKE_SIGNED( next[row+6] );
					}
					else
					{
						normal[0] = MAKE_SIGNED( in[row+0] ) + MAKE_SIGNED( next[row+0] );
						normal[1] = MAKE_SIGNED( in[row+1] ) + MAKE_SIGNED( next[row+1] );
						normal[2] = MAKE_SIGNED( in[row+2] ) + MAKE_SIGNED( next[row+2] );
					}
					if( !VectorNormalizeLength( normal ))
						VectorSet( normal, 0.5f, 0.5f, 1.0f );
					out[0] = 128 + (byte)(127.0f * normal[0]);
					out[1] = 128 + (byte)(127.0f * normal[1]);
					out[2] = 128 + (byte)(127.0f * normal[2]);
					out[3] = 255;
				}
			}
		}
		else
		{
			for( int y = 0; y < mipHeight; y++, in += instride * 2, out += outpadding )
			{
				byte *next = ((( y << 1 ) + 1 ) < srcHeight ) ? ( in + instride ) : in;
				int row = 0;
				for( int x = 0; x < mipWidth; x++, row += 8, out += 4 )
				{
					if((( x << 1 ) + 1 ) < srcWidth )
					{
						out[0] = (in[row+0] + in[row+4] + next[row+0] + next[row+4]) >> 2;
						out[1] = (in[row+1] + in[row+5] + next[row+1] + next[row+5]) >> 2;
						out[2] = (in[row+2] + in[row+6] + next[row+2] + next[row+6]) >> 2;
						out[3] = (in[row+3] + in[row+7] + next[row+3] + next[row+7]) >> 2;
					}
					else
					{
						out[0] = (in[row+0] + next[row+0]) >> 1;
						out[1] = (in[row+1] + next[row+1]) >> 1;
						out[2] = (in[row+2] + next[row+2]) >> 1;
						out[3] = (in[row+3] + next[row+3]) >> 1;
					}
				}
			}
		}
	}
}

static void GX_ConvertToRGBA8( byte *dst, const byte *src, int width, int height, pixformat_t fmt )
{
	int tw = ( width + 3 ) & ~3;
	int srcBpp;
	int sr, sg, sb, sa;

	switch( fmt )
	{
	case PF_RGBA_32: srcBpp = 4; sr = 0; sg = 1; sb = 2; sa = 3; break;
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
					int in;
					if( sx >= width ) sx = width - 1;
					t = src + ( sy * width + sx ) * srcBpp;
					in = ( y * 4 + x ) * 2;
					tile[in + 0] = ( sa >= 0 ) ? t[sa] : 255;
					tile[in + 1] = t[sr];
					tile[32 + in + 0] = t[sg];
					tile[32 + in + 1] = t[sb];
				}
			}
		}
	}
}



typedef struct { int bpp, r, g, b, a; } gx_srcfmt_t;

static qboolean GX_GetSrcFmt( pixformat_t fmt, gx_srcfmt_t *s )
{
	switch( fmt )
	{
	case PF_RGBA_32:   s->bpp = 4; s->r = 0; s->g = 1; s->b = 2; s->a = 3;  return true;
	case PF_BGRA_32:   s->bpp = 4; s->r = 2; s->g = 1; s->b = 0; s->a = 3;  return true;
	case PF_RGB_24:    s->bpp = 3; s->r = 0; s->g = 1; s->b = 2; s->a = -1; return true;
	case PF_BGR_24:    s->bpp = 3; s->r = 2; s->g = 1; s->b = 0; s->a = -1; return true;
	case PF_LUMINANCE: s->bpp = 1; s->r = s->g = s->b = 0; s->a = -1;       return true;
	default: return false;
	}
}

static u8 GX_PickFormat( const byte *src, int width, int height, pixformat_t fmt )
{
	gx_srcfmt_t s;
	qboolean hasAlpha = false, constRGB = true, haveRef = false;
	byte rr = 0, rg = 0, rb = 0;

	if( !src || !GX_GetSrcFmt( fmt, &s ) )
		return GX_TF_RGB565;

	if( s.a < 0 )
		return GX_TF_RGB565;

	for( int j = 0, n = width * height; j < n; j++, src += s.bpp )
	{
		byte a = src[s.a];

		if( a == 255 ) { if( !constRGB && hasAlpha ) break; }
		else hasAlpha = true;

		if( a == 0 ) continue;

		if( !haveRef )
		{
			rr = src[s.r]; rg = src[s.g]; rb = src[s.b];
			haveRef = true;
		}
		else if( src[s.r] != rr || src[s.g] != rg || src[s.b] != rb )
		{
			constRGB = false;
		}
	}

	if( !hasAlpha )
		return GX_TF_RGB565;
	if( constRGB && haveRef && rr >= 240 && rg >= 240 && rb >= 240 )
		return GX_TF_IA4;
	return GX_TF_RGB5A3;
}




static void GX_ConvertToNative( byte *dst, const byte *src, int width, int height, pixformat_t fmt, u8 gxFmt )
{
	gx_srcfmt_t s;
	int tw, tx, ty;

	if( gxFmt == GX_TF_RGBA8 || !GX_GetSrcFmt( fmt, &s ) )
	{
		GX_ConvertToRGBA8( dst, src, width, height, fmt );
		return;
	}

	/* GX espera texturas swizzled en tiles de 4x4 (o 8x4 para IA4/I8). */
	tw = ( width + 3 ) & ~3;

	if( gxFmt == GX_TF_IA4 || gxFmt == GX_TF_I8 )
	{
		/* Tiles 8x4 */
		int tw8 = ( width + 7 ) & ~7;
		for( ty = 0; ty < height; ty += 4 )
		{
			for( tx = 0; tx < width; tx += 8 )
			{
				for( int y = 0; y < 4; y++ )
				{
					int sy = ty + y;
					if( sy >= height ) sy = height - 1;
					for( int x = 0; x < 8; x++ )
					{
						int sx = tx + x;
						const byte *t;
						byte r, g, b, a;
						byte *tile;
						int in;
						if( sx >= width ) sx = width - 1;
						t = src + ( sy * width + sx ) * s.bpp;
						r = t[s.r]; g = t[s.g]; b = t[s.b];
						a = ( s.a >= 0 ) ? t[s.a] : 255;
						tile = dst + ( ( ty >> 2 ) * ( tw8 >> 3 ) + ( tx >> 3 ) ) * 32;
						in = y * 8 + x;
						if( gxFmt == GX_TF_IA4 )
							tile[in] = (byte)((( a * 15 + 127 ) / 255 ) << 4 ) | 0x0F;
						else /* GX_TF_I8 */
							tile[in] = (byte)(( r * 77 + g * 150 + b * 29 ) >> 8 );
					}
				}
			}
		}
		return;
	}

	/* Tiles 4x4 para RGB565, RGB5A3, IA8 */
	for( ty = 0; ty < height; ty += 4 )
	{
		for( tx = 0; tx < width; tx += 4 )
		{
			for( int y = 0; y < 4; y++ )
			{
				int sy = ty + y;
				if( sy >= height ) sy = height - 1;
				for( int x = 0; x < 4; x++ )
				{
					int sx = tx + x;
					const byte *t;
					byte r, g, b, a;
					byte *tile;
					int in;
					if( sx >= width ) sx = width - 1;
					t = src + ( sy * width + sx ) * s.bpp;
					r = t[s.r]; g = t[s.g]; b = t[s.b];
					a = ( s.a >= 0 ) ? t[s.a] : 255;
					tile = dst + ( ( ty >> 2 ) * ( tw >> 2 ) + ( tx >> 2 ) ) * 32;
					in = ( y * 4 + x ) * 2;
					if( gxFmt == GX_TF_RGB565 )
					{
						u16 v = (u16)( ( ( r >> 3 ) << 11 ) | ( ( g >> 2 ) << 5 ) | ( b >> 3 ) );
						tile[in + 0] = (byte)( v >> 8 );
						tile[in + 1] = (byte)( v & 0xFF );
					}
					else if( gxFmt == GX_TF_RGB5A3 )
					{
						u16 v;
						if( a >= 0xE0 )
							v = (u16)( 0x8000 | ( ( r >> 3 ) << 10 ) | ( ( g >> 3 ) << 5 ) | ( b >> 3 ) );
						else
							v = (u16)( ( ( a >> 5 ) << 12 ) | ( ( r >> 4 ) << 8 ) | ( ( g >> 4 ) << 4 ) | ( b >> 4 ) );
						tile[in + 0] = (byte)( v >> 8 );
						tile[in + 1] = (byte)( v & 0xFF );
					}
					else /* GX_TF_IA8 */
					{
						/* I = luma, A = alpha */
						byte luma = (byte)(( r * 77 + g * 150 + b * 29 ) >> 8 );
						tile[in + 0] = luma;
						tile[in + 1] = a;
					}
				}
			}
		}
	}
}


byte *GX_ResampleTexture( const byte *in, int inw, int inh, int outw, int outh, qboolean isNormal )
{
if( !in ) return NULL;
if( inw == outw && inh == outh )
return (byte *)in;

byte *out = (byte *)Mem_Malloc( r_temppool, outw * outh * 4 );
if( !out ) return (byte *)in;

for( int y = 0; y < outh; y++ )
{
int sy = ( y * inh ) / outh;
for( int x = 0; x < outw; x++ )
{
int sx = ( x * inw ) / outw;
const byte *src = in + ( sy * inw + sx ) * 4;
byte *dst = out + ( y * outw + x ) * 4;
dst[0] = src[0];
dst[1] = src[1];
dst[2] = src[2];
dst[3] = src[3];
}
}
return out;
}

static void GX_UploadMipLevel( gl_texture_t *tex, int level, int width, int height,
	pixformat_t srcFmt, const byte *data, u8 newFmt )
{
	if( level != 0 )
		return;

	size_t nativeSize = GX_CalcTextureSize( newFmt, width, height, 1 );

	if( tex->nativeData && tex->format == newFmt && (int)tex->width == width && (int)tex->height == height )
	{
		GX_ConvertToNative( (byte *)tex->nativeData, data, width, height, srcFmt, newFmt );
		DCFlushRange( tex->nativeData, nativeSize );
		GX_InvalidateTexAll();
	}
	else
	{
		if( tex->nativeData )
		{
			GX_TexAccount( tex, GX_CalcTextureSize( tex->format, tex->width, tex->height, 1 ), -1 );
			free( tex->nativeData );
			tex->nativeData = NULL;
		}
		tex->nativeData = memalign( 32, nativeSize );
		if( !tex->nativeData )
		{
			gEngfuncs.Host_Error( "%s: out of memory (%zu bytes) for %s\n",
				__func__, nativeSize, tex->name );
			return;
		}
		tex->format = newFmt;
		tex->width  = (word)width;
		tex->height = (word)height;
		GX_TexAccount( tex, nativeSize, +1 );

		GX_ConvertToNative( (byte *)tex->nativeData, data, width, height, srcFmt, newFmt );
		DCFlushRange( tex->nativeData, nativeSize );

		GX_InitTexObj( &tex->texObj, tex->nativeData, (u16)width, (u16)height,
			tex->format, GX_CLAMP, GX_CLAMP, GX_FALSE );
	}
	tex->size = nativeSize;
}

static qboolean GX_UploadTexture( gl_texture_t *tex, rgbdata_t *pic )
{
	if( !glw_state.initialized )
		return true;

	Assert( pic != NULL );
	Assert( tex != NULL );

	GX_SetTextureTarget( tex, pic );
	if( tex->format == GX_TARGET_NONE )
	{
		gEngfuncs.Con_DPrintf( S_ERROR "%s: %s — texture type not supported on GX\n",
			__func__, tex->name );
		return false;
	}

	if( ImageCompressed( pic->type ))
	{
		gEngfuncs.Con_DPrintf( S_ERROR
			"%s: %s — compressed formats must be decompressed before GX upload\n",
			__func__, tex->name );
		return false;
	}

	s_allow_half = ( pic->buffer != NULL );
	GX_SetTextureDimensions( tex, pic->width, pic->height, pic->depth );
	s_allow_half = false;
	if( g_diagFmtCount < 10 )
	{
		g_diagFmtCount++;
		GX_LOG( "[FMTDIAG] #%d %s pic->type=%d flags=0x%X %dx%d\n",
			g_diagFmtCount, tex->name, (int)pic->type, pic->flags, pic->width, pic->height );
	}

	tex->fogParams[0] = pic->fogParams[0];
	tex->fogParams[1] = pic->fogParams[1];
	tex->fogParams[2] = pic->fogParams[2];
	tex->fogParams[3] = pic->fogParams[3];

	if( !pic->buffer )
	{
		if( tex->nativeData )
		{
			GX_TexAccount( tex, GX_CalcTextureSize( tex->format, tex->width, tex->height, 1 ), -1 );
			free( tex->nativeData );
			tex->nativeData = NULL;
		}
		tex->format = GX_TF_RGB565; // forzado: cabe en TMEM 128K como el skybox
		size_t size = GX_CalcTextureSize( tex->format, tex->width, tex->height, 1 );
		tex->nativeData = memalign( 32, size );
		if( !tex->nativeData )
			return false;
		GX_TexAccount( tex, size, +1 );
		memset( tex->nativeData, 0, size );
		GX_InitTexObj( &tex->texObj, tex->nativeData, tex->width, tex->height,
			tex->format, GX_CLAMP, GX_CLAMP, GX_FALSE );
		DCFlushRange( tex->nativeData, size );
		SetBits( tex->flags, TF_IMG_UPLOADED );
		return true;
	}

	byte *data;
	pixformat_t dataFmt = pic->type;
	if(( pic->width != tex->width ) || ( pic->height != tex->height ))
	{
		dataFmt = PF_RGBA_32;
		data = GX_ResampleTexture( pic->buffer, pic->width, pic->height,
			tex->width, tex->height,
			FBitSet( tex->flags, 0 ) ? true : false );
	}
	else
		data = pic->buffer;

	if( !FBitSet( tex->flags, TF_NOMIPMAP ) && FBitSet( pic->flags, IMAGE_ONEBIT_ALPHA ))
		data = GX_ApplyFilter( data, tex->width, tex->height );

		{
		u8 picked = GX_PickFormat( data, tex->width, tex->height, dataFmt );
		if( FBitSet( tex->flags, TF_ATLAS_PAGE ))
			picked = ( gx_testmode == 14 ) ? GX_TF_RGB565 : GX_TF_RGBA8;
		if( g_diagTexIdx < 15 )
		{
			g_diagTexIdx++;
			GX_LOG( "[UPLDIAG] #%d %s dataFmt=%d -> gxFmt=%d %dx%d\n",
				g_diagTexIdx, tex->name, (int)dataFmt, (int)picked, tex->width, tex->height );
		}
		GX_UploadMipLevel( tex, 0, tex->width, tex->height, dataFmt, data, picked );
	}

	if( data != pic->buffer )
		Mem_Free( data );

	SetBits( tex->flags, TF_IMG_UPLOADED );

	GX_ApplyTextureParams( tex );

	/* NO marcar la textura como bindeada: nunca se cargo al hardware (GX_LoadTexObj). */
	/* Si se marca, el siguiente GX_Bind de esa textura se salta la carga y se dibuja con otra. */
	glState.currentTextures[glState.activeTMU] = NULL;
	glState.currentTexturesIndex[glState.activeTMU] = 0;

	return true;
}

static void GX_ProcessImage( gl_texture_t *tex, rgbdata_t *pic )
{
	uint img_flags = 0;

	if( tex->flags & TF_FORCE_COLOR )   pic->flags |= IMAGE_HAS_COLOR;
	if( pic->flags & IMAGE_HAS_ALPHA )  tex->flags |= TF_HAS_ALPHA;
	if( FBitSet( pic->flags, IMAGE_PREMULTIPLIED ))
		SetBits( tex->flags, TF_PREMULTIPLIED );

	tex->encode = pic->encode;

	if( ImageCompressed( pic->type ))
	{
		if( !pic->numMips )
			tex->flags |= TF_NOMIPMAP;
		tex->flags &= ~TF_KEEP_SOURCE;
	}
	else
	{
		if( pic->flags & IMAGE_HAS_LUMA )   tex->flags |= TF_HAS_LUMA;
		if( pic->flags & IMAGE_QUAKEPAL )   tex->flags |= TF_QUAKEPAL;

		if( tex->flags & TF_MAKELUMA )
		{
			img_flags |= IMAGE_MAKE_LUMA;
			tex->flags &= ~TF_MAKELUMA;
		}

		if( !FBitSet( tex->flags, TF_IMG_UPLOADED ) && FBitSet( tex->flags, TF_KEEP_SOURCE ))
			tex->original = gEngfuncs.FS_CopyImage( pic );

		if( pic->type == PF_INDEXED_24 || pic->type == PF_INDEXED_32 )
			img_flags |= IMAGE_FORCE_RGBA;

		if( pic->buffer )
			gEngfuncs.Image_Process( &pic, 0, 0, img_flags, 0 );

		if( FBitSet( pic->flags, IMAGE_PLAYERDECAL ) && pic->buffer
		    && pic->type == PF_RGBA_32 && FBitSet( pic->flags, IMAGE_HAS_ALPHA )
		    && !FBitSet( pic->flags, IMAGE_PREMULTIPLIED ))
		{
			int   cnt = (int)( pic->width * pic->height );
			byte *p   = pic->buffer;
			for( int i = 0; i < cnt; i++, p += 4 )
			{
				int a = p[3];
				p[0] = (byte)(( p[0] * a + 127 ) / 255 );
				p[1] = (byte)(( p[1] * a + 127 ) / 255 );
				p[2] = (byte)(( p[2] * a + 127 ) / 255 );
			}
			SetBits( pic->flags, IMAGE_PREMULTIPLIED );
			SetBits( tex->flags, TF_PREMULTIPLIED );
		}

		if( FBitSet( tex->flags, TF_LUMINANCE ))
			ClearBits( pic->flags, IMAGE_HAS_COLOR );
	}
}

static qboolean GX_CheckTexName( const char *name )
{
	if( COM_StringEmptyOrNULL( name ))
		return false;
	int len = Q_strlen( name );
	if( len >= (int)sizeof( gl_textures->name ))
	{
		gEngfuncs.Con_Printf( S_ERROR "LoadTexture: too long name %s (%d)\n", name, len );
		return false;
	}
	return true;
}

static gl_texture_t *GL_TextureForName( const char *name )
{
	uint hash = COM_HashKey( name, TEXTURES_HASH_SIZE );
	for( gl_texture_t *tex = gl_texturesHashTable[hash]; tex != NULL; tex = tex->nextHash )
	{
		if( !Q_stricmp( tex->name, name ))
			return tex;
	}
	return NULL;
}

static gl_texture_t *GL_AllocTexture( const char *name, texFlags_t flags )
{
	const qboolean skyboxhack = FBitSet( flags, TF_SKYSIDE );
	gl_texture_t *tex = NULL;
	uint texnum;

	if( skyboxhack )
	{
		texnum = tr.skyboxbasenum;
		if( texnum < MAX_TEXTURES && gl_textures[texnum].texnum == 0 )
		{
			tex    = &gl_textures[texnum];
			tex->texnum = texnum;
		}
	}

	if( tex == NULL )
	{
		for( uint i = 1; i < MAX_TEXTURES; i++ )
		{
			if( gl_textures[i].texnum == 0 )
			{
				tex    = &gl_textures[i];
				texnum = i;
				tex->texnum = texnum;
				break;
			}
		}
	}

	if( tex == NULL )
	{
		gEngfuncs.Host_Error( "%s: MAX_TEXTURES limit exceeds\n", __func__ );
		return NULL;
	}

	Q_strncpy( tex->name, name, sizeof( tex->name ));
	tex->flags = flags;
	gl_numTextures = Q_max(( tex - gl_textures ) + 1, gl_numTextures );

	if( skyboxhack )
		tr.skyboxbasenum++;

	tex->hashValue = COM_HashKey( name, TEXTURES_HASH_SIZE );
	tex->nextHash  = gl_texturesHashTable[tex->hashValue];
	gl_texturesHashTable[tex->hashValue] = tex;

	return tex;
}

static void GL_DeleteTexture( gl_texture_t *tex )
{
	Assert( tex != NULL );
	if( !tex->texnum ) return;

	gl_texture_t **prev = &gl_texturesHashTable[tex->hashValue];
	while( 1 )
	{
		gl_texture_t *cur = *prev;
		if( !cur ) break;
		if( cur == tex ) { *prev = cur->nextHash; break; }
		prev = &cur->nextHash;
	}

	int texIdx = (int)( tex - gl_textures );
	for( int i = 0; i < MAX_TEXTURE_UNITS; i++ )
	{
		if( glState.currentTexturesIndex[i] == texIdx )
		{
			glState.currentTextures[i]       = NULL;
			glState.currentTexturesIndex[i]  = 0;
			glState.currentTextureFormats[i] = 0;
		}
	}

	if( tex->original )
		gEngfuncs.FS_FreeImage( tex->original );

	if( tex->nativeData )
	{
		GX_TexAccount( tex, GX_CalcTextureSize( tex->format, tex->width, tex->height, 1 ), -1 );
		free( tex->nativeData );
		tex->nativeData = NULL;
	}
	memset( tex, 0, sizeof( *tex ));
}

void GX_UpdateTexSize( int texnum, int width, int height, int depth )
{
	if( texnum <= 0 || texnum >= MAX_TEXTURES )
		return;
	gl_texture_t *tex = &gl_textures[texnum];
	GX_SetTextureDimensions( tex, width, height, depth );
	tex->size = 0;
	for( int j = 0; j < Q_max( 1, tex->numMips ); j++ )
	{
		int w = Q_max( 1, ( tex->width  >> j ));
		int h = Q_max( 1, ( tex->height >> j ));
		tex->size += GX_CalcTextureSize( tex->format, w, h, tex->depth );
	}
}

/* tiempo acumulado cargando texturas, para el log [LOAD] de R_NewMap */
double gx_load_tex_time;
int    gx_load_tex_count;

static int GX_LoadTexture_impl( const char *name, const byte *buf, size_t size, int flags );
static int GX_LoadTextureFromBuffer_impl( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update );

int GX_LoadTexture( const char *name, const byte *buf, size_t size, int flags )
{
	double t0 = Platform_DoubleTime();
	int ret = GX_LoadTexture_impl( name, buf, size, flags );
	gx_load_tex_time += Platform_DoubleTime() - t0;
	gx_load_tex_count++;
	return ret;
}

int GX_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	double t0 = Platform_DoubleTime();
	int ret = GX_LoadTextureFromBuffer_impl( name, pic, flags, update );
	gx_load_tex_time += Platform_DoubleTime() - t0;
	gx_load_tex_count++;
	return ret;
}

static int GX_LoadTexture_impl( const char *name, const byte *buf, size_t size, int flags )

{
	if( !GX_CheckTexName( name )) return 0;

	gl_texture_t *tex;
	if(( tex = GL_TextureForName( name )))
		return (int)( tex - gl_textures );

	uint picFlags = 0;
	if( FBitSet( flags, TF_NOFLIP_TGA ))   SetBits( picFlags, IL_DONTFLIP_TGA );
	if( FBitSet( flags, TF_KEEP_SOURCE ) && !FBitSet( flags, TF_EXPAND_SOURCE ))
		SetBits( picFlags, IL_KEEP_8BIT );

	gEngfuncs.Image_SetForceFlags( picFlags );

	rgbdata_t *pic = gEngfuncs.FS_LoadImage( name, buf, size );
	if( !pic ) return 0;

	tex = GL_AllocTexture( name, flags );
	GX_ProcessImage( tex, pic );

	if( !GX_UploadTexture( tex, pic ))
	{
		memset( tex, 0, sizeof( gl_texture_t ));
		gEngfuncs.FS_FreeImage( pic );
		return 0;
	}

	GX_ApplyTextureParams( tex );

	gEngfuncs.FS_FreeImage( pic );
	return (int)( tex - gl_textures );
}

int GX_LoadTextureArray( const char **names, int flags )
{
	gEngfuncs.Con_DPrintf( S_ERROR "%s: texture arrays not supported on GX\n", __func__ );
	return 0;
}

static int GX_LoadTextureFromBuffer_impl( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	if( !GX_CheckTexName( name )) return 0;

	gl_texture_t *tex;
	if(( tex = GL_TextureForName( name )) && !update )
		return (int)( tex - gl_textures );

	if( !pic ) return 0;

	if( update )
	{
		if( tex == NULL )
			gEngfuncs.Host_Error( "%s: couldn't find texture %s for update\n", __func__, name );
		SetBits( tex->flags, flags );
	}
	else
	{
		tex = GL_AllocTexture( name, flags );
	}

	GX_ProcessImage( tex, pic );

	if( !GX_UploadTexture( tex, pic ))
	{
		memset( tex, 0, sizeof( gl_texture_t ));
		return 0;
	}

	GX_ApplyTextureParams( tex );

	return (int)( tex - gl_textures );
}

int GX_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	qboolean update = FBitSet( flags, TF_UPDATE ) ? true : false;
	ClearBits( flags, TF_UPDATE );

	rgbdata_t r_empty;
	memset( &r_empty, 0, sizeof( r_empty ));
	r_empty.width  = width;
	r_empty.height = height;
	r_empty.type   = PF_RGBA_32;
	r_empty.size   = r_empty.width * r_empty.height * 4;
	r_empty.buffer = (byte *)buffer;

	ClearBits( flags, TF_TEXTURE_3D );

	if( !FBitSet( flags, TF_LUMINANCE ) && !FBitSet( flags, TF_ALPHACONTRAST ))
		SetBits( r_empty.flags, IMAGE_HAS_COLOR );
	if( FBitSet( flags, TF_HAS_ALPHA ))
		SetBits( r_empty.flags, IMAGE_HAS_ALPHA );

	if( FBitSet( flags, TF_CUBEMAP ))
	{
		gEngfuncs.Con_DPrintf( S_ERROR "%s: cubemap not supported on GX (%s)\n", __func__, name );
		return 0;
	}

	int texnum = GX_LoadTextureFromBuffer( name, &r_empty, flags, update );

	if( !Q_strcmp( name, REF_DEFAULT_TEXTURE ))   tr.defaultTexture  = texnum;
	else if( !Q_strcmp( name, REF_PARTICLE_TEXTURE )) tr.particleTexture = texnum;
	else if( !Q_strcmp( name, REF_WHITE_TEXTURE ))    tr.whiteTexture    = texnum;
	else if( !Q_strcmp( name, REF_GRAY_TEXTURE ))     tr.grayTexture     = texnum;
	else if( !Q_strcmp( name, REF_BLACK_TEXTURE ))    tr.blackTexture    = texnum;

	return texnum;
}

int GX_CreateTextureArray( const char *name, int width, int height, int depth, const void *buffer, texFlags_t flags )
{
	gEngfuncs.Con_DPrintf( S_ERROR "%s: texture arrays not supported on GX (%s)\n", __func__, name );
	return 0;
}

int GX_FindTexture( const char *name )
{
	if( !GX_CheckTexName( name )) return 0;
	gl_texture_t *tex;
	if(( tex = GL_TextureForName( name )))
		return (int)( tex - gl_textures );
	return 0;
}

void GX_FreeTexture( unsigned int texnum )
{
	if( texnum == 0 || texnum >= MAX_TEXTURES )
		return;
	GL_DeleteTexture( &gl_textures[texnum] );
}

void GX_ProcessTexture( int texnum, float gamma, int topColor, int bottomColor )
{
	int flags = 0;
	if( texnum <= 0 || texnum >= MAX_TEXTURES ) return;
	gl_texture_t *image = &gl_textures[texnum];

	if( gamma != -1.0f )          flags = IMAGE_LIGHTGAMMA;
	else if( topColor != -1 && bottomColor != -1 ) flags = IMAGE_REMAP;
	else
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: bad operation for %s\n", __func__, image->name );
		return;
	}

	if( !image->original )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: no input data for %s\n", __func__, image->name );
		return;
	}
	if( ImageCompressed( image->original->type ))
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: can't process compressed texture %s\n", __func__, image->name );
		return;
	}

	rgbdata_t *pic = gEngfuncs.FS_CopyImage( image->original );
	if( pic->type == PF_INDEXED_24 || pic->type == PF_INDEXED_32 )
		flags |= IMAGE_FORCE_RGBA;

	gEngfuncs.Image_Process( &pic, topColor, bottomColor, flags, 0.0f );

	if( GX_UploadTexture( image, pic ))
		GX_ApplyTextureParams( image );

	gEngfuncs.FS_FreeImage( pic );
}

int GX_TexMemory( void )
{
	int total = 0;
	for( int i = 0; i < (int)gl_numTextures; i++ )
		total += (int)gl_textures[i].size;
	return total;
}

void R_InitDlightTexture( void )
{
	rgbdata_t r_image =
	{
		.width  = BLOCK_SIZE,
		.height = BLOCK_SIZE,
		.type   = PF_RGBA_32,
		.flags  = IMAGE_HAS_COLOR,
		.size   = BLOCK_SIZE * BLOCK_SIZE * 4,
	};
	qboolean update = ( tr.dlightTexture != 0 );
	tr.dlightTexture = GX_LoadTextureFromBuffer( "*dlight", &r_image,
		TF_NOMIPMAP | TF_CLAMP | TF_ATLAS_PAGE, update );
}

static const char *GX_FormatToString( u8 fmt )
{
	switch( fmt )
	{
	case GX_TF_RGBA8:   return "RGBA8 ";
	case GX_TF_RGB565:  return "RGB565";
	case GX_TF_RGB5A3:  return "RGB5A3";
	case GX_TF_IA8:     return "IA8   ";
	case GX_TF_I8:      return "I8    ";
	case GX_TF_I4:      return "I4    ";
	case GX_TF_CMPR:    return "CMPR  ";
	default:            return "??    ";
	}
}

void R_TextureList_f( void )
{
	int bytes = 0;
	gEngfuncs.Con_Printf( "\n" );
	gEngfuncs.Con_Printf( " -id-   -w-  -h-     -size-  -fmt-  -data-  -encode-  -wrap-  -depth-  -name--------\n" );

	int texCount = 0;
	gl_texture_t *image = gl_textures;
	for( int i = 0; i < (int)gl_numTextures; i++, image++ )
	{
		if( !image->texnum ) continue;
		bytes += (int)image->size;
		texCount++;

		gEngfuncs.Con_Printf( "%4i: ", i );
		gEngfuncs.Con_Printf( "%4i %4i ", image->width, image->height );
		gEngfuncs.Con_Printf( "%12s ", Q_memprint( image->size ));
		gEngfuncs.Con_Printf( "%s ", GX_FormatToString( image->format ));
		gEngfuncs.Con_Printf( " 2D    " );

		if( image->flags & 0 )
			gEngfuncs.Con_Printf( "normal  " );
		else
			gEngfuncs.Con_Printf( "diffuse " );

		switch( (int)(image->encode) )
		{
		case DXT_ENCODE_COLOR_YCoCg:          gEngfuncs.Con_Printf( "YCoCg     " ); break;
		case DXT_ENCODE_NORMAL_AG_ORTHO:      gEngfuncs.Con_Printf( "ortho     " ); break;
		case DXT_ENCODE_NORMAL_AG_STEREO:     gEngfuncs.Con_Printf( "stereo    " ); break;
		case DXT_ENCODE_NORMAL_AG_PARABOLOID: gEngfuncs.Con_Printf( "parabolic " ); break;
		case DXT_ENCODE_NORMAL_AG_QUARTIC:    gEngfuncs.Con_Printf( "quartic   " ); break;
		case DXT_ENCODE_NORMAL_AG_AZIMUTHAL:  gEngfuncs.Con_Printf( "azimuthal " ); break;
		default:                              gEngfuncs.Con_Printf( "default   " ); break;
		}

		if( image->flags & TF_CLAMP )       gEngfuncs.Con_Printf( "clamp  " );
		else if( image->flags & TF_BORDER ) gEngfuncs.Con_Printf( "clamp  " );
		else                                gEngfuncs.Con_Printf( "repeat " );
		gEngfuncs.Con_Printf( "   %d  ", image->depth );
		gEngfuncs.Con_Printf( "  %s\n", image->name );
	}

	gEngfuncs.Con_Printf( "---------------------------------------------------------\n" );
	gEngfuncs.Con_Printf( "%i total textures\n", texCount );
	gEngfuncs.Con_Printf( "%s total memory used\n", Q_memprint( bytes ));
	gEngfuncs.Con_Printf( "\n" );
}

void R_InitImages( void )
{
	GX_EnsureTextureTable();
	memset( gl_textures,          0, MAX_TEXTURES * sizeof( *gl_textures ));
	memset( gl_texturesHashTable, 0, sizeof( gl_texturesHashTable ));
	gl_numTextures = 0;

	Q_strncpy( gl_textures->name, "*unused*", sizeof( gl_textures->name ));
	gl_textures->hashValue = COM_HashKey( gl_textures->name, TEXTURES_HASH_SIZE );
	gl_textures->nextHash  = gl_texturesHashTable[gl_textures->hashValue];
	gl_texturesHashTable[gl_textures->hashValue] = gl_textures;
	gl_numTextures = 1;

	R_SetTextureParameters();
	gEngfuncs.Cmd_AddCommand( "texturelist", R_TextureList_f, "display loaded textures list" );
}

void R_ShutdownImages( void )
{
	gEngfuncs.Cmd_RemoveCommand( "texturelist" );
	GX_CleanupAllTextureUnits();

	gl_texture_t *tex = gl_textures;
	for( int i = 0; i < (int)gl_numTextures; i++, tex++ )
		GL_DeleteTexture( tex );

	memset( tr.lightmapTextures,  0, sizeof( tr.lightmapTextures ));
	memset( gl_texturesHashTable, 0, sizeof( gl_texturesHashTable ));
	GX_EnsureTextureTable();
	memset( gl_textures,          0, MAX_TEXTURES * sizeof( *gl_textures ));
	gl_numTextures = 0;
}

void R_TextureReplacementReport( const char *modelname, int gl_texturenum, const char *foundpath )
{
	if( ref_host_allow_materials->value != 2.0f ) return;
	if( gl_texturenum > 0 )
		gEngfuncs.Con_Printf( "Looking for %s tex replacement..." S_GREEN "OK (%s)\n", modelname, foundpath );
	else if( gl_texturenum < 0 )
		gEngfuncs.Con_Printf( "Looking for %s tex replacement..." S_YELLOW "MISS (%s)\n", modelname, foundpath );
	else
		gEngfuncs.Con_Printf( "Looking for %s tex replacement..." S_RED "FAIL (%s)\n", modelname, foundpath );
}

qboolean R_SearchForTextureReplacement( char *out, size_t size, const char *modelname, const char *fmt, ... )
{
	va_list ap;
	va_start( ap, fmt );
	int ret = Q_vsnprintf( out, size, fmt, ap );
	va_end( ap );

	if( ret < 0 )
	{
		R_TextureReplacementReport( modelname, -1, "overflow" );
		return false;
	}
	if( gEngfuncs.fsapi->FileExists( out, false ))
		return true;
	R_TextureReplacementReport( modelname, -1, out );
	return false;
}

void R_ShowTextures( void )
{
	static qboolean showHelp = true;

	if( !ref_r_showtextures->value )
		return;

	if( showHelp )
	{
		gEngfuncs.CL_CenterPrint( "use '<-' and '->' keys to change atlas page, ESC to quit", 0.25f );
		showHelp = false;
	}

	float w = 200.0f;
	float h = 200.0f;

	float time = gp_cl->time * 0.5f;
	time -= floorf( time );

	int charHeight;
	gEngfuncs.Con_DrawStringLen( NULL, NULL, &charHeight );

	int base_w   = (int)( gpGlobals->width  / w );
	int base_h   = (int)( gpGlobals->height / ( h + charHeight * 2 ));
	int per_page = base_w * base_h;
	int start    = per_page * ( (int)ref_r_showtextures->value - 1 ) + 1;

	qboolean empty_page = true;
	int skipped_empty_pages = 0;
	while( empty_page )
	{
		for( int k = 0; k < per_page; k++ )
		{
			int i = k + start;
			if( i >= MAX_TEXTURES ) { empty_page = false; break; }
			if( gl_textures[i].texnum != 0 ) { empty_page = false; break; }
		}
		if( empty_page ) { start += per_page; skipped_empty_pages++; }
	}

	if( skipped_empty_pages > 0 )
	{
		char text[MAX_VA_STRING];
		Q_snprintf( text, sizeof( text ), "%s: skipped %d empty texture pages",
			__func__, skipped_empty_pages );
		gEngfuncs.CL_CenterPrint( text, 0.25f );
	}

	const rgba_t color = { 255, 255, 255, 255 };

	R_Set2DMode( true );

	for( int k = 0; k < per_page; k++ )
	{
		int i = k + start;
		if( i >= MAX_TEXTURES ) break;
		const gl_texture_t *image = R_GetTexture( i );
		if( !image->texnum ) continue;

		float x = (float)( k % base_w ) * gpGlobals->width  / (float)base_w;
		float y = (float)( k / base_w ) * gpGlobals->height / (float)base_h;

		GX_Bind( XASH_TEXTURE0, i );

		GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );
			GX_Position2f32( x,     y     ); GX_TexCoord2f32( 0.0f, 0.0f );
			GX_Position2f32( x + w, y     ); GX_TexCoord2f32( 1.0f, 0.0f );
			GX_Position2f32( x + w, y + h ); GX_TexCoord2f32( 1.0f, 1.0f );
			GX_Position2f32( x,     y + h ); GX_TexCoord2f32( 0.0f, 1.0f );
		GX_End();

		string shortname;
		COM_FileBase( image->name, shortname, sizeof( shortname ));
		int textlen;
		gEngfuncs.Con_DrawStringLen( shortname, &textlen, NULL );
		if( textlen > (int)w )
		{
			shortname[16] = '.';
			shortname[17] = '.';
			shortname[18] = '\0';
		}
		gEngfuncs.Con_DrawString( (int)x + 1, (int)( y + h ), shortname, color );

		char text[MAX_VA_STRING];
		Q_snprintf( text, sizeof( text ), "%ix%i 2D", image->width, image->height );
		gEngfuncs.Con_DrawString( (int)x + 1, (int)( y + h ) + charHeight, text, color );

		Q_strncpy( text, Q_memprint( image->size ), sizeof( text ));
		gEngfuncs.Con_DrawStringLen( text, &textlen, NULL );
		gEngfuncs.Con_DrawString( (int)( x + w ) - textlen - 1,
			(int)( y + h ) + charHeight, text, color );
	}

	R_Set2DMode( false );

	gEngfuncs.CL_DrawCenterPrint();

	GX_DrawDone();
}
// ============================================================
// Sistema de gestión de texturas nativo para GX (Wii)
// Implementado para eliminar la dependencia del renderizador soft
// ============================================================


// ============================================================
// Definición mínima de gl_state para GX (independiente del soft)
// ============================================================
// El motor base espera una variable global llamada gl_state.
// Como GX no usa el sistema de texturas del soft, la definimos
// como un array de bytes vacío para satisfacer al enlazador.
unsigned char gl_state[4096];
