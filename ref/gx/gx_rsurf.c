/*
gx_rsurf.c - surface-related refresh code (Wii GX native port)
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

#include <stdio.h>
#include "gx_local.h"

#include "xash3d_mathlib.h"
#include "mod_local.h"
#include "atlas.h"
#include "gx_vbo.h"

#define TURBSCALE		( 256.0f / ( M_PI2 ))

static const float r_turbsin[] =
{
#include "warpsin.h"



};

typedef struct
{
	atlas_t		atlas;
	int		current_lightmap_texture;
	msurface_t	*dynamic_surfaces;
	msurface_t	*lightmap_surfaces[MAX_LIGHTMAPS];
	byte		lightmap_buffer[BLOCK_SIZE_MAX*BLOCK_SIZE_MAX*4];
} gxlightmapstate_t;

static vec2_t		world_orthocenter;
static vec2_t		world_orthohalf;
static uint		r_blocklights[BLOCK_SIZE_MAX*BLOCK_SIZE_MAX*3];
static mextrasurf_t		*fullbright_surfaces[MAX_TEXTURES];
static mextrasurf_t		*detail_surfaces[MAX_TEXTURES];

typedef struct
{
	int first, last;
} separate_pass_t;

static separate_pass_t draw_wateralpha = { 0, -1 };
static separate_pass_t draw_alpha_surfaces = { 0, -1 };
static separate_pass_t draw_fullbrights = { 0, -1 };
static separate_pass_t draw_details = { 0, -1 };
static msurface_t		*skychain = NULL;
static gxlightmapstate_t	gx_lms;

static void LM_UploadBlock( qboolean dynamic );
static qboolean R_AddSurfToVBO( msurface_t *surf, qboolean buildlightmaps );
static void R_DrawVBO( qboolean drawlightmaps, qboolean drawtextures );

static void GX_SetupVtxFormat( void )
{
	GX_SetChanVtxColor( false );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS,  GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX1, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0 );
}

static inline void R_AddToSeparatePass( separate_pass_t *sp, int num )
{
	if( sp->first > num )
		sp->first = num;
	if( sp->last < num )
		sp->last = num;
}

static inline void R_ResetSeparatePass( separate_pass_t *sp )
{
	sp->last = -1;
}

static inline qboolean R_SeparatePassActive( const separate_pass_t *sp )
{
	return sp->last >= 0 ? true : false;
}

static qboolean Mod_HaveLightmappedWater( void )
{
	if( FBitSet( tr.world->flags, FWORLD_HAS_LITWATER ))
		return true;
	return gl_litwater_force.value ? true : false;
}

static int Mod_LightmappedWaterMinlight( void )
{
	if( FBitSet( tr.world->flags, FWORLD_HAS_LITWATER ))
	{
		if( tr.world->litwater_minlight >= 0 )
			return tr.world->litwater_minlight;
	}
	return 0;
}

static float Mod_LightmappedWaterScale( void )
{
	if( FBitSet( tr.world->flags, FWORLD_HAS_LITWATER ))
	{
		if( tr.world->litwater_scale >= 0.0f )
			return tr.world->litwater_scale;
	}
	return 1.0f;
}

byte *Mod_GetCurrentVis( void )
{
	if( gEngfuncs.drawFuncs->Mod_GetCurrentVis && tr.fCustomRendering )
		return gEngfuncs.drawFuncs->Mod_GetCurrentVis();
	return RI.visbytes;
}

void Mod_SetOrthoBounds( const float *mins, const float *maxs )
{
	if( gEngfuncs.drawFuncs->GL_OrthoBounds )
	{
		gEngfuncs.drawFuncs->GL_OrthoBounds( mins, maxs );
	}
	Vector2Average( maxs, mins, world_orthocenter );
	Vector2Subtract( maxs, world_orthocenter, world_orthohalf );
}

void R_LightmapCoord( const vec3_t v, const msurface_t *surf, const float sample_size, vec2_t coords )
{
	const mextrasurf_t *info = surf->info;

	float s = DotProduct( v, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
	s += surf->light_s * sample_size;
	s += sample_size * 0.5f;
	s /= BLOCK_SIZE * sample_size;

	float t = DotProduct( v, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];
	t += surf->light_t * sample_size;
	t += sample_size * 0.5f;
	t /= BLOCK_SIZE * sample_size;

	Vector2Set( coords, s, t );
}

static void R_TextureCoord( const vec3_t v, const msurface_t *surf, vec2_t coords )
{
	const mtexinfo_t *info = surf->texinfo;

	float s = DotProduct( v, info->vecs[0] );
	float t = DotProduct( v, info->vecs[1] );

	if( !FBitSet( surf->flags, SURF_DRAWTURB ))
	{
		s = ( s + info->vecs[0][3] ) / info->texture->width;
		t = ( t + info->vecs[1][3] ) / info->texture->height;
	}

	Vector2Set( coords, s, t );
}

static void R_GetEdgePosition( const model_t *mod, const msurface_t *fa, int i, vec3_t vec )
{
	const int lindex = mod->surfedges[fa->firstedge + i];

	if( FBitSet( mod->flags, MODEL_QBSP2 ))
	{
		const medge32_t *pedges = mod->edges32;

		if( lindex > 0 )
			VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, vec );
		else
			VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, vec );
	}
	else
	{
		const medge16_t *pedges = mod->edges16;

		if( lindex > 0 )
			VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, vec );
		else
			VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, vec );
	}
}

static void BoundPoly( int numverts, float *verts, vec3_t mins, vec3_t maxs )
{
	float	*v;
	int	i;

	ClearBounds( mins, maxs );

	for( i = 0, v = verts; i < numverts; i++ )
	{
		for( int j = 0; j < 3; j++, v++ )
		{
			if( *v < mins[j] ) mins[j] = *v;
			if( *v > maxs[j] ) maxs[j] = *v;
		}
	}
}

static void SubdividePolygon_r( model_t *loadmodel, msurface_t *warpface, int numverts, float *verts )
{
	vec3_t		front[SUBDIVIDE_SIZE], back[SUBDIVIDE_SIZE];
	float		dist[SUBDIVIDE_SIZE];
	float		*v;
	int		j;
	vec3_t		mins, maxs;

	if( numverts > ( SUBDIVIDE_SIZE - 4 ))
		gEngfuncs.Host_Error( "%s: too many vertexes on face ( %i )\n", __func__, numverts );

	BoundPoly( numverts, verts, mins, maxs );

	for( int i = 0; i < 3; i++ )
	{
		float m = ( mins[i] + maxs[i] ) * 0.5f;
		m = SUBDIVIDE_SIZE * floor( m / SUBDIVIDE_SIZE + 0.5f );
		if( maxs[i] - m < 8 ) continue;
		if( m - mins[i] < 8 ) continue;

		v = verts + i;
		for( j = 0; j < numverts; j++, v += 3 )
			dist[j] = *v - m;

		dist[j] = dist[0];
		v -= i;
		VectorCopy( verts, v );

		int f = 0, b = 0;
		v = verts;
		for( j = 0; j < numverts; j++, v += 3 )
		{
			if( dist[j] >= 0 )
			{
				VectorCopy( v, front[f] );
				f++;
			}

			if( dist[j] <= 0 )
			{
				VectorCopy (v, back[b]);
				b++;
			}

			if( dist[j] == 0 || dist[j+1] == 0 )
				continue;

			if(( dist[j] > 0 ) != ( dist[j+1] > 0 ))
			{
				float frac = dist[j] / ( dist[j] - dist[j+1] );
				for( int k = 0; k < 3; k++ )
					front[f][k] = back[b][k] = v[k] + frac * (v[3+k] - v[k]);
				f++;
				b++;
			}
		}

		SubdividePolygon_r( loadmodel, warpface, f, front[0] );
		SubdividePolygon_r( loadmodel, warpface, b, back[0] );
		return;
	}

	if( numverts != 4 )
		ClearBits( warpface->flags, SURF_DRAWTURB_QUADS );

	glpoly2_t *poly = Mem_Calloc( loadmodel->mempool, sizeof( glpoly2_t ) + numverts * VERTEXSIZE * sizeof( float ));
	poly->next = warpface->polys;
	poly->flags = warpface->flags;
	warpface->polys = poly;
	poly->numverts = numverts;

	for( int i = 0; i < numverts; i++, verts += 3 )
	{
		VectorCopy( verts, poly->verts[i] );
		R_TextureCoord( verts, warpface, &poly->verts[i][3] );
	}
}

static void GX_BuildLightmapWater( model_t *mod, msurface_t *fa )
{
	if( !mod || !fa->texinfo || !fa->texinfo->texture )
		return;

	float sample_size = gEngfuncs.Mod_SampleSizeForFace( fa );

	for( glpoly2_t *poly = fa->polys; poly; poly = poly->next )
	{
		for( int i = 0; i < poly->numverts; i++ )
		{
			vec3_t vec = Vec3( poly->verts[i] );
			R_LightmapCoord( vec, fa, sample_size, &poly->verts[i][5] );
		}
	}
}

static void GX_SetupFogColorForSurfacesEx( int passes, float density, qboolean blend_lightmaps )
{
	if( !glState.isFogEnabled )
		return;

	if(( passes < 2 ) || (RI.currententity && RI.currententity->curstate.rendermode == kRenderTransTexture ))
		return;

	float div = passes - 1;
	float factor = passes;
	vec4_t fogColor;
	fogColor[0] = pow( RI.fogColor[0] / div, ( 1.0f / factor ));
	fogColor[1] = pow( RI.fogColor[1] / div, ( 1.0f / factor ));
	fogColor[2] = pow( RI.fogColor[2] / div, ( 1.0f / factor ));
	fogColor[3] = 1.0f;

	if( blend_lightmaps && gl_overbright.value )
		VectorScale( fogColor, 0.5f, fogColor );

	VectorCopy( fogColor, RI.fogColor );
	RI.fogDensity *= density;

	R_DrawFog();
}

void GX_SetupFogColorForSurfaces( void )
{
	GX_SetupFogColorForSurfacesEx( r_detailtextures.value ? 3 : 2, 1.0f, false );
}

void GX_ResetFogColor( void )
{
	if( glState.isFogEnabled )
	{
		R_DrawFog();
	}
}

void GX_SubdivideSurface( model_t *loadmodel, msurface_t *fa )
{
	vec3_t	verts[SUBDIVIDE_SIZE];

	for( int i = 0; i < fa->numedges; i++ )
		R_GetEdgePosition( loadmodel, fa, i, verts[i] );

	SetBits( fa->flags, SURF_DRAWTURB_QUADS );
	SubdividePolygon_r( loadmodel, fa, fa->numedges, verts[0] );
}

static int GX_BuildPolygonFromSurface( model_t *mod, msurface_t *fa )
{
	int		nColinElim = 0;

	if( !mod || !fa->texinfo || !fa->texinfo->texture )
		return nColinElim;

	if( FBitSet( fa->flags, SURF_CONVEYOR ) && fa->texinfo->texture->gl_texturenum != 0 )
	{
		gl_texture_t *glt = R_GetTexture( fa->texinfo->texture->gl_texturenum );
		texture_t *tex = fa->texinfo->texture;
		Assert( glt != NULL && tex != NULL );

		glt->srcWidth = tex->width;
		glt->srcHeight = tex->height;
	}

	float sample_size = gEngfuncs.Mod_SampleSizeForFace( fa );

	int lnumverts = fa->numedges;

	glpoly2_t *poly = fa->polys;
	fa->polys = NULL;

	poly = Mem_Realloc( mod->mempool, poly, sizeof( glpoly2_t ) + lnumverts * VERTEXSIZE * sizeof( float ));
	poly->next = fa->polys;
	poly->flags = fa->flags;
	fa->polys = poly;
	poly->numverts = lnumverts;

	for( int i = 0; i < lnumverts; i++ )
	{
		R_GetEdgePosition( mod, fa, i, poly->verts[i] );
		R_TextureCoord( poly->verts[i], fa, &poly->verts[i][3] );
		R_LightmapCoord( poly->verts[i], fa, sample_size, &poly->verts[i][5] );
	}

	if( !gl_keeptjunctions.value && !FBitSet( fa->flags, SURF_UNDERWATER ))
	{
		for( int i = 0; i < lnumverts; i++ )
		{
			vec3_t	v1, v2;

			float *prev = poly->verts[(i + lnumverts - 1) % lnumverts];
			float *next = poly->verts[(i + 1) % lnumverts];
			float *this = poly->verts[i];

			VectorSubtract( this, prev, v1 );
			VectorNormalize( v1 );
			VectorSubtract( next, prev, v2 );
			VectorNormalize( v2 );

			if(( fabs( v1[0] - v2[0] ) <= 0.001f) && (fabs( v1[1] - v2[1] ) <= 0.001f) && (fabs( v1[2] - v2[2] ) <= 0.001f))
			{
				for( int j = i + 1; j < lnumverts; j++ )
				{
					for( int k = 0; k < VERTEXSIZE; k++ )
						poly->verts[j-1][k] = poly->verts[j][k];
				}
				lnumverts--;
				nColinElim++;
				i--;
			}
		}
	}

	poly->numverts = lnumverts;
	return nColinElim;
}

static texture_t *R_TextureAnim( texture_t *b )
{
	texture_t *base = b;
	int	reletive;

	if( RI.currententity->curstate.frame )
	{
		if( base->alternate_anims )
			base = base->alternate_anims;
	}

	if( !base->anim_total )
		return base;

	if( base->name[0] == '-' )
	{
		return b;
	}
	else
	{
		int	speed;
		if( FBitSet( R_GetTexture( base->gl_texturenum )->flags, TF_QUAKEPAL ))
			speed = 10;
		else speed = 20;

		reletive = (int)(gp_cl->time * speed) % base->anim_total;
	}

	int count = 0;

	while( base->anim_min > reletive || base->anim_max <= reletive )
	{
		base = base->anim_next;

		if( !base || ++count > MOD_FRAMES )
			return b;
	}

	return base;
}

static texture_t *R_TextureAnimation( msurface_t *s )
{
	texture_t	*base = s->texinfo->texture;
	int	reletive;

	if( RI.currententity && RI.currententity->curstate.frame )
	{
		if( base->alternate_anims )
			base = base->alternate_anims;
	}

	if( !base->anim_total )
		return base;

	if( base->name[0] == '-' )
	{
		int	tx = (int)((s->texturemins[0] + (base->width << 16)) / base->width) % MOD_FRAMES;
		int	ty = (int)((s->texturemins[1] + (base->height << 16)) / base->height) % MOD_FRAMES;

		reletive = rtable[tx][ty] % base->anim_total;
	}
	else
	{
		int	speed;
		if( FBitSet( R_GetTexture( base->gl_texturenum )->flags, TF_QUAKEPAL ))
			speed = 10;
		else speed = 20;

		reletive = (int)(gp_cl->time * speed) % base->anim_total;
	}

	int count = 0;

	while( base->anim_min > reletive || base->anim_max <= reletive )
	{
		base = base->anim_next;

		if( !base || ++count > MOD_FRAMES )
			return s->texinfo->texture;
	}

	return base;
}

static void R_AddDynamicLights( const msurface_t *surf, float sample_size, int smax, int tmax )
{
	const mextrasurf_t *info = surf->info;
	int sample_frac = 1.0;

	if( !surf->dlightbits )
		return;

	mtexinfo_t *tex = surf->texinfo;

	if( FBitSet( tex->flags, TEX_WORLD_LUXELS ))
	{
		if( surf->texinfo->faceinfo )
			sample_frac = surf->texinfo->faceinfo->texture_step;
		else if( FBitSet( surf->texinfo->flags, TEX_EXTRA_LIGHTMAP ))
			sample_frac = LM_SAMPLE_EXTRASIZE;
		else sample_frac = LM_SAMPLE_SIZE;
	}

	for( int lnum = 0; lnum < MAX_DLIGHTS; lnum++ )
	{
		vec3_t impact, origin_l;

		if( !FBitSet( surf->dlightbits, BIT( lnum )))
			continue;

		dlight_t *dl = &gp_dlights[lnum];

		if( !tr.modelviewIdentity )
			Matrix4x4_VectorITransform( RI.objectMatrix, dl->origin, origin_l );
		else VectorCopy( dl->origin, origin_l );

		float rad = dl->radius;
		float dist = PlaneDiff( origin_l, surf->plane );
		rad -= fabs( dist );

		float minlight = dl->minlight;
		if( rad < minlight )
			continue;

		minlight = rad - minlight;

		if( surf->plane->type < 3 )
		{
			VectorCopy( origin_l, impact );
			impact[surf->plane->type] -= dist;
		}
		else VectorMA( origin_l, -dist, surf->plane->normal, impact );

		float sl = DotProduct( impact, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
		float tl = DotProduct( impact, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];

		float half = ( minlight + 1.0f ) / ( sample_size * sample_frac );
		int s0 = Q_max( 0, (int)( sl / sample_size - half ));
		int s1 = Q_min( smax - 1, (int)( sl / sample_size + half ));
		int t0 = Q_max( 0, (int)( tl / sample_size - half ));
		int t1 = Q_min( tmax - 1, (int)( tl / sample_size + half ));

		for( int t = t0; t <= t1; t++ )
		{
			int td = (tl - sample_size * t) * sample_frac;

			if( td < 0 )
				td = -td;

			for( int s = s0; s <= s1; s++ )
			{
				int sd = (sl - sample_size * s) * sample_frac;
				float dist;

				if( sd < 0 )
					sd = -sd;

				if( sd > td )
					dist = sd + (td >> 1);
				else
					dist = td + (sd >> 1);

				if( dist < minlight )
				{
					uint *bl = &r_blocklights[(s + (t * smax)) * 3];

					bl[0] += ((int)((rad - dist) * 256) * dl->color.r ) / 256;
					bl[1] += ((int)((rad - dist) * 256) * dl->color.g ) / 256;
					bl[2] += ((int)((rad - dist) * 256) * dl->color.b ) / 256;
				}
			}
		}
	}
}

static void LM_InitBlock( void )
{
	Atlas_Init( &gx_lms.atlas, BLOCK_SIZE );
}

static qboolean LM_AllocBlock( int w, int h, int *x, int *y )
{
	return Atlas_AllocBlock( &gx_lms.atlas, w, h, x, y );
}

static void LM_UploadDynamicBlock( void )
{
	/* siempre la pagina completa: si se subiera solo max_height, la textura se achicaria y las UV quedarian mal */
	GX_UpdateTexture( tr.dlightTexture,
		BLOCK_SIZE, BLOCK_SIZE,
		BLOCK_SIZE, BLOCK_SIZE,
		gx_lms.lightmap_buffer, PF_RGBA_32 );
}

