/*
gx_alias.c - alias model renderer (Wii GX native port)
Copyright (C) 2017 Uncle Mike
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
#include "const.h"
#include "r_studioint.h"
#include "triangleapi.h"
#include "alias.h"
#include "pm_local.h"

typedef struct
{
	double		time;
	double		frametime;
	int		framecount;
	qboolean		interpolate;

	float		ambientlight;
	float		shadelight;
	vec3_t		lightvec;
	vec3_t		lightvec_local;
	vec3_t		lightspot;
	vec3_t		lightcolor;
	int		oldpose;
	int		newpose;
	float		lerpfrac;
} alias_draw_state_t;

static alias_draw_state_t	g_alias;

static qboolean	m_fDoRemap;
static aliashdr_t	*m_pAliasHeader;
static dtriangle_t	g_triangles[MAXALIASTRIS];
static stvert_t	g_stverts[MAXALIASVERTS];
static int	g_used[8192];

static int	g_commands[8192];
static int	g_numcommands;

static int	g_vertexorder[8192];
static int	g_numorder;

static int	g_stripverts[128];
static int	g_striptris[128];
static int	g_stripcount;

static void GX_SetupVtxFormatAlias( qboolean useColor, qboolean useTex )
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS,  GX_DIRECT );
	GX_SetVtxDesc( GX_VA_CLR0, useColor ? GX_DIRECT : GX_NONE );
	GX_SetVtxDesc( GX_VA_TEX0, useTex   ? GX_DIRECT : GX_NONE );

	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ, GX_F32, 0 );
	if( useColor )
		GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );
	if( useTex )
		GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0 );
}

void R_AliasInit( void )
{
	g_alias.interpolate = true;
	m_fDoRemap = false;
}

static int StripLength( int starttri, int startv )
{
	g_used[starttri] = 2;

	dtriangle_t *last = &g_triangles[starttri];

	g_stripverts[0] = last->vertindex[(startv+0) % 3];
	g_stripverts[1] = last->vertindex[(startv+1) % 3];
	g_stripverts[2] = last->vertindex[(startv+2) % 3];

	g_striptris[0] = starttri;
	g_stripcount = 1;

	int m1 = last->vertindex[(startv+2)%3];
	int m2 = last->vertindex[(startv+1)%3];
	int j;
	dtriangle_t *check;
nexttri:
	for( j = starttri + 1, check = &g_triangles[starttri + 1]; j < m_pAliasHeader->numtris; j++, check++ )
	{
		if( check->facesfront != last->facesfront )
			continue;

		for( int k = 0; k < 3; k++ )
		{
			if( check->vertindex[k] != m1 )
				continue;
			if( check->vertindex[(k+1) % 3] != m2 )
				continue;

			if( g_used[j] ) goto done;

			if( g_stripcount & 1 )
				m2 = check->vertindex[(k+2) % 3];
			else m1 = check->vertindex[(k+2) % 3];

			g_stripverts[g_stripcount+2] = check->vertindex[(k+2) % 3];
			g_striptris[g_stripcount] = j;
			g_stripcount++;

			g_used[j] = 2;
			goto nexttri;
		}
	}
done:
	for( j = starttri + 1; j < m_pAliasHeader->numtris; j++ )
	{
		if( g_used[j] == 2 )
			g_used[j] = 0;
	}

	return g_stripcount;
}

static int FanLength( int starttri, int startv )
{
	g_used[starttri] = 2;

	dtriangle_t *last = &g_triangles[starttri];

	g_stripverts[0] = last->vertindex[(startv+0) % 3];
	g_stripverts[1] = last->vertindex[(startv+1) % 3];
	g_stripverts[2] = last->vertindex[(startv+2) % 3];

	g_striptris[0] = starttri;
	g_stripcount = 1;

	int m1 = last->vertindex[(startv+0) % 3];
	int m2 = last->vertindex[(startv+2) % 3];
	int j;
	dtriangle_t *check;

nexttri:
	for( j = starttri + 1, check = &g_triangles[starttri + 1]; j < m_pAliasHeader->numtris; j++, check++ )
	{
		if( check->facesfront != last->facesfront )
			continue;

		for( int k = 0; k < 3; k++ )
		{
			if( check->vertindex[k] != m1 )
				continue;
			if( check->vertindex[(k+1) % 3] != m2 )
				continue;

			if( g_used[j] ) goto done;

			m2 = check->vertindex[(k+2) % 3];

			g_stripverts[g_stripcount + 2] = m2;
			g_striptris[g_stripcount] = j;
			g_stripcount++;

			g_used[j] = 2;
			goto nexttri;
		}
	}
done:
	for( j = starttri + 1; j < m_pAliasHeader->numtris; j++ )
	{
		if( g_used[j] == 2 )
			g_used[j] = 0;
	}

	return g_stripcount;
}

static void BuildTris( void )
{
	int bestverts[1024];
	int besttris[1024];

	memset( g_used, 0, sizeof( g_used ));
	g_numcommands = 0;
	g_numorder = 0;

	for( int i = 0; i < m_pAliasHeader->numtris; i++ )
	{
		if( g_used[i] ) continue;

		int bestlen = 0;
		int besttype = 0;
		for( int type = 0; type < 2; type++ )
		{
			for( int startv = 0; startv < 3; startv++ )
			{
				int len;
				if( type == 1 ) len = StripLength( i, startv );
				else len = FanLength( i, startv );

				if( len > bestlen )
				{
					besttype = type;
					bestlen = len;

					for( int j = 0; j < bestlen + 2; j++ )
						bestverts[j] = g_stripverts[j];

					for( int j = 0; j < bestlen; j++ )
						besttris[j] = g_striptris[j];
				}
			}
		}

		for( int j = 0; j < bestlen; j++ )
			g_used[besttris[j]] = 1;

		if( besttype == 1 )
			g_commands[g_numcommands++] = (bestlen + 2);
		else g_commands[g_numcommands++] = -(bestlen + 2);

		for( int j = 0; j < bestlen + 2; j++ )
		{
			int k = bestverts[j];
			g_vertexorder[g_numorder++] = k;

			float s = g_stverts[k].s;
			float t = g_stverts[k].t;

			if( !g_triangles[besttris[0]].facesfront && g_stverts[k].onseam )
				s += m_pAliasHeader->skinwidth / 2;
			s = (s + 0.5f) / m_pAliasHeader->skinwidth;
			t = (t + 0.5f) / m_pAliasHeader->skinheight;

			g_commands[g_numcommands++] = FloatAsInt( s );
			g_commands[g_numcommands++] = FloatAsInt( t );
		}
	}

	g_commands[g_numcommands++] = 0;
}

static void GL_MakeAliasModelDisplayLists( model_t *m )
{
	BuildTris( );

	m_pAliasHeader->poseverts = g_numorder;

	m_pAliasHeader->commands = Mem_Malloc( m->mempool, g_numcommands * 4 );
	memcpy( m_pAliasHeader->commands, g_commands, g_numcommands * 4 );

	m_pAliasHeader->posedata = Mem_Malloc( m->mempool, m_pAliasHeader->numposes * m_pAliasHeader->poseverts * sizeof( trivertex_t ));
	trivertex_t *verts = m_pAliasHeader->posedata;

	for( int i = 0; i < m_pAliasHeader->numposes; i++ )
	{
		for( int j = 0; j < g_numorder; j++ )
			*verts++ = m_pAliasHeader->pposeverts[i][g_vertexorder[j]];
	}
}

static rgbdata_t *Mod_CreateSkinData( model_t *mod, const byte *data, int width, int height )
{
	static rgbdata_t skin;

	skin.width = width;
	skin.height = height;
	skin.depth = 1;
	skin.type = PF_INDEXED_24;
	skin.flags = IMAGE_HAS_COLOR|IMAGE_QUAKEPAL;
	skin.encode = DXT_ENCODE_DEFAULT;
	skin.numMips = 1;
	skin.buffer = (byte *)data;
	skin.palette = (byte *)tr.palette;
	skin.size = width * height;

	if( !gEngfuncs.Image_CustomPalette() )
	{
		for( int i = 0; i < skin.width * skin.height; i++ )
		{
			if( data[i] > 224 && data[i] != 255 )
			{
				SetBits( skin.flags, IMAGE_HAS_LUMA );
				break;
			}
		}
	}

	if( mod != NULL && !Q_stricmp( mod->name, "player" ))
	{
		int i = mod->numtextures;
		mod->textures = (texture_t **)Mem_Realloc( mod->mempool, mod->textures, ( i + 1 ) * sizeof( texture_t* ));
		int size = width * height + 768;
		texture_t *tx = Mem_Calloc( mod->mempool, sizeof( *tx ) + size );
		mod->textures[i] = tx;

		Q_strncpy( tx->name, "DM_Skin", sizeof( tx->name ));
		tx->anim_min = SHIRT_HUE_START;
		tx->anim_max = SHIRT_HUE_END;
		tx->anim_total = PANTS_HUE_END;

		tx->width = width;
		tx->height = height;

		memcpy( (tx+1), data, width * height );
		memcpy( ((byte *)(tx+1)+(width * height)), skin.palette, 768 );
		mod->numtextures++;
	}

	return gEngfuncs.FS_CopyImage( &skin );
}

static const void *Mod_LoadSingleSkin( model_t *loadmodel, const daliasskintype_t *pskintype, int skinnum, int size )
{
	const byte *ptexture = (const byte *)&pskintype[1];

	string name, checkname;
	Q_snprintf( name, sizeof( name ), "%s:frame%i", loadmodel->name, skinnum );
	Q_snprintf( checkname, sizeof( checkname ), "%s_%i.tga", loadmodel->name, skinnum );

	rgbdata_t *pic;
	if( !gEngfuncs.fsapi->FileExists( checkname, false ) || ( pic = gEngfuncs.FS_LoadImage( checkname, NULL, 0 )) == NULL )
		pic = Mod_CreateSkinData( loadmodel, ptexture, m_pAliasHeader->skinwidth, m_pAliasHeader->skinheight );

	m_pAliasHeader->gl_texturenum[skinnum][0] =
		m_pAliasHeader->gl_texturenum[skinnum][1] =
		m_pAliasHeader->gl_texturenum[skinnum][2] =
		m_pAliasHeader->gl_texturenum[skinnum][3] = GX_LoadTextureInternal( name, pic, 0 );
	gEngfuncs.FS_FreeImage( pic );

	if( FBitSet( R_GetTexture( m_pAliasHeader->gl_texturenum[skinnum][0] )->flags, TF_HAS_LUMA ))
	{
		string lumaname;
		Q_snprintf( lumaname, sizeof( lumaname ), "%s:luma%i", loadmodel->name, skinnum );

		pic = Mod_CreateSkinData( NULL,	ptexture, m_pAliasHeader->skinwidth, m_pAliasHeader->skinheight );
		m_pAliasHeader->fb_texturenum[skinnum][0] =
			m_pAliasHeader->fb_texturenum[skinnum][1] =
			m_pAliasHeader->fb_texturenum[skinnum][2] =
			m_pAliasHeader->fb_texturenum[skinnum][3] = GX_LoadTextureInternal( lumaname, pic, TF_MAKELUMA );
		gEngfuncs.FS_FreeImage( pic );
	}

	return ptexture + size;
}

static const void *Mod_LoadGroupSkin( model_t *loadmodel, const daliasskintype_t *pskintype, int skinnum, int size )
{
	const daliasskingroup_t *pinskingroup = (const daliasskingroup_t *)&pskintype[1];
	const daliasskininterval_t *pinskinintervals = (const daliasskininterval_t *)(&pinskingroup[1]);
	const byte *ptexture = (const byte *)&pinskinintervals[pinskingroup->numskins];

	int i;
	for( i = 0; i < pinskingroup->numskins; i++ )
	{
		string name;
		Q_snprintf( name, sizeof( name ), "%s_%i_%i", loadmodel->name, skinnum, i );
		rgbdata_t *pic = Mod_CreateSkinData( loadmodel, ptexture, m_pAliasHeader->skinwidth, m_pAliasHeader->skinheight );
		m_pAliasHeader->gl_texturenum[skinnum][i & 3] = GX_LoadTextureInternal( name, pic, 0 );
		gEngfuncs.FS_FreeImage( pic );

		if( FBitSet( R_GetTexture( m_pAliasHeader->gl_texturenum[skinnum][i & 3] )->flags, TF_HAS_LUMA ))
		{
			string lumaname;
			Q_snprintf( lumaname, sizeof( lumaname ), "%s_%i_%i_luma", loadmodel->name, skinnum, i );
			pic = Mod_CreateSkinData( NULL, ptexture, m_pAliasHeader->skinwidth, m_pAliasHeader->skinheight );
			m_pAliasHeader->fb_texturenum[skinnum][i & 3] = GX_LoadTextureInternal( lumaname, pic, TF_MAKELUMA );
			gEngfuncs.FS_FreeImage( pic );
		}

		ptexture += size;
	}

	for( int j = i; i < 4; i++ )
	{
		m_pAliasHeader->gl_texturenum[skinnum][i & 3] = m_pAliasHeader->gl_texturenum[skinnum][i - j];
		m_pAliasHeader->fb_texturenum[skinnum][i & 3] = m_pAliasHeader->fb_texturenum[skinnum][i - j];
	}

	return ptexture;
}

static const void *Mod_LoadAllSkins( model_t *mod, int numskins, const daliasskintype_t *pskintype )
{
	int size = m_pAliasHeader->skinwidth * m_pAliasHeader->skinheight;

	for( int i = 0; i < numskins; i++ )
	{
		if( pskintype->type == ALIAS_SKIN_SINGLE )
			pskintype = Mod_LoadSingleSkin( mod, pskintype, i, size );
		else
			pskintype = Mod_LoadGroupSkin( mod, pskintype, i, size );
	}

	return pskintype;
}

void Mod_LoadAliasModel( model_t *mod, const void *buffer, qboolean *loaded )
{
	if( loaded ) *loaded = false;
	const daliashdr_t *pinmodel = (const daliashdr_t *)buffer;
	m_pAliasHeader = mod->cache.data;
	if( !m_pAliasHeader ) return;

	const daliasskintype_t *pskintype = (const daliasskintype_t *)&pinmodel[1];
	pskintype = Mod_LoadAllSkins( mod, m_pAliasHeader->numskins, pskintype );

	const stvert_t *pinstverts = (const stvert_t *)pskintype;
	memset( g_stverts, 0, sizeof( g_stverts ));
	memcpy( g_stverts, pinstverts, sizeof( g_stverts[0] ) * m_pAliasHeader->numverts );

	const dtriangle_t *pintriangles = (const dtriangle_t *)&pinstverts[m_pAliasHeader->numverts];
	memset( g_triangles, 0, sizeof( g_triangles ));
	memcpy( g_triangles, pintriangles, sizeof( g_triangles[0] ) * m_pAliasHeader->numtris );

	GL_MakeAliasModelDisplayLists( mod );

	m_pAliasHeader = NULL;

	if( loaded ) *loaded = true;
}

void Mod_AliasUnloadTextures( void *data )
{
	aliashdr_t *palias = data;
	if( !palias ) return;

	for( int i = 0; i < MAX_SKINS; i++ )
	{
		if( !palias->gl_texturenum[i][0] )
			break;

		for( int j = 0; j < 4; j++ )
		{
			GX_FreeTexture( palias->gl_texturenum[i][j] );
			GX_FreeTexture( palias->fb_texturenum[i][j] );
		}
	}
}

static void R_AliasSetupLighting( alight_t *plight )
{
	if( !m_pAliasHeader || !plight )
		return;

	g_alias.ambientlight = plight->ambientlight;
	g_alias.shadelight = plight->shadelight;
	VectorCopy( plight->plightvec, g_alias.lightvec );
	VectorCopy( plight->color, g_alias.lightcolor );

	Matrix4x4_VectorIRotate( RI.objectMatrix, g_alias.lightvec, g_alias.lightvec_local );
	VectorNormalize( g_alias.lightvec_local );
}

static void R_AliasLighting( float *lv, const vec3_t normal )
{
	float illum = g_alias.ambientlight;

	float lightcos = DotProduct( normal, g_alias.lightvec_local );
	if( lightcos > 1.0f ) lightcos = 1.0f;

	illum += g_alias.shadelight;

	float r = SHADE_LAMBERT;

	if( r <= 1.0f )
	{
		r += 1.0f;
		lightcos = (( r - 1.0f ) - lightcos) / r;
		if( lightcos > 0.0f )
			illum += g_alias.shadelight * lightcos;
	}
	else
	{
		lightcos = (lightcos + ( r - 1.0f )) / r;
		if( lightcos > 0.0f )
			illum -= g_alias.shadelight * lightcos;
	}

	illum = bound( 0.0f, illum, 255.0f );

	*lv = LightToTexGamma( illum * 4 ) / 1023.0f;
}

static void R_AliasSetRemapColors( int newTop, int newBottom )
{
	if( gEngfuncs.CL_EntitySetRemapColors( RI.currententity, RI.currentmodel, newTop, newBottom ))
		m_fDoRemap = true;
}

static void GX_DrawAliasFrame( aliashdr_t *paliashdr )
{
	trivertex_t *verts0 = paliashdr->posedata;
	trivertex_t *verts1 = paliashdr->posedata;
	verts0 += g_alias.oldpose * paliashdr->poseverts;
	verts1 += g_alias.newpose * paliashdr->poseverts;
	int *order = paliashdr->commands;

	GX_SetupVtxFormatAlias( true, true );

	while( 1 )
	{
		int count = *order++;
		if( !count ) break;

		if( count < 0 )
		{
			GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(-count) );
			count = -count;
		}
		else
		{
			GX_Begin( GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)count );
		}

		do
		{
			float s = ((float *)order)[0];
			float t = ((float *)order)[1];
			order += 2;

			vec3_t norm;
			VectorLerp( m_bytenormals[verts0->lightnormalindex], g_alias.lerpfrac, m_bytenormals[verts1->lightnormalindex], norm );
			VectorNormalize( norm );
			float lv_tmp;
			R_AliasLighting( &lv_tmp, norm );

			u8 r = (u8)(g_alias.lightcolor[0] * lv_tmp * 255.0f);
			u8 g = (u8)(g_alias.lightcolor[1] * lv_tmp * 255.0f);
			u8 b = (u8)(g_alias.lightcolor[2] * lv_tmp * 255.0f);
			u8 a = (u8)(tr.blend * 255.0f);

			vec3_t vert;
			VectorLerp( verts0->v, g_alias.lerpfrac, verts1->v, vert );

			GX_Position3f32( vert[0], vert[1], vert[2] );
			GX_Color4u8( r, g, b, a );
			GX_TexCoord2f32( s, t );

			verts0++; verts1++;
		} while( --count );

		GX_End();
	}
}

static void GX_DrawAliasShadow( aliashdr_t *paliashdr )
{
	if( FBitSet( RI.currententity->curstate.effects, EF_NOSHADOW ))
		return;

	float vec_x = -g_alias.lightvec[0] * 8.0f;
	float vec_y = -g_alias.lightvec[1] * 8.0f;

	r_stats.c_alias_polys += paliashdr->numtris;

	trivertex_t *verts0 = paliashdr->posedata;
	trivertex_t *verts1 = paliashdr->posedata;
	verts0 += g_alias.oldpose * paliashdr->poseverts;
	verts1 += g_alias.newpose * paliashdr->poseverts;
	int *order = paliashdr->commands;

	GX_SetupVtxFormatAlias( true, false );

	u8 shadowAlpha = (u8)(0.5f * 255.0f);

	while( 1 )
	{
		int count = *order++;
		if( !count ) break;

		if( count < 0 )
		{
			GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(-count) );
			count = -count;
		}
		else
		{
			GX_Begin( GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)count );
		}

		do
		{
			order += 2;

			vec3_t av, point;
			VectorLerp( verts0->v, g_alias.lerpfrac, verts1->v, av );
			point[0] = av[0] * paliashdr->scale[0] + paliashdr->scale_origin[0];
			point[1] = av[1] * paliashdr->scale[1] + paliashdr->scale_origin[1];
			point[2] = av[2] * paliashdr->scale[2] + paliashdr->scale_origin[2];
			Matrix3x4_VectorTransform( RI.objectMatrix, point, av );

			point[0] = av[0] - (vec_x * ( av[2] - g_alias.lightspot[2] ));
			point[1] = av[1] - (vec_y * ( av[2] - g_alias.lightspot[2] ));
			point[2] = g_alias.lightspot[2] + 1.0f;

			GX_Position3f32( point[0], point[1], point[2] );
			GX_Color4u8( 0, 0, 0, shadowAlpha );

			verts0++; verts1++;
		} while( --count );

		GX_End();
	}
}

static void R_AliasLerpMovement( cl_entity_t *e )
{
	float	f = 1.0f;

	if( g_alias.interpolate && ( g_alias.time < e->curstate.animtime + 1.0f ) && ( e->curstate.animtime != e->latched.prevanimtime ))
		f = ( g_alias.time - e->curstate.animtime ) / ( e->curstate.animtime - e->latched.prevanimtime );

	if( ENGINE_GET_PARM( PARM_PLAYING_DEMO ) == DEMO_QUAKE1 )
		f = f + 1.0f;

	g_alias.lerpfrac = bound( 0.0f, f, 1.0f );

	if( e->player || e->curstate.movetype != MOVETYPE_STEP )
		return;

	VectorLerp( e->latched.prevorigin, f, e->curstate.origin, e->origin );

	if( !VectorCompareEpsilon( e->curstate.angles, e->latched.prevangles, ON_EPSILON ))
	{
		vec4_t	q, q1, q2;

		AngleQuaternion( e->curstate.angles, q1, false );
		AngleQuaternion( e->latched.prevangles, q2, false );
		QuaternionSlerp( q2, q1, f, q );
		QuaternionAngle( q, e->angles );
	}
	else VectorCopy( e->curstate.angles, e->angles );

	if( FBitSet( e->model->flags, ALIAS_ROTATE ))
		e->angles[1] = anglemod( 100.0f * g_alias.time );
}

static void R_SetupAliasFrame( cl_entity_t *e, aliashdr_t *paliashdr )
{
	int oldframe = e->latched.prevframe;
	int newframe = e->curstate.frame;

	if( newframe < 0 )
	{
		newframe = 0;
	}
	else if( newframe >= paliashdr->numframes )
	{
		if( newframe > paliashdr->numframes )
			gEngfuncs.Con_Reportf( S_WARN "%s: no such frame %d (%s)\n", __func__, newframe, e->model->name );
		newframe = paliashdr->numframes - 1;
	}

	if(( oldframe >= paliashdr->numframes ) || ( oldframe < 0 ))
		oldframe = newframe;

	int numposes = paliashdr->frames[newframe].numposes;

	int oldpose, newpose;
	if( numposes > 1 )
	{
		oldpose = newpose = paliashdr->frames[newframe].firstpose;
		float interval = 1.0f / paliashdr->frames[newframe].interval;
		int cycle = (int)(g_alias.time * interval);
		oldpose += (cycle + 0) % numposes;
		newpose += (cycle + 1) % numposes;
		g_alias.lerpfrac = ( g_alias.time * interval );
		g_alias.lerpfrac -= (int)g_alias.lerpfrac;
	}
	else
	{
		oldpose = paliashdr->frames[oldframe].firstpose;
		newpose = paliashdr->frames[newframe].firstpose;
	}

	g_alias.oldpose = oldpose;
	g_alias.newpose = newpose;

	GX_DrawAliasFrame( paliashdr );
}

static void R_AliasDrawAbsBBox( cl_entity_t *e, const vec3_t absmin, const vec3_t absmax )
{
	if( r_drawentities->value != 5 || e == tr.viewent )
		return;

	vec3_t p[8];
	for( int i = 0; i < 8; i++ )
	{
		p[i][0] = ( i & 1 ) ? absmin[0] : absmax[0];
		p[i][1] = ( i & 2 ) ? absmin[1] : absmax[1];
		p[i][2] = ( i & 4 ) ? absmin[2] : absmax[2];
	}

	GX_Bind( XASH_TEXTURE0, tr.whiteTexture );
	TriColor4f( 0.5f, 0.5f, 1.0f, 0.5f );
	TriRenderMode( kRenderTransAdd );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

	TriBegin( TRI_QUADS );
	for( int i = 0; i < 6; i++ )
	{
		TriBrightness( g_alias.shadelight / 255.0f );
		TriVertex3fv( p[boxpnt[i][0]] );
		TriVertex3fv( p[boxpnt[i][1]] );
		TriVertex3fv( p[boxpnt[i][2]] );
		TriVertex3fv( p[boxpnt[i][3]] );
	}
	TriEnd();

	TriRenderMode( kRenderNormal );
}

static void R_AliasDrawLightTrace( cl_entity_t *e )
{
	if( r_drawentities->value == 7 )
	{
		vec3_t	origin;

		GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
		GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
		GX_SetupVtxFormatAlias( false, false );

		GXColor c1 = { 255, 128, 0, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, c1 );
		GX_Begin( GX_LINES, GX_VTXFMT0, 2 );
		GX_Position3f32( e->origin[0], e->origin[1], e->origin[2] );
		GX_Position3f32( g_alias.lightspot[0], g_alias.lightspot[1], g_alias.lightspot[2] );
		GX_End();

		GXColor c2 = { 0, 128, 255, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, c2 );
		VectorMA( g_alias.lightspot, -64.0f, g_alias.lightvec, origin );
		GX_Begin( GX_LINES, GX_VTXFMT0, 2 );
		GX_Position3f32( g_alias.lightspot[0], g_alias.lightspot[1], g_alias.lightspot[2] );
		GX_Position3f32( origin[0], origin[1], origin[2] );
		GX_End();

		GX_SetPointSize( (u8)(5 * 6), GX_TO_ZERO );
		GXColor c3 = { 255, 0, 0, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, c3 );
		GX_Begin( GX_POINTS, GX_VTXFMT0, 1 );
		GX_Position3f32( g_alias.lightspot[0], g_alias.lightspot[1], g_alias.lightspot[2] );
		GX_End();
		GX_SetPointSize( (u8)(1 * 6), GX_TO_ZERO );

		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	}
}

static void R_AliasSetupTimings( void )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
	{
		g_alias.time = gp_cl->time;
	}
	else
	{
		g_alias.time = gp_host->realtime;
	}

	m_fDoRemap = false;
}

void R_DrawAliasModel( cl_entity_t *e )
{
	model_t *clmodel = RI.currententity->model;

	vec3_t absmin, absmax;
	VectorAdd( e->origin, clmodel->mins, absmin );
	VectorAdd( e->origin, clmodel->maxs, absmax );

	if( R_CullModel( e, absmin, absmax ))
		return;

	m_pAliasHeader = (aliashdr_t *)gEngfuncs.Mod_Extradata( mod_alias, RI.currententity->model );
	if( !m_pAliasHeader ) return;

	R_AliasSetupTimings();

	vec3_t angles = Vec3( e->angles );

	R_AliasLerpMovement( e );

	if( !FBitSet( gp_host->features, ENGINE_COMPENSATE_QUAKE_BUG ))
		e->angles[PITCH] = -e->angles[PITCH];

	if( e->player ) e->angles[PITCH] = 0.0f;

	vec3_t dir;
	alight_t lighting =
	{
		.plightvec = dir,
	};
	R_EntityDynamicLight( e, &lighting, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_alias.time, g_alias.lightspot, g_alias.lightvec );

	r_stats.c_alias_polys += m_pAliasHeader->numtris;
	r_stats.c_alias_models_drawn++;

	R_RotateForEntity( e );

	matrix4x4 saveModelview;
	Matrix4x4_Copy( saveModelview, RI.modelviewMatrix );

	if( tr.fFlipViewModel )
	{
		Matrix4x4_ConcatScale( RI.modelviewMatrix,  m_pAliasHeader->scale[0], -m_pAliasHeader->scale[1], m_pAliasHeader->scale[2] );
		Matrix4x4_ConcatTranslate( RI.modelviewMatrix, m_pAliasHeader->scale_origin[0], -m_pAliasHeader->scale_origin[1], m_pAliasHeader->scale_origin[2] );
	}
	else
	{
		Matrix4x4_ConcatScale( RI.modelviewMatrix,  m_pAliasHeader->scale[0], m_pAliasHeader->scale[1], m_pAliasHeader->scale[2] );
		Matrix4x4_ConcatTranslate( RI.modelviewMatrix, m_pAliasHeader->scale_origin[0], m_pAliasHeader->scale_origin[1], m_pAliasHeader->scale_origin[2] );
	}

	Mtx gxMv;
	Matrix4x4_ToMtx( gxMv, RI.modelviewMatrix );
	GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );

	R_AliasSetupLighting( &lighting );
	GX_SetRenderMode( e->curstate.rendermode );

	player_info_t *playerinfo;
	if( e->player && ( playerinfo = pfnPlayerInfo( e->curstate.number - 1 )) != NULL )
	{
		int topcolor = bound( 0, playerinfo->topcolor, 13 );
		int bottomcolor = bound( 0, playerinfo->bottomcolor, 13 );
		R_AliasSetRemapColors( topcolor, bottomcolor );
	}

	int anim = (int)(g_alias.time * 10) & 3;
	int skin = bound( 0, RI.currententity->curstate.skin, m_pAliasHeader->numskins - 1 );
	remap_info_t *pinfo = NULL;
	if( m_fDoRemap ) pinfo = gEngfuncs.CL_GetRemapInfoForEntity( e );

	qboolean hasLuma = false;
	if( r_lightmap->value && !r_fullbright->value )
	{
		GX_Bind( XASH_TEXTURE0, tr.whiteTexture );
	}
	else if( pinfo != NULL && pinfo->textures[skin] != 0 )
	{
		GX_Bind( XASH_TEXTURE0, pinfo->textures[skin] );
	}
	else
	{
		GX_Bind( XASH_TEXTURE0, m_pAliasHeader->gl_texturenum[skin][anim] );
		if( FBitSet( R_GetTexture( m_pAliasHeader->gl_texturenum[skin][anim] )->flags, TF_HAS_ALPHA ))
		{
			GX_SetAlphaCompare( GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0 );
			tr.blend = 1.0f;
		}
	}

	if( m_pAliasHeader->fb_texturenum[skin][anim] )
	{
		hasLuma = true;
		GX_Bind( XASH_TEXTURE1, m_pAliasHeader->fb_texturenum[skin][anim] );

	
		GX_SetNumTevStages( 2 );

		
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );

		GX_SetTevOrder( GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR0A0 );
		GX_SetTevColorIn( GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO );
		GX_SetTevColorOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
		GX_SetTevAlphaIn( GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO );
		GX_SetTevAlphaOp( GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV );
	}
	else
	{
		GX_SetNumTevStages( 1 );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	}

	R_SetupAliasFrame( e, m_pAliasHeader );

	GX_SetNumTevStages( 1 );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

	Matrix4x4_Copy( RI.modelviewMatrix, saveModelview );
	Matrix4x4_ToMtx( gxMv, RI.modelviewMatrix );
	GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );

	VectorAdd( e->origin, clmodel->mins, absmin );
	VectorAdd( e->origin, clmodel->maxs, absmax );

	R_AliasDrawAbsBBox( e, absmin, absmax );
	R_AliasDrawLightTrace( e );

	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );

	if( r_shadows.value )
	{
		Matrix4x4_CreateFromEntity( RI.objectMatrix, e->angles, e->origin, 1.0f );

		R_LoadIdentity();

		GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LESS, GX_TRUE );

		GX_DrawAliasShadow( m_pAliasHeader );

		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
		GXColor white = { 255, 255, 255, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, white );

		Matrix4x4_Copy( RI.modelviewMatrix, saveModelview );
		Matrix4x4_ToMtx( gxMv, RI.modelviewMatrix );
		GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );
	}

	VectorCopy( angles, e->angles );
}