static void LM_UploadBlock( qboolean dynamic )
{
	if( dynamic )
	{
		LM_UploadDynamicBlock();
	}
	else
	{
		rgbdata_t	r_lightmap;
		char	lmName[16];
		int i = gx_lms.current_lightmap_texture;

		memset( &r_lightmap, 0, sizeof( r_lightmap ));
		Q_snprintf( lmName, sizeof( lmName ), "*lightmap%i", i );

		r_lightmap.width = BLOCK_SIZE;
		r_lightmap.height = BLOCK_SIZE;
		r_lightmap.type = PF_RGBA_32;
		r_lightmap.size = r_lightmap.width * r_lightmap.height * 4;
		r_lightmap.flags = IMAGE_HAS_COLOR;
		r_lightmap.buffer = gx_lms.lightmap_buffer;

		if( gx_testmode == 12 )
		{
			for( int k = 0; k < BLOCK_SIZE * BLOCK_SIZE; k++ )
			{
				gx_lms.lightmap_buffer[k*4+0] = 128;
				gx_lms.lightmap_buffer[k*4+1] = 128;
				gx_lms.lightmap_buffer[k*4+2] = 128;
				gx_lms.lightmap_buffer[k*4+3] = 255;
			}
		}
		else if( gx_testmode == 13 )
		{
			for( int k = 0; k < BLOCK_SIZE * BLOCK_SIZE; k++ )
			{
				gx_lms.lightmap_buffer[k*4+0] = (byte)(( k % BLOCK_SIZE ) * 255 / ( BLOCK_SIZE - 1 ));
				gx_lms.lightmap_buffer[k*4+1] = (byte)(( k / BLOCK_SIZE ) * 255 / ( BLOCK_SIZE - 1 ));
				gx_lms.lightmap_buffer[k*4+2] = 0;
				gx_lms.lightmap_buffer[k*4+3] = 255;
			}
		}



		tr.lightmapTextures[i] = GX_CreateTexture( lmName,
			BLOCK_SIZE, BLOCK_SIZE,
			gx_lms.lightmap_buffer,
			TF_NOMIPMAP | TF_CLAMP | TF_ATLAS_PAGE );
        if( i < 3 ) gEngfuncs.Con_Printf("[LMDIAG] p%d px0=%02X%02X%02X%02X px1=%02X%02X%02X%02X\n", i, gx_lms.lightmap_buffer[0], gx_lms.lightmap_buffer[1], gx_lms.lightmap_buffer[2], gx_lms.lightmap_buffer[3], gx_lms.lightmap_buffer[4], gx_lms.lightmap_buffer[5], gx_lms.lightmap_buffer[6], gx_lms.lightmap_buffer[7]);

		if( ++gx_lms.current_lightmap_texture == MAX_LIGHTMAPS )
			gEngfuncs.Host_Error( "%s: full\n", __func__ );
	}
}

static void R_BuildLightMap( const msurface_t *surf, byte *restrict dest, int stride, qboolean dynamic )
{
	const mextrasurf_t *info = surf->info;
	const qboolean turb = FBitSet( surf->flags, SURF_DRAWTURB );
	const qboolean linear_gamma = FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE );
	const uint *restrict lightgammatable = tr.lightgammatable;

	int lightscale;

	const int litwater_minlight = Mod_LightmappedWaterMinlight();
	const float litwater_scale = Mod_LightmappedWaterScale();
	const int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
	const int smax = ( info->lightextents[0] / sample_size ) + 1;
	const int tmax = ( info->lightextents[1] / sample_size ) + 1;
	const int size = smax * tmax;

	if( gl_overbright.value )
		lightscale = ( R_HasEnabledVBO() && !r_vbo_overbrightmode.value) ? 171 : 256;
	else
		lightscale = ( pow( 2.0f, 1.0f / v_lightgamma->value ) * 256 ) + 0.5;

	int map;
	qboolean init = false;

	for( map = 0; map < MAXLIGHTMAPS && surf->samples; map++ )
	{
		if( surf->styles[map] >= 255 )
			break;

		uint scale = g_lightstylevalue[surf->styles[map]];
		const color24 *lm = &surf->samples[map * size];
		for( int i = 0; i < size; i++ )
		{
			r_blocklights[i * 3 + 0] = lm[i].r * scale;
			r_blocklights[i * 3 + 1] = lm[i].g * scale;
			r_blocklights[i * 3 + 2] = lm[i].b * scale;
		}
		init = true;
		break;
	}

	if( init )
	{
		for( map++ ; map < MAXLIGHTMAPS; map++ )
		{
			if( surf->styles[map] >= 255 )
				break;

			uint scale = g_lightstylevalue[surf->styles[map]];
			const color24 *lm = &surf->samples[map * size];
			for( int i = 0; i < size; i++ )
			{
				r_blocklights[i * 3 + 0] += lm[i].r * scale;
				r_blocklights[i * 3 + 1] += lm[i].g * scale;
				r_blocklights[i * 3 + 2] += lm[i].b * scale;
			}
		}
	}
	else
		memset( r_blocklights, 0, sizeof( uint ) * size * 3 );

	if( surf->dlightframe == tr.framecount && dynamic )
		R_AddDynamicLights( surf, sample_size, smax, tmax );

	for( int t = 0; t < tmax; t++ )
	{
		for( int s = 0; s < smax; s++ )
		{
			const uint *restrict bl = &r_blocklights[(s + (t * smax)) * 3];
			byte *restrict dst = &dest[(t * stride) + (s * 4)];

			for( int i = 0; i < 3; i++ )
			{
				int t = bl[i] * lightscale >> 14;

				if( turb )
				{
					float ft = t * litwater_scale;
					t = Q_max( Q_rint( ft ), litwater_minlight );
				}

				if( t > 1023 )
					t = 1023;

				if( linear_gamma )
					dst[i] = t >> 2;
				else
					dst[i] = lightgammatable[t] >> 2;
			}
			dst[3] = 255;
		}
	}
}

static void R_DrawTriangleOutlines( void )
{
	if( !gl_wireframe.value )
		return;
}

static void DrawGLPoly( glpoly2_t *p, float xScale, float yScale )
{
    float sOffset, tOffset;
    int total_tris;
    glpoly2_t *q;
    const qboolean hasScale = xScale != 0.0f && yScale != 0.0f;

    if( !p )
        return;

    if( FBitSet( p->flags, SURF_DRAWTILED ))
        GX_ResetFogColor();

    if( FBitSet( p->flags, SURF_CONVEYOR ))
    {
        const cl_entity_t *e = RI.currententity;
        float flConveyorSpeed;
        float sy, cy;

        if( e == CL_GetEntityByIndex( 0 ) && FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
        {
            flConveyorSpeed = -35.0f;
        }
        else
        {
            flConveyorSpeed = (e->curstate.rendercolor.g<<8|e->curstate.rendercolor.b) / 16.0f;
            if( e->curstate.rendercolor.r ) flConveyorSpeed = -flConveyorSpeed;
        }
        gl_texture_t *texture = R_GetTexture( glState.currentTexturesIndex[glState.activeTMU] );

        float flRate = fabs( flConveyorSpeed ) / (float)texture->srcWidth;
        float flAngle = ( flConveyorSpeed >= 0 ) ? 180 : 0;

        SinCos( flAngle * ( M_PI_F / 180.0f ), &sy, &cy );
        sOffset = gp_cl->time * cy * flRate;
        tOffset = gp_cl->time * sy * flRate;

        if( sOffset < 0.0f ) sOffset += 1.0f + -(int)sOffset;
        if( tOffset < 0.0f ) tOffset += 1.0f + -(int)tOffset;

        sOffset = sOffset - (int)sOffset;
        tOffset = tOffset - (int)tOffset;
    }
    else
    {
        sOffset = tOffset = 0.0f;
    }

    // Contar triangulos totales de la cadena
    total_tris = 0;
    for( q = p; q != NULL; q = q->chain )
    {
        if( q->numverts >= 3 )
            total_tris += q->numverts - 2;
    }

    if( total_tris == 0 )
        return;

    GX_SetupVtxFormat();

    // Un solo Begin/End para TODA la cadena
    GX_Begin( GX_TRIANGLES, GX_VTXFMT0, total_tris * 3 );

    for( q = p; q != NULL; q = q->chain )
    {
        float *base = q->verts[0];
        float *v0 = base;

        for( int i = 1; i < q->numverts - 1; i++ )
        {
            float *v1 = base + i * VERTEXSIZE;
            float *v2 = base + (i + 1) * VERTEXSIZE;

            GX_Position3f32( v0[0], v0[1], v0[2] );
            if( hasScale ) GX_TexCoord2f32(( v0[3] + sOffset ) * xScale, ( v0[4] + tOffset ) * yScale );
            else GX_TexCoord2f32( v0[3] + sOffset, v0[4] + tOffset );
            GX_TexCoord2f32( v0[5], v0[6] );

            GX_Position3f32( v1[0], v1[1], v1[2] );
            if( hasScale ) GX_TexCoord2f32(( v1[3] + sOffset ) * xScale, ( v1[4] + tOffset ) * yScale );
            else GX_TexCoord2f32( v1[3] + sOffset, v1[4] + tOffset );
            GX_TexCoord2f32( v1[5], v1[6] );

            GX_Position3f32( v2[0], v2[1], v2[2] );
            if( hasScale ) GX_TexCoord2f32(( v2[3] + sOffset ) * xScale, ( v2[4] + tOffset ) * yScale );
            else GX_TexCoord2f32( v2[3] + sOffset, v2[4] + tOffset );
            GX_TexCoord2f32( v2[5], v2[6] );
        }
    }

    GX_End();

    if( FBitSet( p->flags, SURF_DRAWTILED ))
        GX_SetupFogColorForSurfaces();
}
static void EmitWaterPolys( msurface_t *warp, qboolean reverse, qboolean ripples )
{
	float	waveHeight;
	const qboolean useQuads = FBitSet( warp->flags, SURF_DRAWTURB_QUADS );

	if( !warp->polys ) return;

	if( warp->polys->verts[0][2] >= RI.rvp.vieworigin[2] )
		waveHeight = -RI.currententity->curstate.scale;
	else waveHeight = RI.currententity->curstate.scale;

	GX_ResetFogColor();

	GX_SetupVtxFormat();

	if( useQuads )
		GX_Begin( GX_QUADS, GX_VTXFMT0, 0 );

	for( glpoly2_t *p = warp->polys; p; p = p->next )
	{
		float *v;

		if( reverse )
			v = p->verts[0] + ( p->numverts - 1 ) * VERTEXSIZE;
		else v = p->verts[0];

		if( !useQuads )
			GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, p->numverts );

		for( int i = 0; i < p->numverts; i++ )
		{
			float nv;
			float s, t;

			if( waveHeight )
			{
				nv = r_turbsin[(int)(gp_cl->time * 160.0f + v[1] + v[0]) & 255] + 8.0f;
				nv = (r_turbsin[(int)(v[0] * 5.0f + gp_cl->time * 171.0f - v[1]) & 255] + 8.0f ) * 0.8f + nv;
				nv = nv * waveHeight + v[2];
			}
			else nv = v[2];

			float os = v[3];
			float ot = v[4];

			if( !ripples )
			{
				s = os + r_turbsin[(int)((ot * 0.125f + gp_cl->time) * TURBSCALE) & 255];
				t = ot + r_turbsin[(int)((os * 0.125f + gp_cl->time) * TURBSCALE) & 255];
			}
			else
			{
				s = os;
				t = ot;
			}

			s *= ( 1.0f / SUBDIVIDE_SIZE );
			t *= ( 1.0f / SUBDIVIDE_SIZE );

			GX_TexCoord2f32( s, t );
			GX_Position3f32( v[0], v[1], nv );

			if( reverse )
				v -= VERTEXSIZE;
			else v += VERTEXSIZE;
		}

		if( !useQuads )
			GX_End();
	}

	if( useQuads )
		GX_End();

	GX_SetupFogColorForSurfaces();

	}

static void EmitWaterLightPolys( msurface_t *warp, float soffset, float toffset, qboolean dynamic )
{
	const qboolean useQuads = FBitSet( warp->flags, SURF_DRAWTURB_QUADS );
	float waveHeight = RI.currententity->curstate.scale;

	if( warp->polys->verts[0][2] >= RI.rvp.vieworigin[2] )
		waveHeight = -waveHeight;

	GX_SetupVtxFormat();

	if( useQuads )
		GX_Begin( GX_QUADS, GX_VTXFMT0, 0 );

	for( glpoly2_t *p = warp->polys; p; p = p->next )
	{
		float	*v = p->verts[0];

		if( !useQuads )
			GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, p->numverts );

		for( int i = 0; i < p->numverts; i++, v += VERTEXSIZE )
		{
			if( !dynamic ) GX_TexCoord2f32( v[5], v[6] );
			else GX_TexCoord2f32( v[5] - soffset, v[6] - toffset );

			if( waveHeight )
			{
				float nv = r_turbsin[(int)(gp_cl->time * 160.0f + v[1] + v[0]) & 255] + 8.0f;
				nv = (r_turbsin[(int)(v[0] * 5.0f + gp_cl->time * 171.0f - v[1]) & 255] + 8.0f ) * 0.8f + nv;
				nv = nv * waveHeight + v[2];

				GX_Position3f32( v[0], v[1], nv );
			}
			else GX_Position3f32( v[0], v[1], v[2] );
		}

		if( !useQuads )
			GX_End();
	}

	if( useQuads )
		GX_End();
}

static void DrawGLPolyChain( glpoly2_t *p, float soffset, float toffset, msurface_t *surf )
{
    const qboolean dynamic = soffset != 0.0f || toffset != 0.0f;
    glpoly2_t *q;
    int total_tris = 0;

    if( FBitSet( surf->flags, SURF_DRAWTURB ))
    {
        EmitWaterLightPolys( surf, soffset, toffset, dynamic );
        return;
    }

    // Contar triangulos de TODA la cadena
    for( q = p; q != NULL; q = q->chain )
        if( q->numverts >= 3 )
            total_tris += q->numverts - 2;

    if( total_tris == 0 )
        return;

    GX_SetupVtxFormat();
    GX_Begin( GX_TRIANGLES, GX_VTXFMT0, total_tris * 3 );

    for( q = p; q != NULL; q = q->chain )
    {
        float *base = q->verts[0];
        for( int i = 1; i < q->numverts - 1; i++ )
        {
            float *v0 = base;
            float *v1 = base + i * VERTEXSIZE;
            float *v2 = base + (i + 1) * VERTEXSIZE;

            GX_Position3f32( v0[0], v0[1], v0[2] );
            GX_TexCoord2f32( v0[3], v0[4] );
            if( !dynamic ) GX_TexCoord2f32( v0[5], v0[6] );
            else GX_TexCoord2f32( v0[5] - soffset, v0[6] - toffset );

            GX_Position3f32( v1[0], v1[1], v1[2] );
            GX_TexCoord2f32( v1[3], v1[4] );
            if( !dynamic ) GX_TexCoord2f32( v1[5], v1[6] );
            else GX_TexCoord2f32( v1[5] - soffset, v1[6] - toffset );

            GX_Position3f32( v2[0], v2[1], v2[2] );
            GX_TexCoord2f32( v2[3], v2[4] );
            if( !dynamic ) GX_TexCoord2f32( v2[5], v2[6] );
            else GX_TexCoord2f32( v2[5] - soffset, v2[6] - toffset );
        }
    }

    GX_End();
}

static qboolean R_HasLightmap( void )
{
	if( r_fullbright->value || !WORLDMODEL->lightdata )
		return false;

	if( RI.currententity )
	{
		if( RI.currententity->curstate.effects & EF_FULLBRIGHT )
			return false;

		switch( RI.currententity->curstate.rendermode )
		{
		case kRenderTransTexture:
			return FBitSet( RI.currentmodel->flags, MODEL_LIQUID ) ? true : false;

		case kRenderTransColor:
		case kRenderTransAdd:
		case kRenderGlow:
			return false;
		}
	}

	return true;
}
static void DrawGLPolyBatchChain( msurface_t *head );

static void R_BlendDynamicLightmapsSingle( void )
{
	msurface_t *newsurf, *drawsurf;

	if( !r_dynamic->value || !gx_lms.dynamic_surfaces )
		return;

	LM_InitBlock();
	newsurf = gx_lms.dynamic_surfaces;

	for( msurface_t *surf = gx_lms.dynamic_surfaces; surf != NULL; surf = surf->info->lightmapchain )
	{
		mextrasurf_t *info = surf->info;
		byte *base;
		int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
		int smax = ( info->lightextents[0] / sample_size ) + 1;
		int tmax = ( info->lightextents[1] / sample_size ) + 1;

		if( !LM_AllocBlock( smax, tmax, &surf->info->dlight_s, &surf->info->dlight_t ))
		{
			LM_UploadBlock( true );

			for( drawsurf = newsurf; drawsurf != surf; drawsurf = drawsurf->info->lightmapchain )
			{
				GX_Bind( 0, tr.dlightTexture );
				DrawGLPolyChain( drawsurf->polys,
					( drawsurf->light_s - drawsurf->info->dlight_s ) * ( 1.0f / (float)BLOCK_SIZE ),
					( drawsurf->light_t - drawsurf->info->dlight_t ) * ( 1.0f / (float)BLOCK_SIZE ),
					drawsurf );
			}

			newsurf = drawsurf;
			LM_InitBlock();

			if( !LM_AllocBlock( smax, tmax, &surf->info->dlight_s, &surf->info->dlight_t ))
				gEngfuncs.Host_Error( "AllocBlock: full\n" );
		}

		base = gx_lms.lightmap_buffer;
		base += ( surf->info->dlight_t * BLOCK_SIZE + surf->info->dlight_s ) * 4;
		R_BuildLightMap( surf, base, BLOCK_SIZE * 4, true );
	}

	if( newsurf ) LM_UploadBlock( true );

	for( msurface_t *surf = newsurf; surf != NULL; surf = surf->info->lightmapchain )
	{
		GX_Bind( 0, tr.dlightTexture );
		DrawGLPolyChain( surf->polys,
			( surf->light_s - surf->info->dlight_s ) * ( 1.0f / (float)BLOCK_SIZE ),
			( surf->light_t - surf->info->dlight_t ) * ( 1.0f / (float)BLOCK_SIZE ),
			surf );
	}
}

static void R_BlendLightmaps( void )
{
	msurface_t	*newsurf = NULL;

	if( !R_HasLightmap() || gx_testmode == 5 )
		return;

	if( gx_testmode != 9 )
	{
		/* Pase de lightmap con UNA sola textura (TEXMAP0): resultado = lightmap * framebuffer.
		   La textura difusa ya se dibujo en el primer pase. Evita el TEV de 2 texturas. */
		GX_SetNumTevStages( 1 );
		GX_SetNumTexGens( 1 );
		GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL );
		if( gx_testmode == 11 ) { GX_SetTevKColorSel( GX_TEVSTAGE0, GX_TEV_KCSEL_1_2 ); GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST ); } else GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC );
		GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA );
		GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		if( gl_overbright.value )
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_SRCCLR, GX_LO_CLEAR );
		else
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );

		for( int i = 0; i < MAX_LIGHTMAPS; i++ )
		{
			msurface_t *surf;
			if( !gx_lms.lightmap_surfaces[i] )
				continue;
						{
				static int lmlog;
				if( lmlog < 6 )
				{
					int cnt = 0;
					msurface_t *q;
					lmlog++;
					for( q = gx_lms.lightmap_surfaces[i]; q != NULL; q = q->info->lightmapchain ) cnt++;
					gEngfuncs.Con_Printf( "[LMDRAW] page %d tex=%d surfs=%d\n", i, (int)tr.lightmapTextures[i], cnt );
					q = gx_lms.lightmap_surfaces[i];
					for( int k = 0; k < 3 && q; k++, q = q->info->lightmapchain )
					{
						glpoly2_t *pp = q->polys;
						gEngfuncs.Con_Printf( "[LMDRAW]   s%d ls=%d lt=%d vbo=%d nv=%d", k, (int)q->light_s, (int)q->light_t, (int)q->info->reserved[0], pp ? pp->numverts : -1 );
						if( pp && pp->numverts >= 3 )
							gEngfuncs.Con_Printf( " uv0=%.3f,%.3f uv1=%.3f,%.3f uv2=%.3f,%.3f", pp->verts[0][5], pp->verts[0][6], pp->verts[1][5], pp->verts[1][6], pp->verts[2][5], pp->verts[2][6] );
						gEngfuncs.Con_Printf( "\n" );
					}
				}
			}
			GX_Bind( 0, ( gx_testmode == 10 ) ? tr.grayTexture : tr.lightmapTextures[i] );
			for( surf = gx_lms.lightmap_surfaces[i]; surf != NULL; surf = surf->info->lightmapchain )
				surf->texturechain = surf->info->lightmapchain;
			DrawGLPolyBatchChain_VBO( gx_lms.lightmap_surfaces[i] );
			for( surf = gx_lms.lightmap_surfaces[i]; surf != NULL; surf = surf->info->lightmapchain )
				surf->texturechain = NULL;
		}

		/* lightmaps dinamicos (linterna, dlights, estilos animados) */
		/* R_BlendDynamicLightmapsSingle(); */

		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		GX_SetNumTexGens( 1 );
		GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
		GX_SetNumTevStages( 1 );
		GX_EnableTextureUnit( 0, true );
		return;
	}

	GX_SetupFogColorForSurfacesEx( r_detailtextures.value ? 3 : 2, 1.0f, true );

	GX_SetNumTevStages( 2 );
	GX_SetNumTexGens( 2 );
	/* texgen explicito: TEXCOORD1 debe leer el 2do juego de UVs (lightmap), no el 1ro */
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_SetTexCoordGen( GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY );

	/* Stage 0: TEXMAP0 (difusa) x COLOR0A0 -> intermedio */
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO );
	GX_SetTevAlphaOp( GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );

	/* Stage 1: TEXMAP1 (lightmap) x CPREV -> iluminacion final */
	GX_SetTevOrder( GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL );
	GX_SetTevColorIn( GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO );
	GX_SetTevColorOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	GX_SetTevAlphaIn( GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_APREV, GX_CA_TEXA, GX_CA_ZERO );
	GX_SetTevAlphaOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );

	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );

	if( gl_overbright.value )
	{
		GXColor bright = { 128, 128, 128, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, bright );
	}
	else
	{
		GXColor white = { 255, 255, 255, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, white );
	}

	GX_EnableTextureUnit( 0, true );
	GX_EnableTextureUnit( 1, true );

	if( gx_testmode == 7 ) /* diagnostico: mostrar SOLO el lightmap */
		GX_SetTevColorIn( GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ONE );

	for( int i = 0; i < MAX_LIGHTMAPS; i++ )
	{
		if( gx_lms.lightmap_surfaces[i] )
		{
			msurface_t *batch_head = NULL;
			msurface_t *prev = NULL;
			int batch_texnum = -1;
			msurface_t *surf;

			GX_Bind( 1, tr.lightmapTextures[i] );

			for( surf = gx_lms.lightmap_surfaces[i]; surf != NULL; surf = surf->info->lightmapchain )
			{
				texture_t *tex = R_TextureAnimation( surf );
				int texnum = ( tex && tex->gl_texturenum ) ? tex->gl_texturenum : 0;

				if( texnum != batch_texnum )
				{
					if( batch_head )
					{
						/* cortar la cadena: si no, cada lote dibuja TODO el resto de la pagina */
						prev->texturechain = NULL;
						DrawGLPolyBatchChain_VBO( batch_head );
					}

					if( texnum != 0 )
						GX_Bind( 0, texnum );

					batch_texnum = texnum;
					batch_head = surf;
				}
				surf->texturechain = surf->info->lightmapchain;
				prev = surf;
			}

			if( batch_head )
				DrawGLPolyBatchChain_VBO( batch_head );

			for( surf = gx_lms.lightmap_surfaces[i]; surf != NULL; surf = surf->info->lightmapchain )
				surf->texturechain = NULL;
		}
	}

	if( r_dynamic->value )
	{
		LM_InitBlock();

		newsurf = gx_lms.dynamic_surfaces;

		for( msurface_t *surf = gx_lms.dynamic_surfaces; surf != NULL; surf = surf->info->lightmapchain )
		{
			mextrasurf_t	*info = surf->info;
			byte		*base;

			int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
			int smax = ( info->lightextents[0] / sample_size ) + 1;
			int tmax = ( info->lightextents[1] / sample_size ) + 1;

			if( LM_AllocBlock( smax, tmax, &surf->info->dlight_s, &surf->info->dlight_t ))
			{
				base = gx_lms.lightmap_buffer;
				base += ( surf->info->dlight_t * BLOCK_SIZE + surf->info->dlight_s ) * 4;

				R_BuildLightMap( surf, base, BLOCK_SIZE * 4, true );
			}
			else
			{
				LM_UploadBlock( true );

				msurface_t *drawsurf;
				for( drawsurf = newsurf; drawsurf != surf; drawsurf = drawsurf->info->lightmapchain )
				{
					texture_t *tex = R_TextureAnimation( drawsurf );
					if( tex && tex->gl_texturenum )
						GX_Bind( 0, tex->gl_texturenum );
					GX_Bind( 1, tr.dlightTexture );
					DrawGLPolyChain( drawsurf->polys,
						( drawsurf->light_s - drawsurf->info->dlight_s ) * ( 1.0f / (float)BLOCK_SIZE ),
						( drawsurf->light_t - drawsurf->info->dlight_t ) * ( 1.0f / (float)BLOCK_SIZE ),
						drawsurf );
				}

				newsurf = drawsurf;

				LM_InitBlock();

				if( !LM_AllocBlock( smax, tmax, &surf->info->dlight_s, &surf->info->dlight_t ))
					gEngfuncs.Host_Error( "AllocBlock: full\n" );

				base = gx_lms.lightmap_buffer;
				base += ( surf->info->dlight_t * BLOCK_SIZE + surf->info->dlight_s ) * 4;

				R_BuildLightMap( surf, base, BLOCK_SIZE * 4, true );
			}
		}

		if( newsurf ) LM_UploadBlock( true );

		for( msurface_t *surf = newsurf; surf != NULL; surf = surf->info->lightmapchain )
		{
			texture_t *tex = R_TextureAnimation( surf );
			if( tex && tex->gl_texturenum )
				GX_Bind( 0, tex->gl_texturenum );
			GX_Bind( 1, tr.dlightTexture );
			DrawGLPolyChain( surf->polys,
				( surf->light_s - surf->info->dlight_s ) * ( 1.0f / (float)BLOCK_SIZE ),
				( surf->light_t - surf->info->dlight_t ) * ( 1.0f / (float)BLOCK_SIZE ),
				surf );
		}
	}

	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GXColor white = { 255, 255, 255, 255 };
	GX_SetChanMatColor( GX_COLOR0A0, white );
	GX_EnableTextureUnit( 0, true );
	GX_EnableTextureUnit( 1, false );
	GX_SetNumTevStages( 1 );
	GX_SetNumTexGens( 1 );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

	GX_ResetFogColor();
}

/* Batching de fullbrights: 1 Begin/End por textura completa */
static void DrawGLPolyBatchLuma( mextrasurf_t *head )
{
	int total_tris = 0;
	mextrasurf_t *es;
	glpoly2_t *p;

	if( !head ) return;

	for( es = head; es != NULL; es = es->lumachain )
		for( p = es->surf->polys; p != NULL; p = p->chain )
			if( p->numverts >= 3 )
				total_tris += p->numverts - 2;

	if( total_tris == 0 ) return;

	GX_SetupVtxFormat();
	GX_Begin( GX_TRIANGLES, GX_VTXFMT0, total_tris * 3 );

	for( es = head; es != NULL; es = es->lumachain )
	{
		for( p = es->surf->polys; p != NULL; p = p->chain )
		{
			float *base = p->verts[0];
			for( int i = 1; i < p->numverts - 1; i++ )
			{
				float *v0 = base;
				float *v1 = base + i * VERTEXSIZE;
				float *v2 = base + (i + 1) * VERTEXSIZE;

				GX_Position3f32( v0[0], v0[1], v0[2] );
				GX_TexCoord2f32( v0[3], v0[4] );
				GX_TexCoord2f32( v0[5], v0[6] );

				GX_Position3f32( v1[0], v1[1], v1[2] );
				GX_TexCoord2f32( v1[3], v1[4] );
				GX_TexCoord2f32( v1[5], v1[6] );

				GX_Position3f32( v2[0], v2[1], v2[2] );
				GX_TexCoord2f32( v2[3], v2[4] );
				GX_TexCoord2f32( v2[5], v2[6] );
			}
		}
	}

	GX_End();
}

/* Batching de details: 1 Begin/End por textura de detalle completa */
static void DrawGLPolyBatchDetail( mextrasurf_t *head, float xscale, float yscale )
{
	int total_tris = 0;
	mextrasurf_t *es;
	glpoly2_t *p;

	if( !head ) return;

	for( es = head; es != NULL; es = es->detailchain )
		for( p = es->surf->polys; p != NULL; p = p->chain )
			if( p->numverts >= 3 )
				total_tris += p->numverts - 2;

	if( total_tris == 0 ) return;

	GX_SetupVtxFormat();
	GX_Begin( GX_TRIANGLES, GX_VTXFMT0, total_tris * 3 );

	for( es = head; es != NULL; es = es->detailchain )
	{
		for( p = es->surf->polys; p != NULL; p = p->chain )
		{
			float *base = p->verts[0];
			for( int i = 1; i < p->numverts - 1; i++ )
			{
				float *v0 = base;
				float *v1 = base + i * VERTEXSIZE;
				float *v2 = base + (i + 1) * VERTEXSIZE;

				GX_Position3f32( v0[0], v0[1], v0[2] );
				GX_TexCoord2f32( v0[3] * xscale, v0[4] * yscale );
				GX_TexCoord2f32( v0[5], v0[6] );

				GX_Position3f32( v1[0], v1[1], v1[2] );
				GX_TexCoord2f32( v1[3] * xscale, v1[4] * yscale );
				GX_TexCoord2f32( v1[5], v1[6] );

				GX_Position3f32( v2[0], v2[1], v2[2] );
				GX_TexCoord2f32( v2[3] * xscale, v2[4] * yscale );
				GX_TexCoord2f32( v2[5], v2[6] );
			}
		}
	}

	GX_End();
}

static void R_RenderFullbrights( qboolean allow_vbo )
{
	if( !R_SeparatePassActive( &draw_fullbrights ))
		return;

	R_AllowFog( false );
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );

	if( allow_vbo && gl_polyoffset.value )
		GX_PushPolygonOffset( -1.0f, -gl_polyoffset.value );

	GX_SetupVtxFormat();

	for( int i = draw_fullbrights.first; i <= draw_fullbrights.last; i++ )
	{
		mextrasurf_t *es = fullbright_surfaces[i];
		if( !es )
			continue;

		GX_Bind( XASH_TEXTURE0, i );

		DrawGLPolyBatchLuma( es );

		fullbright_surfaces[i] = NULL;
		es->lumachain = NULL;
	}

	if( allow_vbo && gl_polyoffset.value )
		GX_PopPolygonOffset();

	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );

	R_ResetSeparatePass( &draw_fullbrights );
	R_AllowFog( true );
}

static void R_RenderDetails( int passes )
{
	if( !R_SeparatePassActive( &draw_details ))
		return;

	GX_SetupFogColorForSurfacesEx( passes, passes == 2 ? 0.5f : 1.0f, false );

	GX_SetBlendMode( GX_BM_BLEND, GX_BL_DSTCLR, GX_BL_SRCCLR, GX_LO_CLEAR );
	if( passes == 3 )
		GX_SetZMode( GX_TRUE, GX_EQUAL, GX_FALSE );
	else
	{
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
	}

	GX_SetupVtxFormat();

	for( int i = draw_details.first; i <= draw_details.last; i++ )
	{
		mextrasurf_t *es = detail_surfaces[i];
		if( !es )
			continue;

		GX_Bind( XASH_TEXTURE0, i );

		{
			gl_texture_t *glt = R_GetTexture( es->surf->texinfo->texture->gl_texturenum );
			DrawGLPolyBatchDetail( es, glt->xscale, glt->yscale );
		}

		detail_surfaces[i] = NULL;
		es->detailchain = NULL;
	}

	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );

	R_ResetSeparatePass( &draw_details );

	GX_ResetFogColor();
}

static void R_RenderFullbrightForSurface( msurface_t *fa, texture_t *t )
{
	if( !t->fb_texturenum )
		return;

	fa->info->lumachain = fullbright_surfaces[t->fb_texturenum];
	fullbright_surfaces[t->fb_texturenum] = fa->info;
	R_AddToSeparatePass( &draw_fullbrights, t->fb_texturenum );
}

static void R_RenderDetailsForSurface( msurface_t *fa, texture_t *t )
{
	if( !r_detailtextures.value )
		return;

	if( glState.isFogEnabled )
	{
		if( RI.currententity->curstate.rendermode != kRenderTransTexture )
		{
			int texturenum = t->dt_texturenum ? t->dt_texturenum : tr.grayTexture;

			fa->info->detailchain = detail_surfaces[texturenum];
			detail_surfaces[texturenum] = fa->info;
			R_AddToSeparatePass( &draw_details, texturenum );
		}
	}
	else if( t->dt_texturenum )
	{
		fa->info->detailchain = detail_surfaces[t->dt_texturenum];
		detail_surfaces[t->dt_texturenum] = fa->info;
		R_AddToSeparatePass( &draw_details, t->dt_texturenum );
	}
}

static void R_RenderDecalsForSurface( msurface_t *fa, int cull_type )
{
	if( RI.currententity->curstate.rendermode == kRenderNormal )
	{
		if( tr.num_draw_decals < MAX_DECAL_SURFS && fa->pdecals )
			tr.draw_decals[tr.num_draw_decals++] = fa;
	}
	else
	{
		DrawSurfaceDecals( fa, true, (cull_type == CULL_BACKSIDE));
	}
}

static qboolean R_CheckLightMap( msurface_t *fa )
{
	if( unlikely( !r_dynamic->value ))
		return false;

	if( fa->dlightframe == tr.framecount )
		return true;

	for( int maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++ )
	{
		const int style = fa->styles[maps];

		if( g_lightstylevalue[style] == fa->cached_light[maps] )
			continue;

		byte temp[132*132*4];
		mextrasurf_t *info = fa->info;
		int sample_size = gEngfuncs.Mod_SampleSizeForFace( fa );
		int smax = ( info->lightextents[0] / sample_size ) + 1;
		int tmax = ( info->lightextents[1] / sample_size ) + 1;

		if( smax < 132 && tmax < 132 )
			R_BuildLightMap( fa, temp, smax * 4, true );
		else
		{
			smax = Q_min( smax, 132 );
			tmax = Q_min( tmax, 132 );
			memset( temp, 255, sizeof( temp ));
		}

		R_UpdateSurfaceCachedLight( fa );

		GX_UpdateTextureSub( tr.lightmapTextures[fa->lightmaptexturenum],
			fa->light_s, fa->light_t, smax, tmax, temp );

		return false;
	}

	return false;
}

static void R_RenderLightmapForSurface( msurface_t *fa )
{
	if( !fa->polys || FBitSet( fa->flags, SURF_DRAWTILED ))
		return;

	if( R_CheckLightMap( fa ))
	{
		fa->info->lightmapchain = gx_lms.dynamic_surfaces;
		gx_lms.dynamic_surfaces = fa;
	}
	else
	{
		fa->info->lightmapchain = gx_lms.lightmap_surfaces[fa->lightmaptexturenum];
		gx_lms.lightmap_surfaces[fa->lightmaptexturenum] = fa;
	}
}

static void DrawGLPolyBatchChain( msurface_t *head )
{
    int total_tris = 0;
    msurface_t *s;
    glpoly2_t *p;

    if( !head )
        return;

    // Contar triangulos de TODAS las superficies de la cadena
    for( s = head; s != NULL; s = s->texturechain )
    {
        for( p = s->polys; p != NULL; p = p->chain )
        {
            if( p->numverts >= 3 )
                total_tris += p->numverts - 2;
        }
    }

    if( total_tris == 0 )
        return;

    GX_SetupVtxFormat();
    GX_Begin( GX_TRIANGLES, GX_VTXFMT0, total_tris * 3 );

    // Emitir TODOS los triangulos de TODAS las superficies
    for( s = head; s != NULL; s = s->texturechain )
    {
        for( p = s->polys; p != NULL; p = p->chain )
        {
            float *base = p->verts[0];
            float *v0 = base;
            int i;

            for( i = 1; i < p->numverts - 1; i++ )
            {
                float *v1 = base + i * VERTEXSIZE;
                float *v2 = base + (i + 1) * VERTEXSIZE;

                GX_Position3f32( v0[0], v0[1], v0[2] );
                /* TEXCOORD0 = difusa */
                GX_TexCoord2f32( v0[3], v0[4] );
                /* TEXCOORD1 = lightmap */
                GX_TexCoord2f32( v0[5], v0[6] );

                GX_Position3f32( v1[0], v1[1], v1[2] );
                GX_TexCoord2f32( v1[3], v1[4] );
                GX_TexCoord2f32( v1[5], v1[6] );

                GX_Position3f32( v2[0], v2[1], v2[2] );
                GX_TexCoord2f32( v2[3], v2[4] );
                GX_TexCoord2f32( v2[5], v2[6] );
            }
        }
    }

    GX_End();
}

static void R_RenderBrushPoly( msurface_t *fa, int cull_type )
{
	r_stats.c_world_polys++;

	if( fa->flags & SURF_DRAWSKY )
		return;

	texture_t *t = R_TextureAnimation( fa );

	if( FBitSet( fa->flags, SURF_DRAWTURB ))
	{
		EmitWaterPolys( fa, cull_type == CULL_BACKSIDE, R_UploadRipples( t ));

		if( Mod_HaveLightmappedWater( ))
			R_RenderLightmapForSurface( fa );

		return;
	}
	else
	{
		GX_Bind( XASH_TEXTURE0, t->gl_texturenum );
	}

	R_RenderFullbrightForSurface( fa, t );
	R_RenderDetailsForSurface( fa, t );
	// DrawGLPoly( fa->polys, 0.0f, 0.0f );  // BATCHED
	R_RenderDecalsForSurface( fa, cull_type );
	R_RenderLightmapForSurface( fa );
}

static int gx_dbg_chain_surfs;

static void GX_DiagProbe( const char *tag )
{
	static int frame;
	static int last_logged = -1000;
	const int w = gpGlobals->width, h = gpGlobals->height;
	const int px[5] = { w / 4, w / 2, ( w * 3 ) / 4, w / 5, ( w * 4 ) / 5 };
	const int py[5] = { h / 3, h / 2, h / 3, ( h * 3 ) / 4, ( h * 3 ) / 4 };
	int i;

	if( gx_testmode != 20 )
		return;

	if( tag[0] == 0x42 ) /* B... */
		frame++;

	if( frame - last_logged < 20 && frame > 10 )
		return;
	if( tag[0] == 0x5A ) /* Z... */
		last_logged = frame;

	GX_DrawDone();
	gEngfuncs.Con_Printf( "[PROBE] f=%d %s chainsurfs=%d dyn=%d dl=%d fog=%d/%d poff=%d", frame, tag,
		gx_dbg_chain_surfs, gx_lms.dynamic_surfaces != NULL, (int)tr.dlightframecount,
		(int)glState.isFogEnabled, (int)RI.fogEnabled, (int)glState.num_polyoffsets );
	for( i = 0; i < 5; i++ )
	{
		GXColor c = { 0, 0, 0, 0 };
		u32 z = 0;
		GX_PeekARGB( px[i], py[i], &c );
		GX_PeekZ( px[i], py[i], &z );
		gEngfuncs.Con_Printf( " | %02x%02x%02x z=%06x", c.r, c.g, c.b, (unsigned)z );
	}
	gEngfuncs.Con_Printf( "\n" );
}

static void R_DrawTextureChains( void )
{
	gx_dbg_chain_surfs = 0;
	/* Reset defensivo: el muzzle flash puede dejar ColorUpdate/ZMode/Blend en estado que produce mundo negro */
	GX_SetColorUpdate( GX_TRUE );
	GX_SetAlphaUpdate( GX_TRUE );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetNumTexGens( 1 );
	GX_SetTexCoordGen( GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY );
	GX_SetNumTevStages( 1 );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetChanVtxColor( false );
	GXColor white = { 255, 255, 255, 255 };
	GX_SetChanMatColor( GX_COLOR0A0, white );
	R_LoadIdentity();

	GX_SetupFogColorForSurfaces();

	RI.currententity = CL_GetEntityByIndex( 0 );
	RI.currentmodel = RI.currententity->model;

	for( msurface_t *s = skychain; s != NULL; s = s->texturechain )
		R_AddSkyBoxSurface( s );

	if( skychain )
		R_DrawClouds();

	/* NO anular skychain aqui: R_DrawWorld lo necesita para dibujar el skybox */

	R_DrawVBO( !r_fullbright->value && !!WORLDMODEL->lightdata, true );

	for( int i = 0; i < WORLDMODEL->numtextures; i++ )
	{
		texture_t *t = WORLDMODEL->textures[i];
		if( !t ) continue;

		msurface_t *s = t->texturechain;

		if( !s || ( i == tr.skytexturenum ))
			continue;

		if(( s->flags & SURF_DRAWTURB ) && gp_movevars->wateralpha < 1.0f )
		{
			R_AddToSeparatePass( &draw_wateralpha, i );
			continue;
		}

		if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) && FBitSet( s->flags, SURF_TRANSPARENT ))
		{
			R_AddToSeparatePass( &draw_alpha_surfaces, i );
			continue;
		}

			{
				msurface_t *batch_head = s;
				for( ; s != NULL; s = s->texturechain )
				{
					R_RenderBrushPoly( s, CULL_VISIBLE );
					gx_dbg_chain_surfs++;
				}

				/* el pase de lightmaps ya dibuja textura x lightmap: no dibujar dos veces
				   las superficies con lightmap (solo las que no pasan por ese pase) */
				if( gx_testmode == 9 && R_HasLightmap( ))
				{
					msurface_t *nh = NULL, *nt = NULL, *q, *nx;
					for( q = batch_head; q != NULL; q = nx )
					{
						nx = q->texturechain;
						if( !q->polys || FBitSet( q->flags, SURF_DRAWTILED | SURF_DRAWTURB ))
						{
							q->texturechain = NULL;
							if( nt ) nt->texturechain = q; else nh = q;
							nt = q;
						}
					}
					batch_head = nh;
				}

				GX_Bind( XASH_TEXTURE0, ( gx_testmode == 6 ) ? tr.whiteTexture : t->gl_texturenum );
				if( gx_world_dbg > 0 )
				{
					gl_texture_t *gt = R_GetTexture( t->gl_texturenum );
					int nf = 0;
					for( msurface_t *q = batch_head; q; q = q->texturechain ) nf++;
					gEngfuncs.Con_Printf( "[WCHAIN] #%d %s tex=%d %dx%d fmt=%d mips=%d surfs=%d ...\n", i, t->name, t->gl_texturenum,
						gt ? gt->width : 0, gt ? gt->height : 0, gt ? (int)gt->format : -1, gt ? (int)gt->numMips : -1, nf );
				}
				if( batch_head )
					DrawGLPolyBatchChain_VBO( batch_head );
				if( gx_world_dbg > 0 )
				{
					GX_DrawDone();
					gEngfuncs.Con_Printf( "[WCHAIN]   ok\n" );
				}
			}
		t->texturechain = NULL;
	}
}

void R_DrawAlphaTextureChains( void )
{
	if( !R_SeparatePassActive( &draw_alpha_surfaces ))
		return;

	memset( gx_lms.lightmap_surfaces, 0, sizeof( gx_lms.lightmap_surfaces ));
	gx_lms.dynamic_surfaces = NULL;

	GXColor white = { 255, 255, 255, 255 };
	GX_SetChanMatColor( GX_COLOR0A0, white );
	R_LoadIdentity();

	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetAlphaCompare( GX_GREATER, 64, GX_AOP_AND, GX_ALWAYS, 0 );

	GX_SetupFogColorForSurfaces();

	RI.currententity = CL_GetEntityByIndex( 0 );
	RI.currentmodel = RI.currententity->model;
	RI.currententity->curstate.rendermode = kRenderTransAlpha;

	for( int i = draw_alpha_surfaces.first; i <= draw_alpha_surfaces.last; i++ )
	{
		texture_t *t = WORLDMODEL->textures[i];
		if( !t )
			continue;

		msurface_t *s = t->texturechain;

		if( !s || !FBitSet( s->flags, SURF_TRANSPARENT ))
			continue;

		for( ; s != NULL; s = s->texturechain )
			R_RenderBrushPoly( s, CULL_VISIBLE );
		t->texturechain = NULL;
	}

	R_ResetSeparatePass( &draw_alpha_surfaces );

	GX_ResetFogColor();
	R_BlendLightmaps();
	RI.currententity->curstate.rendermode = kRenderNormal;
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
}

void R_DrawWaterSurfaces( void )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) || FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
		return;

	if( !R_SeparatePassActive( &draw_wateralpha ))
		return;

	RI.currententity = CL_GetEntityByIndex( 0 );
	RI.currentmodel = RI.currententity->model;

	R_LoadIdentity();

	GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );

	u8 alpha = (u8)(gp_movevars->wateralpha * 255.0f);
	GXColor waterColor = { 255, 255, 255, alpha };
	GX_SetChanMatColor( GX_COLOR0A0, waterColor );

	for( int i = draw_wateralpha.first; i <= draw_wateralpha.last; i++ )
	{
		texture_t *t = WORLDMODEL->textures[i];
		if( !t ) continue;

		msurface_t *s = t->texturechain;
		if( !s ) continue;

		if( !FBitSet( s->flags, SURF_DRAWTURB ))
			continue;

		for( ; s; s = s->texturechain )
		{
			EmitWaterPolys( s, false, R_UploadRipples( t ));

			if( Mod_HaveLightmappedWater( ))
				R_RenderLightmapForSurface( s );
		}

		t->texturechain = NULL;
	}

	GX_ResetFogColor();
	R_BlendLightmaps();

	R_ResetSeparatePass( &draw_wateralpha );

	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GXColor white = { 255, 255, 255, 255 };
	GX_SetChanMatColor( GX_COLOR0A0, white );
}

static int R_SurfaceCompare( const void *a, const void *b )
{
	vec3_t		org1, org2;

	msurface_t *surf1 = (msurface_t *)((sortedface_t *)a)->surf;
	msurface_t *surf2 = (msurface_t *)((sortedface_t *)b)->surf;

	VectorAdd( RI.currententity->origin, surf1->info->origin, org1 );
	VectorAdd( RI.currententity->origin, surf2->info->origin, org2 );

	float len1 = DotProduct( org1, RI.vforward ) - RI.viewplanedist;
	float len2 = DotProduct( org2, RI.vforward ) - RI.viewplanedist;

	if( len1 > len2 )
		return -1;
	if( len1 < len2 )
		return 1;

	return 0;
}

static void R_SetRenderMode( cl_entity_t *e )
{
	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
	default:
		GXColor white = { 255, 255, 255, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, white );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		break;
	case kRenderTransColor:
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		{
			GXColor c = { e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, e->curstate.renderamt };
			GX_SetChanMatColor( GX_COLOR0A0, c );
		}
		break;
	case kRenderTransAdd:
		{
			u8 a = (u8)(tr.blend * 255.0f);
			GXColor c = { 255, 255, 255, a };
			GX_SetChanMatColor( GX_COLOR0A0, c );
		}
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		break;
	case kRenderTransAlpha:
		GX_SetAlphaCompare( GX_GREATER, 64, GX_AOP_AND, GX_ALWAYS, 0 );
		if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
		{
			u8 a = (u8)(tr.blend * 255.0f);
			GXColor c = { 255, 255, 255, a };
			GX_SetChanMatColor( GX_COLOR0A0, c );
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		}
		else
		{
			GXColor white = { 255, 255, 255, 255 };
			GX_SetChanMatColor( GX_COLOR0A0, white );
			GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		}
		break;
	case kRenderTransTexture:
		{
			u8 a = (u8)(tr.blend * 255.0f);
			GXColor c = { 255, 255, 255, a };
			GX_SetChanMatColor( GX_COLOR0A0, c );
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		}
		break;
	case kRenderGlow:
		{
			u8 a = (u8)(tr.blend * 255.0f);
			GXColor c = { 255, 255, 255, a };
			GX_SetChanMatColor( GX_COLOR0A0, c );
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR );
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		}
		break;
	}
}

static int R_SortBrushModelSurfaces( cl_entity_t *e, model_t *clmodel, vec3_t mins )
{
	qboolean quake_compatible = FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) ? true : false;
	int num_sorted = 0;

	for( int i = 0; i < clmodel->nummodelsurfaces; i++ )
	{
		msurface_t *psurf = &clmodel->surfaces[clmodel->firstmodelsurface + i];

		if( FBitSet( psurf->flags, SURF_DRAWTURB ) && !quake_compatible )
		{
			if( psurf->plane->type != PLANE_Z && !FBitSet( e->curstate.effects, EF_WATERSIDES ))
				continue;

			if( mins[2] + 1.0f >= psurf->plane->dist )
				continue;
		}

		int cull_type = R_CullSurface( psurf, &RI.frustum, RI.frustum.clipFlags );

		if( cull_type >= CULL_FRUSTUM )
			continue;

		if( cull_type == CULL_BACKSIDE )
		{
			if( FBitSet( psurf->flags, SURF_DRAWTURB ))
			{
				if( Mod_HaveLightmappedWater( ))
					continue;
			}
			else
			{
				if( !( psurf->pdecals && e->curstate.rendermode == kRenderTransTexture ))
					continue;
			}
		}

		if( num_sorted < gpGlobals->max_surfaces )
		{
			gpGlobals->draw_surfaces[num_sorted].surf = psurf;
			gpGlobals->draw_surfaces[num_sorted].cull = cull_type;
			num_sorted++;
		}
	}

	if( !FBitSet( clmodel->flags, MODEL_LIQUID ) && e->curstate.rendermode == kRenderTransTexture && !gl_nosort.value )
		qsort( gpGlobals->draw_surfaces, num_sorted, sizeof( sortedface_t ), R_SurfaceCompare );

	return num_sorted;
}

void R_DrawBrushModel( cl_entity_t *e )
{
	vec3_t mins, maxs;
	qboolean rotated;
	qboolean allow_vbo = R_HasEnabledVBO();

	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	model_t *clmodel = e->model;

	if( clmodel->surfaces != WORLDMODEL->surfaces )
		allow_vbo = false;

	if( !VectorIsNull( e->angles ))
	{
		for( int i = 0; i < 3; i++ )
		{
			mins[i] = e->origin[i] - clmodel->radius;
			maxs[i] = e->origin[i] + clmodel->radius;
		}
		rotated = true;
	}
	else
	{
		VectorAdd( e->origin, clmodel->mins, mins );
		VectorAdd( e->origin, clmodel->maxs, maxs );
		rotated = false;
	}

	if( R_CullBox( mins, maxs ))
		return;

	memset( gx_lms.lightmap_surfaces, 0, sizeof( gx_lms.lightmap_surfaces ));
	int old_rendermode = e->curstate.rendermode;
	gx_lms.dynamic_surfaces = NULL;

	if( rotated )
	{
		R_RotateForEntity( e );

		Matrix4x4_VectorITransform( RI.objectMatrix, RI.cullorigin, tr.modelorg );
	}
	else
	{
		R_TranslateForEntity( e );

		VectorSubtract( RI.cullorigin, e->origin, tr.modelorg );
	}

	if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ) && FBitSet( clmodel->flags, MODEL_TRANSPARENT ))
		e->curstate.rendermode = kRenderTransAlpha;

	e->visframe = tr.realframecount;

	R_PushDlightsForBmodel( clmodel, tr.dlightframecount, RI.objectMatrix );

	R_SetRenderMode( e );
	if( e->curstate.rendermode == kRenderTransAdd )
	{
		R_AllowFog( false );
		allow_vbo = false;
	}

	if( e->curstate.rendermode == kRenderTransColor || e->curstate.rendermode == kRenderTransTexture )
		allow_vbo = false;

	if( !allow_vbo )
		GX_SetupFogColorForSurfaces();

	int num_sorted = R_SortBrushModelSurfaces( e, clmodel, mins );

	qboolean drawlightmap = R_HasLightmap();

	if( gl_polyoffset_bmodels.value )
		GX_PushPolygonOffset( -0.5f, -gl_polyoffset_bmodels.value );

	for( int i = 0; i < num_sorted; i++ )
	{
		if( !allow_vbo || !R_AddSurfToVBO( gpGlobals->draw_surfaces[i].surf, drawlightmap ))
			R_RenderBrushPoly( gpGlobals->draw_surfaces[i].surf, gpGlobals->draw_surfaces[i].cull );
	}

	R_DrawVBO( drawlightmap, true );

	DrawDecalsBatch();

	GX_ResetFogColor();
	R_BlendLightmaps();
	R_RenderFullbrights( allow_vbo );
	R_RenderDetails( allow_vbo ? 2 : 3 );

	if( gl_polyoffset_bmodels.value )
		GX_PopPolygonOffset();

	R_DrawTriangleOutlines();

	if( e->curstate.rendermode == kRenderTransAdd )
		R_AllowFog( true );

	e->curstate.rendermode = old_rendermode;
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );

            if( 0 )  /* r_showfull neutralizado */
	{
		GX_PushPolygonOffset( 1.0f, 2.0f );
		gEngfuncs.R_DrawModelHull( clmodel );
		GX_PopPolygonOffset();
	}

	R_LoadIdentity();
}

qboolean R_HasGeneratedVBO( void )
{
	return false;
}

void R_EnableVBO( qboolean enable )
{
}

qboolean R_HasEnabledVBO( void )
{
	return false;
}

void R_GenerateVBO( void )
{
}

void R_ClearVBO( void )
{
}

void R_AddDecalVBO( decal_t *pdecal, msurface_t *surf )
{
}

static qboolean R_AddSurfToVBO( msurface_t *surf, qboolean buildlightmap )
{
	return false;
}

static void R_DrawVBO( qboolean drawlightmaps, qboolean drawtextures )
{
}

static void R_RecursiveWorldNode( mnode_t *node, uint clipflags )
{
loc0:
	if( node->contents == CONTENTS_SOLID )
		return;

	if( node->visframe != tr.visframecount )
		return;

	if( clipflags && !r_nocull.value )
	{
		for( int i = 0; i < 6; i++ )
		{
			const mplane_t	*p = &RI.frustum.planes[i];

			if( !FBitSet( clipflags, BIT( i )))
				continue;

			int clipped = BOX_ON_PLANE_SIDE( node->minmaxs, node->minmaxs + 3, p );
			if( clipped == 2 ) return;
			if( clipped == 1 ) ClearBits( clipflags, BIT( i ));
		}
	}

	if( node->contents < 0 )
	{
		mleaf_t *pleaf = (mleaf_t *)node;
		msurface_t **mark = pleaf->firstmarksurface;

		for( int i = 0; i < pleaf->nummarksurfaces; i++ )
			mark[i]->visframe = tr.framecount;

		if( pleaf->efrags )
			gEngfuncs.R_StoreEfrags( &pleaf->efrags, tr.realframecount );

		r_stats.c_world_leafs++;
		return;
	}

	float dot = PlaneDiff( tr.modelorg, node->plane );
	int side = (dot >= 0.0f) ? 0 : 1;

	R_RecursiveWorldNode( node_child( node, side, WORLDMODEL ), clipflags );

	int firstsurface = node_firstsurface( node, WORLDMODEL );
	int numsurfaces = node_numsurfaces( node, WORLDMODEL );

	for( int i = firstsurface; i < firstsurface + numsurfaces; i++ )
	{
		msurface_t *surf = &WORLDMODEL->surfaces[i];

		if( R_CullSurface( surf, &RI.frustum, clipflags ))
			continue;

		if( surf->flags & SURF_DRAWSKY )
		{
			surf->texturechain = skychain;
			skychain = surf;
		}
		else if( !R_AddSurfToVBO( surf, true ) )
		{
			surf->texturechain = surf->texinfo->texture->texturechain;
			surf->texinfo->texture->texturechain = surf;
		}
	}

	node = node_child( node, !side, WORLDMODEL );
	goto loc0;
}

static qboolean R_CullNodeTopView( mnode_t *node )
{
	vec2_t	delta, size;
	vec3_t	center, half;

	VectorAverage( node->minmaxs, node->minmaxs + 3, center );
	VectorSubtract( node->minmaxs + 3, center, half );

	Vector2Subtract( center, world_orthocenter, delta );
	Vector2Add( half, world_orthohalf, size );

	return ( fabs( delta[0] ) > size[0] ) || ( fabs( delta[1] ) > size[1] );
}

static void R_DrawTopViewLeaf( mleaf_t *pleaf, uint clipflags )
{
	msurface_t	**mark;
	int		i;

	for( i = 0, mark = pleaf->firstmarksurface; i < pleaf->nummarksurfaces; i++, mark++ )
	{
		msurface_t *surf = *mark;

		if( surf->visframe == tr.framecount )
			continue;

		surf->visframe = tr.framecount;

		if( R_CullSurface( surf, &RI.frustum, clipflags ))
			continue;

		if(!( surf->flags & SURF_DRAWSKY ))
		{
			surf->texturechain = surf->texinfo->texture->texturechain;
			surf->texinfo->texture->texturechain = surf;
		}
	}

	if( pleaf->efrags )
		gEngfuncs.R_StoreEfrags( &pleaf->efrags, tr.realframecount );

	r_stats.c_world_leafs++;
}

static void R_DrawWorldTopView( mnode_t *node, uint clipflags )
{
	msurface_t	*surf;
	int		c;

	do
	{
		if( node->contents == CONTENTS_SOLID )
			return;

		if( node->visframe != tr.visframecount )
			return;

		if( clipflags && !r_nocull.value )
		{
			for( int i = 0; i < 6; i++ )
			{
				const mplane_t	*p = &RI.frustum.planes[i];

				if( !FBitSet( clipflags, BIT( i )))
					continue;

				int clipped = BOX_ON_PLANE_SIDE( node->minmaxs, node->minmaxs + 3, p );
				if( clipped == 2 ) return;
				if( clipped == 1 ) ClearBits( clipflags, BIT( i ));
			}
		}

		if( R_CullNodeTopView( node ))
			return;

		if( node->contents < 0 )
		{
			R_DrawTopViewLeaf( (mleaf_t *)node, clipflags );
			return;
		}

		int numsurfaces = node_numsurfaces( node, WORLDMODEL );
		int firstsurface = node_firstsurface( node, WORLDMODEL );

		for( c = numsurfaces, surf = WORLDMODEL->surfaces + firstsurface; c; c--, surf++ )
		{
			if( surf->visframe == tr.framecount )
				continue;

			surf->visframe = tr.framecount;

			if( R_CullSurface( surf, &RI.frustum, clipflags ))
				continue;

			if(!( surf->flags & SURF_DRAWSKY ))
			{
				surf->texturechain = surf->texinfo->texture->texturechain;
				surf->texinfo->texture->texturechain = surf;
			}
		}

		R_DrawWorldTopView( node_child( node, 0, WORLDMODEL ), clipflags );
		node = node_child( node, 1, WORLDMODEL );
	} while( node );
}

void R_DrawWorld( void )
{
	RI.currententity = CL_GetEntityByIndex( 0 );
	if( !RI.currententity )
		return;

	RI.currentmodel = RI.currententity->model;
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) || FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
		return;

	VectorCopy( RI.cullorigin, tr.modelorg );
	memset( gx_lms.lightmap_surfaces, 0, sizeof( gx_lms.lightmap_surfaces ));
	memset( fullbright_surfaces, 0, sizeof( fullbright_surfaces ));
	memset( detail_surfaces, 0, sizeof( detail_surfaces ));

	gx_lms.dynamic_surfaces = NULL;
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	tr.blend = 1.0f;
	GX_SetupTexturedPipeline();
	{ GXColor w = { 255, 255, 255, 255 }; GX_SetChanMatColor( GX_COLOR0A0, w ); }

	R_ClearSkyBox();

	glState.faceCull = XASH_CULL_FRONT; /* estado de culling por CPU del mundo */

	/* endurecimiento: no confiar en el estado que dejo el frame anterior */
	{
		int u;
		for( u = 0; u < MAX_TEXTURE_UNITS; u++ )
		{
			glState.currentTexturesIndex[u] = 0;
			glState.currentTextures[u] = NULL;
		}
		GX_SetScissor( 0, 0, gpGlobals->width, gpGlobals->height );
		GX_SetClipMode( GX_CLIP_ENABLE );
		GX_SetCoPlanar( GX_DISABLE );
		GX_SetColorUpdate( GX_TRUE );
		GX_SetAlphaUpdate( GX_TRUE );
		GX_SetZCompLoc( GX_TRUE );
		GX_SetCurrentMtx( GX_PNMTX0 );
		if( !glState.isFogEnabled )
		{
			GXColor nofog = { 0, 0, 0, 0 };
			GX_SetFog( GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, nofog );
		}
	}
	GX_DiagProbe( "B-start" );

	double start = gEngfuncs.pfnTime();
	if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
		R_DrawWorldTopView( WORLDMODEL->nodes, RI.frustum.clipFlags );
	else R_RecursiveWorldNode( WORLDMODEL->nodes, RI.frustum.clipFlags );
	double end = gEngfuncs.pfnTime();

	r_stats.t_world_node = end - start;

	start = gEngfuncs.pfnTime();

	if( gx_world_dbg > 0 ) gEngfuncs.Con_Printf( "[WORLD] chains start\n" );
	R_DrawTextureChains();
	GX_DiagProbe( "after-pass1" );
	if( gx_world_dbg > 0 ) { GX_DrawDone(); gEngfuncs.Con_Printf( "[WORLD] chains done\n" ); }

	if( !ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
	{
		if( gx_testmode != 8 ) DrawDecalsBatch();
		GX_ResetFogColor();
		if( gx_world_dbg > 0 ) { GX_DrawDone(); gEngfuncs.Con_Printf( "[WORLD] decals done\n" ); }
		R_BlendLightmaps();
		GX_DiagProbe( "after-lightmaps" );
		if( gx_world_dbg > 0 ) { GX_DrawDone(); gEngfuncs.Con_Printf( "[WORLD] lightmaps done\n" ); }
		if( gx_testmode != 8 )
		{
			R_RenderFullbrights( R_HasEnabledVBO( ));
			R_RenderDetails( R_HasEnabledVBO() ? 2 : 3 );
		}
		R_DrawTriangleOutlines();

		if( skychain && gx_testmode != 8 )
			R_DrawSkyBox();
		GX_DiagProbe( "Z-after-sky" );
		if( gx_world_dbg > 0 ) { GX_DrawDone(); gEngfuncs.Con_Printf( "[WORLD] sky/extras done\n" ); }
	}

	end = gEngfuncs.pfnTime();

	r_stats.t_world_draw = end - start;

	if( gx_world_dbg > 0 )
	{
		int pages = 0;
		gx_world_dbg--;
		for( int i = 0; i < MAX_LIGHTMAPS; i++ )
			if( tr.lightmapTextures[i] ) pages++;
		gEngfuncs.Con_Printf( "[WORLD] polys=%d leafs=%d lightmap pages=%d haslightdata=%d t_node=%.1fms t_draw=%.1fms\n",
			(int)r_stats.c_world_polys, (int)r_stats.c_world_leafs, pages,
			WORLDMODEL->lightdata ? 1 : 0, r_stats.t_world_node * 1000.0, r_stats.t_world_draw * 1000.0 );
	}
	tr.num_draw_decals = 0;
	skychain = NULL;

	gEngfuncs.R_DrawWorldHull();
}

void R_MarkLeaves( void )
{
	qboolean	novis = false;
	qboolean	force = false;
	mleaf_t	*leaf = NULL;

	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	if( FBitSet( r_novis.flags, FCVAR_CHANGED ) || tr.fResetVis )
	{
		ClearBits( r_novis.flags, FCVAR_CHANGED );
		tr.fResetVis = false;
		RI.viewleaf = NULL;
	}

	vec3_t test = Vec3( RI.rvp.vieworigin );

	if( RI.viewleaf != NULL )
	{
		if( RI.viewleaf->contents == CONTENTS_EMPTY )
			VectorSet( test, RI.rvp.vieworigin[0], RI.rvp.vieworigin[1], RI.rvp.vieworigin[2] - 16.0f );
		else
			VectorSet( test, RI.rvp.vieworigin[0], RI.rvp.vieworigin[1], RI.rvp.vieworigin[2] + 16.0f );

		leaf = gEngfuncs.Mod_PointInLeaf( test, WORLDMODEL->nodes, WORLDMODEL );

		if(( leaf->contents != CONTENTS_SOLID ) && ( RI.viewleaf != leaf ))
			force = true;
	}

	if( RI.viewleaf == RI.oldviewleaf && RI.viewleaf != NULL && !force )
		return;

	if( r_lockpvs.value ) return;

	RI.oldviewleaf = RI.viewleaf;
	tr.visframecount++;

	if( r_novis.value || FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ) || !RI.viewleaf || !WORLDMODEL->visdata )
		novis = true;

	gEngfuncs.R_FatPVS( RI.rvp.vieworigin, r_pvs_radius->value, RI.visbytes, false, novis );
	if( force && !novis )
		gEngfuncs.R_FatPVS( test, r_pvs_radius->value, RI.visbytes, true, novis );

	for( int i = 0; i < WORLDMODEL->numleafs; i++ )
	{
		if( CHECKVISBIT( RI.visbytes, i ))
		{
			mnode_t *node = (mnode_t *)&WORLDMODEL->leafs[i+1];
			do
			{
				if( node->visframe == tr.visframecount )
					break;
				node->visframe = tr.visframecount;
				node = node->parent;
			} while( node );
		}
	}
}

static void GX_CreateSurfaceLightmap( msurface_t *surf, model_t *loadmodel )
{
	mextrasurf_t	*info = surf->info;

	if( !loadmodel->lightdata )
		return;

	if( FBitSet( surf->flags, SURF_DRAWTILED ))
		return;

	int sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
	int smax = ( info->lightextents[0] / sample_size ) + 1;
	int tmax = ( info->lightextents[1] / sample_size ) + 1;

	if( !LM_AllocBlock( smax, tmax, &surf->light_s, &surf->light_t ))
	{
		LM_UploadBlock( false );
		LM_InitBlock();

		if( !LM_AllocBlock( smax, tmax, &surf->light_s, &surf->light_t ))
			gEngfuncs.Host_Error( "%s: full\n", __func__ );
	}

	surf->lightmaptexturenum = gx_lms.current_lightmap_texture;

	byte *base = gx_lms.lightmap_buffer;
	base += ( surf->light_t * BLOCK_SIZE + surf->light_s ) * 4;

	R_UpdateSurfaceCachedLight( surf );
	R_BuildLightMap( surf, base, BLOCK_SIZE * 4, false );
}

void GX_RebuildLightmaps( void )
{
	if( !ENGINE_GET_PARM( PARM_CLIENT_ACTIVE ) )
		return;

	for( int i = 0; i < MAX_LIGHTMAPS; i++ )
	{
		if( !tr.lightmapTextures[i] ) break;
		GX_FreeTexture( tr.lightmapTextures[i] );
	}

	memset( tr.lightmapTextures, 0, sizeof( tr.lightmapTextures ));
	gx_lms.current_lightmap_texture = 0;

	CL_RunLightStyles((lightstyle_t *)ENGINE_GET_PARM( PARM_GET_LIGHTSTYLES_PTR ));

	LM_InitBlock();

	for( int i = 0; i < gp_cl->nummodels; i++ )
	{
		model_t *m = CL_ModelHandle( i + 1 );
		if( m == NULL )
			continue;

		if( m->name[0] == '*' || m->type != mod_brush )
			continue;

		for( int j = 0; j < m->numsurfaces; j++ )
			GX_CreateSurfaceLightmap( m->surfaces + j, m );
	}
	LM_UploadBlock( false );

	if( gEngfuncs.drawFuncs->GL_BuildLightmaps )
	{
		gEngfuncs.drawFuncs->GL_BuildLightmaps( );
	}
}

void GX_BuildLightmaps( void )
{
	int	nColinElim = 0;

	for( int i = 0; i < MAX_LIGHTMAPS; i++ )
	{
		if( !tr.lightmapTextures[i] ) break;
		GX_FreeTexture( tr.lightmapTextures[i] );
	}

	memset( tr.lightmapTextures, 0, sizeof( tr.lightmapTextures ));
	memset( &RI, 0, sizeof( RI ));

	if( FBitSet( gp_host->features, ENGINE_LARGE_LIGHTMAPS ) || tr.world->version == QBSP2_VERSION || r_large_lightmaps.value )
		tr.block_size = BLOCK_SIZE_MAX;
	else tr.block_size = BLOCK_SIZE_DEFAULT;

	skychain = NULL;

	tr.framecount = tr.visframecount = 1;
	gx_lms.current_lightmap_texture = 0;
	tr.modelviewIdentity = false;
	tr.realframecount = 1;

	R_InitDlightTexture();

	CL_RunLightStyles((lightstyle_t *)ENGINE_GET_PARM( PARM_GET_LIGHTSTYLES_PTR ));

	LM_InitBlock();

	for( int i = 0; i < gp_cl->nummodels; i++ )
	{
		model_t *m = CL_ModelHandle( i + 1 );
		if( m == NULL )
			continue;

		if( m->name[0] == '*' || m->type != mod_brush )
			continue;

		for( int j = 0; j < m->numsurfaces; j++ )
		{
			m->surfaces[j].pdecals = NULL;
			m->surfaces[j].visframe = 0;

			GX_CreateSurfaceLightmap( m->surfaces + j, m );

			if( m->surfaces[j].flags & SURF_DRAWTURB )
			{
				GX_BuildLightmapWater( m, m->surfaces + j );
				continue;
			}

			nColinElim += GX_BuildPolygonFromSurface( m, m->surfaces + j );
		}

		for( int j = 0; j < m->numleafs; j++ )
			m->leafs[j+1].visframe = 0;
		for( int j = 0; j < m->numnodes; j++ )
			m->nodes[j].visframe = 0;
	}

	LM_UploadBlock( false );

	if( gEngfuncs.drawFuncs->GL_BuildLightmaps )
	{
		gEngfuncs.drawFuncs->GL_BuildLightmaps( );
	}
	GX_VBO_BuildWorld();
}
