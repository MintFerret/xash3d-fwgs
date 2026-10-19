/*
gx_decals.c - decal paste and rendering (Wii GX native port)
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

#define DECAL_OVERLAP_DISTANCE	2
#define DECAL_DISTANCE		4
#define MAX_DECALCLIPVERT		32
#define DECAL_CACHEENTRY		256
#define DECAL_TRANSPARENT_THRESHOLD	230

#define MAX_OVERLAP_DECALS		6
#define DECAL_OVERLAP_DIST		8
#define MIN_DECAL_SCALE		0.01f
#define MAX_DECAL_SCALE		16.0f

#define LEFT_EDGE			0
#define RIGHT_EDGE			1
#define TOP_EDGE			2
#define BOTTOM_EDGE			3

typedef struct
{
	vec3_t		m_Position;
	model_t		*m_pModel;
	int		m_iTexture;
	int		m_Size;
	int		m_Flags;
	int		m_Entity;
	float		m_scale;
	int		m_decalWidth;
	int		m_decalHeight;
	vec3_t		m_Basis[3];
} decalinfo_t;

static float	g_DecalClipVerts[MAX_DECALCLIPVERT][VERTEXSIZE];
static float	g_DecalClipVerts2[MAX_DECALCLIPVERT][VERTEXSIZE];

decal_t	gDecalPool[MAX_RENDER_DECALS];
static int	gDecalCount;

void R_ClearDecals( void )
{
	memset( gDecalPool, 0, sizeof( gDecalPool ));
	gDecalCount = 0;
}

static void R_DecalUnlink( decal_t *pdecal )
{
	if( pdecal->psurface )
	{
		if( pdecal->psurface->pdecals == pdecal )
		{
			pdecal->psurface->pdecals = pdecal->pnext;
		}
		else
		{
			decal_t *tmp = pdecal->psurface->pdecals;
			if( !tmp ) gEngfuncs.Host_Error( "%s: bad decal list\n", __func__ );

			while( tmp->pnext )
			{
				if( tmp->pnext == pdecal )
				{
					tmp->pnext = pdecal->pnext;
					break;
				}
				tmp = tmp->pnext;
			}
		}
	}

	if( pdecal->polys )
		Mem_Free( pdecal->polys );

	pdecal->psurface = NULL;
	pdecal->polys = NULL;
}

static decal_t *R_DecalAlloc( decal_t *pdecal )
{
	int	limit = MAX_RENDER_DECALS;

	if( ref_r_decals->value < limit )
		limit = ref_r_decals->value;

	if( !limit ) return NULL;

	if( !pdecal )
	{
		int	count = 0;
		qboolean found = false;

		do
		{
			if( gDecalCount >= limit )
				gDecalCount = 0;

			pdecal = &gDecalPool[gDecalCount];
			gDecalCount++;
			count++;

			/* Si este slot no es permanente, usarlo */
			if( !FBitSet( pdecal->flags, FDECAL_PERMANENT ))
			{
				found = true;
				break;
			}
		} while( count < limit );

		/* Si todos los slots estan ocupados, reciclar el primero */
		if( !found )
		{
			pdecal = &gDecalPool[0];
			gDecalCount = 1;
		}

		/* Verificacion de seguridad */
		if( !pdecal )
			return NULL;
	}

	R_DecalUnlink( pdecal );

	return pdecal;
}

static void R_GetDecalDimensions( int texture, int *width, int *height )
{
	if( width ) *width = 1;
	if( height ) *height = 1;

	R_GetTextureParms( width, height, texture );
}

static void R_DecalComputeBasis( msurface_t *surf, int flags, vec3_t textureSpaceBasis[3] )
{
	vec3_t	surfaceNormal;

	if( surf->flags & SURF_PLANEBACK )
		VectorNegate( surf->plane->normal, surfaceNormal );
	else VectorCopy( surf->plane->normal, surfaceNormal );

	VectorNormalize2( surfaceNormal, textureSpaceBasis[2] );
	VectorNormalize2( surf->texinfo->vecs[0], textureSpaceBasis[0] );
	VectorNormalize2( surf->texinfo->vecs[1], textureSpaceBasis[1] );
}

static void R_SetupDecalTextureSpaceBasis( decal_t *pDecal, msurface_t *surf, int texture, vec3_t textureSpaceBasis[3], float decalWorldScale[2] )
{
	int	width, height;

	R_DecalComputeBasis( surf, pDecal->flags, textureSpaceBasis );
	R_GetDecalDimensions( texture, &width, &height );

	decalWorldScale[0] = (float)pDecal->scale / width;
	decalWorldScale[1] = (float)pDecal->scale / height;

	VectorScale( textureSpaceBasis[0], decalWorldScale[0], textureSpaceBasis[0] );
	VectorScale( textureSpaceBasis[1], decalWorldScale[1], textureSpaceBasis[1] );
}

static void R_SetupDecalVertsForMSurface( decal_t *pDecal, msurface_t *surf,	vec3_t textureSpaceBasis[3], float *verts )
{
	float *v = surf->polys->verts[0];
	for( int i = 0; i < surf->polys->numverts; i++, v += VERTEXSIZE, verts += VERTEXSIZE )
	{
		VectorCopy( v, verts );
		verts[3] = DotProduct( verts, textureSpaceBasis[0] ) - pDecal->dx + 0.5f;
		verts[4] = DotProduct( verts, textureSpaceBasis[1] ) - pDecal->dy + 0.5f;
		verts[5] = verts[6] = 0.0f;
	}
}

static void R_SetupDecalClip( decal_t *pDecal, msurface_t *surf, int texture, vec3_t textureSpaceBasis[3], float decalWorldScale[2] )
{
	R_SetupDecalTextureSpaceBasis( pDecal, surf, texture, textureSpaceBasis, decalWorldScale );

	pDecal->dx = DotProduct( pDecal->position, textureSpaceBasis[0] );
	pDecal->dy = DotProduct( pDecal->position, textureSpaceBasis[1] );
}

static int R_ClipInside( float *vert, int edge )
{
	switch( edge )
	{
	case LEFT_EDGE:
		if( vert[3] > 0.0f )
			return 1;
		return 0;
	case RIGHT_EDGE:
		if( vert[3] < 1.0f )
			return 1;
		return 0;
	case TOP_EDGE:
		if( vert[4] > 0.0f )
			return 1;
		return 0;
	case BOTTOM_EDGE:
		if( vert[4] < 1.0f )
			return 1;
		return 0;
	}
	return 0;
}

static void R_ClipIntersect( float *one, float *two, float *out, int edge )
{
	float t;
	if( edge < TOP_EDGE )
	{
		if( edge == LEFT_EDGE )
		{
			t = ((one[3] - 0.0f) / (one[3] - two[3]));
			out[3] = out[5] = 0.0f;
		}
		else
		{
			t = ((one[3] - 1.0f) / (one[3] - two[3]));
			out[3] = out[5] = 1.0f;
		}

		out[4] = one[4] + (two[4] - one[4]) * t;
		out[6] = one[6] + (two[6] - one[6]) * t;
	}
	else
	{
		if( edge == TOP_EDGE )
		{
			t = ((one[4] - 0.0f)  / (one[4] - two[4]));
			out[4] = out[6] = 0.0f;
		}
		else
		{
			t = ((one[4] - 1.0f) / (one[4] - two[4]));
			out[4] = out[6] = 1.0f;
		}

		out[3] = one[3] + (two[3] - one[3]) * t;
		out[5] = one[5] + (two[4] - one[5]) * t;
	}

	VectorLerp( one, t, two, out );
}

static int SHClip( float *vert, int vertCount, float *out, int edge )
{
	int outCount = 0;

	float *s = &vert[(vertCount - 1) * VERTEXSIZE];

	for( int j = 0; j < vertCount; j++ )
	{
		float *p = &vert[j * VERTEXSIZE];

		if( R_ClipInside( p, edge ))
		{
			if( R_ClipInside( s, edge ))
			{
				memcpy( out, p, sizeof( float ) * VERTEXSIZE );
				out += VERTEXSIZE;
				outCount++;
			}
			else
			{
				R_ClipIntersect( s, p, out, edge );
				out += VERTEXSIZE;
				outCount++;

				memcpy( out, p, sizeof( float ) * VERTEXSIZE );
				out += VERTEXSIZE;
				outCount++;
			}
		}
		else
		{
			if( R_ClipInside( s, edge ))
			{
				R_ClipIntersect( p, s, out, edge );
				out += VERTEXSIZE;
				outCount++;
			}
		}

		s = p;
	}

	return outCount;
}

static float *R_DoDecalSHClip( float *pInVerts, decal_t *pDecal, int nStartVerts, int *pVertCount )
{
	float *pOutVerts = g_DecalClipVerts[0];

	int outCount = SHClip( pInVerts, nStartVerts, g_DecalClipVerts2[0], LEFT_EDGE );
	outCount = SHClip( g_DecalClipVerts2[0], outCount, g_DecalClipVerts[0], RIGHT_EDGE );
	outCount = SHClip( g_DecalClipVerts[0], outCount, g_DecalClipVerts2[0], TOP_EDGE );
	outCount = SHClip( g_DecalClipVerts2[0], outCount, pOutVerts, BOTTOM_EDGE );

	if( pVertCount )
		*pVertCount = outCount;

	return pOutVerts;
}

static float *R_DecalVertsClip( decal_t *pDecal, msurface_t *surf, int texture, int *pVertCount )
{
	float	decalWorldScale[2];
	vec3_t	textureSpaceBasis[3];

	R_SetupDecalClip( pDecal, surf, texture, textureSpaceBasis, decalWorldScale );

	R_SetupDecalVertsForMSurface( pDecal, surf, textureSpaceBasis, g_DecalClipVerts[0] );

	return R_DoDecalSHClip( g_DecalClipVerts[0], pDecal, surf->polys->numverts, pVertCount );
}

static void R_DecalVertsLight( float *v, msurface_t *surf, int vertCount )
{
	float sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );

	for( int j = 0; j < vertCount; j++, v += VERTEXSIZE )
	{
		R_LightmapCoord( v, surf, sample_size, &v[5] );
	}
}

static decal_t *R_DecalIntersect( decalinfo_t *decalinfo, msurface_t *surf, int *pcount )
{
	decal_t *plast = NULL;
	float lastArea = 2;
	*pcount = 0;

	int texture = decalinfo->m_iTexture;

	int mapSize[2];
	R_GetDecalDimensions( texture, &mapSize[0], &mapSize[1] );
	vec3_t decalExtents[2];
	VectorScale( decalinfo->m_Basis[0], ((mapSize[0] / decalinfo->m_scale) * 0.5f), decalExtents[0] );
	VectorScale( decalinfo->m_Basis[1], ((mapSize[1] / decalinfo->m_scale) * 0.5f), decalExtents[1] );

	decal_t *pDecal = surf->pdecals;

	while( pDecal )
	{
		texture = pDecal->texture;

		if( !FBitSet( pDecal->flags, FDECAL_PERMANENT ))
		{
			vec3_t	testBasis[3];
			vec3_t	testPosition[2];
			float	testWorldScale[2];

			R_SetupDecalTextureSpaceBasis( pDecal, surf, texture, testBasis, testWorldScale );

			VectorSubtract( decalinfo->m_Position, decalExtents[0], testPosition[0] );
			VectorSubtract( decalinfo->m_Position, decalExtents[1], testPosition[1] );

			vec2_t vDecalMin = {
				DotProduct( testPosition[0], testBasis[0] ) - pDecal->dx + 0.5f,
				DotProduct( testPosition[1], testBasis[1] ) - pDecal->dy + 0.5f };

			VectorAdd( decalinfo->m_Position, decalExtents[0], testPosition[0] );
			VectorAdd( decalinfo->m_Position, decalExtents[1], testPosition[1] );

			vec2_t vDecalMax = {
				DotProduct( testPosition[0], testBasis[0] ) - pDecal->dx + 0.5f,
				DotProduct( testPosition[1], testBasis[1] ) - pDecal->dy + 0.5f };

			vec2_t vUnionMin = { Q_max( vDecalMin[0], 0 ), Q_max( vDecalMin[1], 0 ) };
			vec2_t vUnionMax = { Q_min( vDecalMax[0], 1 ), Q_min( vDecalMax[1], 1 ) };

			if( vUnionMin[0] < 1 && vUnionMin[1] < 1 && vUnionMax[0] > 0 && vUnionMax[1] > 0 )
			{
				float	flArea = (vUnionMax[0] - vUnionMin[1]) * (vUnionMax[1] - vUnionMin[1]);

				if( flArea > 0.6f )
				{
					*pcount += 1;

					if( !plast || flArea <= lastArea )
					{
						plast = pDecal;
						lastArea =  flArea;
					}
				}
			}
		}
		pDecal = pDecal->pnext;
	}
	return plast;
}

static glpoly2_t *R_DecalCreatePoly( decalinfo_t *decalinfo, decal_t *pdecal, msurface_t *surf )
{
	if( pdecal->polys )
		return pdecal->polys;

	int lnumverts;
	float *v = R_DecalSetupVerts( pdecal, surf, pdecal->texture, &lnumverts );
	if( !lnumverts ) return NULL;

	glpoly2_t *poly = Mem_Calloc( r_temppool, sizeof( glpoly2_t ) + lnumverts * VERTEXSIZE * sizeof( float ));
	poly->next = pdecal->polys;
	poly->flags = surf->flags;
	pdecal->polys = poly;
	poly->numverts = lnumverts;

	for( int i = 0; i < lnumverts; i++, v += VERTEXSIZE )
	{
		VectorCopy( v, poly->verts[i] );
		poly->verts[i][3] = v[3];
		poly->verts[i][4] = v[4];
		poly->verts[i][5] = v[5];
		poly->verts[i][6] = v[6];
	}

	return poly;
}

static void R_AddDecalToSurface( decal_t *pdecal, msurface_t *surf, decalinfo_t *decalinfo )
{
	pdecal->pnext = NULL;
	decal_t *pold = surf->pdecals;

	if( pold )
	{
		while( pold->pnext )
			pold = pold->pnext;
		pold->pnext = pdecal;
	}
	else
	{
		surf->pdecals = pdecal;
	}

	pdecal->psurface = surf;

	R_DecalCreatePoly( decalinfo, pdecal, surf );
	R_AddDecalVBO( pdecal, surf );
}

static void R_DecalCreate( decalinfo_t *decalinfo, msurface_t *surf, float x, float y )
{
	if( !surf ) return;

	int count;
	decal_t *pold = R_DecalIntersect( decalinfo, surf, &count );
	if( count < MAX_OVERLAP_DECALS ) pold = NULL;

	decal_t *pdecal = R_DecalAlloc( pold );
	if( !pdecal ) return;

	pdecal->flags = decalinfo->m_Flags;

	VectorCopy( decalinfo->m_Position, pdecal->position );

	pdecal->dx = x;
	pdecal->dy = y;

	pdecal->scale = decalinfo->m_scale;
	pdecal->entityIndex = decalinfo->m_Entity;
	pdecal->texture = decalinfo->m_iTexture;

	int vertCount;
	R_DecalVertsClip( pdecal, surf, decalinfo->m_iTexture, &vertCount );

	if( !vertCount )
	{
		R_DecalUnlink( pdecal );
		return;
	}

	R_AddDecalToSurface( pdecal, surf, decalinfo );
}

static void R_DecalSurface( msurface_t *surf, decalinfo_t *decalinfo )
{
	mtexinfo_t *tex = surf->texinfo;
	connstate_t state = ENGINE_GET_PARM( PARM_CONNSTATE );

	if( state == ca_connected || state == ca_validate )
	{
		decal_t *decal = surf->pdecals;
		while( decal != NULL )
		{
			if( VectorCompare( decal->position, decalinfo->m_Position ) && decal->texture == decalinfo->m_iTexture )
				return;
			decal = decal->pnext;
		}
	}

	vec4_t textureU = Vec4( tex->vecs[0] );
	vec4_t textureV = Vec4( tex->vecs[1] );

	float s = DotProduct( decalinfo->m_Position, textureU ) + textureU[3] - surf->texturemins[0];
	float t = DotProduct( decalinfo->m_Position, textureV ) + textureV[3] - surf->texturemins[1];

	R_DecalComputeBasis( surf, decalinfo->m_Flags, decalinfo->m_Basis );

	float w = fabs( decalinfo->m_decalWidth  * DotProduct( textureU, decalinfo->m_Basis[0] )) +
	    fabs( decalinfo->m_decalHeight * DotProduct( textureU, decalinfo->m_Basis[1] ));

	float h = fabs( decalinfo->m_decalWidth  * DotProduct( textureV, decalinfo->m_Basis[0] )) +
	    fabs( decalinfo->m_decalHeight * DotProduct( textureV, decalinfo->m_Basis[1] ));

	s -= ( w * 0.5f );
	t -= ( h * 0.5f );

	if( s <= -w || t <= -h || s > (surf->extents[0] + w) || t > (surf->extents[1] + h))
	{
		return;
	}

	R_DecalCreate( decalinfo, surf, s, t );
}

static void R_DecalNodeSurfaces( model_t *model, mnode_t *node, decalinfo_t *decalinfo )
{
	int firstsurface = node_firstsurface( node, model );
	int numsurfaces  = node_numsurfaces( node, model );

	msurface_t *surf = model->surfaces + firstsurface;

	for( int i = 0; i < numsurfaces; i++, surf++ )
	{
		if( surf->flags & (SURF_DRAWTURB|SURF_DRAWSKY|SURF_CONVEYOR))
			continue;

		if( surf->flags & SURF_TRANSPARENT )
			continue;

		R_DecalSurface( surf, decalinfo );
	}
}

static void R_DecalNode( model_t *model, mnode_t *node, decalinfo_t *decalinfo )
{
	if( node->contents < 0 )
	{
		return;
	}

	mplane_t *splitplane = node->plane;
	float dist = DotProduct( decalinfo->m_Position, splitplane->normal ) - splitplane->dist;

	if( dist > decalinfo->m_Size )
	{
		R_DecalNode( model, node_child( node, 0, model ), decalinfo );
	}
	else if( dist < -decalinfo->m_Size )
	{
		R_DecalNode( model, node_child( node, 1, model ), decalinfo );
	}
	else
	{
		if( dist < DECAL_DISTANCE && dist > -DECAL_DISTANCE )
			R_DecalNodeSurfaces( model, node, decalinfo );

		R_DecalNode( model, node_child( node, 0, model ), decalinfo );
		R_DecalNode( model, node_child( node, 1, model ), decalinfo );
	}
}

void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale )
{
	if( textureIndex <= 0 || textureIndex >= MAX_TEXTURES )
	{
		gEngfuncs.Con_Printf( S_ERROR "Decal has invalid texture!\n" );
		return;
	}

	cl_entity_t *ent = NULL;
	model_t *model = NULL;
	if( entityIndex > 0 )
	{
		ent = CL_GetEntityByIndex( entityIndex );

		if( modelIndex > 0 ) model = CL_ModelHandle( modelIndex );
		else if( ent != NULL ) model = CL_ModelHandle( ent->curstate.modelindex );
		else return;
	}
	else if( modelIndex > 0 )
		model = CL_ModelHandle( modelIndex );
	else model = CL_ModelHandle( 1 );

	if( !model ) return;

	if( model->type != mod_brush )
	{
		gEngfuncs.Con_Reportf( S_ERROR "Decals must hit mod_brush!\n" );
		return;
	}

	decalinfo_t decalInfo;
	decalInfo.m_pModel = model;
	hull_t *hull = &model->hulls[0];

	if( ent && !FBitSet( flags, FDECAL_LOCAL_SPACE ))
	{
		vec3_t pos_l;

		if( !VectorIsNull( ent->angles ))
		{
			matrix4x4 matrix;

			Matrix4x4_CreateFromEntity( matrix, ent->angles, ent->origin, 1.0f );
			Matrix4x4_VectorITransform( matrix, pos, pos_l );
		}
		else
		{
			VectorSubtract( pos, ent->origin, pos_l );
		}

		VectorCopy( pos_l, decalInfo.m_Position );
		SetBits( flags, FDECAL_LOCAL_SPACE );
	}
	else
	{
		VectorCopy( pos, decalInfo.m_Position );
	}

	if( !FBitSet( model->flags, MODEL_HAS_ORIGIN ))
		SetBits( flags, FDECAL_USE_LANDMARK );

	decalInfo.m_iTexture = textureIndex;
	decalInfo.m_Entity = entityIndex;
	decalInfo.m_Flags = flags;

	int width, height;
	R_GetDecalDimensions( textureIndex, &width, &height );
	decalInfo.m_Size = width >> 1;
	if(( height >> 1 ) > decalInfo.m_Size )
		decalInfo.m_Size = height >> 1;

	decalInfo.m_scale = bound( MIN_DECAL_SCALE, scale, MAX_DECAL_SCALE );

	decalInfo.m_decalWidth = width / decalInfo.m_scale;
	decalInfo.m_decalHeight = height / decalInfo.m_scale;

	R_DecalNode( model, &model->nodes[hull->firstclipnode], &decalInfo );
}

float *R_DecalSetupVerts( decal_t *pDecal, msurface_t *surf, int texture, int *outCount )
{
	glpoly2_t *p = pDecal->polys;
	float *v;
	int count;

	if( p )
	{
		v = g_DecalClipVerts[0];
		count = p->numverts;
		float *v2 = p->verts[0];

		for( int i = 0; i < count; i++, v += VERTEXSIZE, v2 += VERTEXSIZE )
		{
			VectorCopy( v2, v );
			v[3] = v2[3];
			v[4] = v2[4];
			v[5] = v2[5];
			v[6] = v2[6];
		}

		v = g_DecalClipVerts[0];
	}
	else
	{
		v = R_DecalVertsClip( pDecal, surf, texture, &count );
		R_DecalVertsLight( v, surf, count );
	}

	if( outCount )
		*outCount = count;

	return v;
}

static void GX_DrawDecalPolygon( float *verts, int numVerts )
{
	if( numVerts < 3 ) return;

	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS,  GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0 );

	GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, numVerts );

	for( int i = 0; i < numVerts; i++, verts += VERTEXSIZE )
	{
		GX_Position3f32( verts[0], verts[1], verts[2] );
		GX_TexCoord2f32( verts[3], verts[4] );
	}

	GX_End();
}

void DrawSingleDecal( decal_t *pDecal, msurface_t *fa )
{
	int numVerts;
	float *v = R_DecalSetupVerts( pDecal, fa, pDecal->texture, &numVerts );
	if( !numVerts ) return;

	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GXColor white = { 255, 255, 255, 255 };
	GX_SetChanMatColor( GX_COLOR0A0, white );

	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

	GX_Bind( XASH_TEXTURE0, pDecal->texture );

	if( FBitSet( R_GetTexture( pDecal->texture )->flags, TF_PREMULTIPLIED ))
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
	else
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );

	GX_DrawDecalPolygon( v, numVerts );
}

void DrawSurfaceDecals( msurface_t *fa, qboolean single, qboolean reverse )
{
	if( !fa->pdecals ) return;

	cl_entity_t *e = RI.currententity;
	Assert( e != NULL );

	if( single )
	{
		if( e->curstate.rendermode == kRenderNormal || e->curstate.rendermode == kRenderTransAlpha )
		{
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
			if( e->curstate.rendermode == kRenderTransAlpha )
				GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		}

		if( e->curstate.rendermode == kRenderTransTexture || e->curstate.rendermode == kRenderTransAdd )
			GX_Cull( XASH_CULL_NONE );

		if( gl_polyoffset.value )
			GX_PushPolygonOffset( -1.0f, -gl_polyoffset.value );
	}

	if( reverse && e->curstate.rendermode == kRenderTransTexture )
	{
		decal_t *list[1024];
		int count = 0;

		for( decal_t *p = fa->pdecals; p && count < 1024; p = p->pnext )
			if( p->texture ) list[count++] = p;

		for( int i = count - 1; i >= 0; i-- )
			DrawSingleDecal( list[i], fa );
	}
	else
	{
		for( decal_t *p = fa->pdecals; p; p = p->pnext )
		{
			if( !p->texture ) continue;
			DrawSingleDecal( p, fa );
		}
	}

	if( single )
	{
		if( e->curstate.rendermode == kRenderNormal || e->curstate.rendermode == kRenderTransAlpha )
		{
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
			GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
			if( e->curstate.rendermode == kRenderTransAlpha )
				GX_SetAlphaCompare( GX_GREATER, (u8)(DEFAULT_ALPHATEST * 255.0f), GX_AOP_AND, GX_ALWAYS, 0 );
		}

		if( gl_polyoffset.value )
			GX_PopPolygonOffset();

		if( e->curstate.rendermode == kRenderTransTexture || e->curstate.rendermode == kRenderTransAdd )
			GX_Cull( XASH_CULL_BACK );

		if( e->curstate.rendermode == kRenderTransAdd || e->curstate.rendermode == kRenderGlow )
			GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR );

		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	}
}

void DrawDecalsBatch( void )
{
	if( !tr.num_draw_decals )
		return;

	cl_entity_t *e = RI.currententity;
	Assert( e != NULL );

	if( e->curstate.rendermode != kRenderTransTexture )
	{
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
	}

	if( e->curstate.rendermode == kRenderTransTexture || e->curstate.rendermode == kRenderTransAdd )
		GX_Cull( XASH_CULL_NONE );

	if( gl_polyoffset.value )
		GX_PushPolygonOffset( -1.0f, -gl_polyoffset.value );

	for( int i = 0; i < tr.num_draw_decals; i++ )
	{
		DrawSurfaceDecals( tr.draw_decals[i], false, false );
	}

	if( e->curstate.rendermode != kRenderTransTexture )
	{
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	}

	if( gl_polyoffset.value )
		GX_PopPolygonOffset();

	if( e->curstate.rendermode == kRenderTransTexture || e->curstate.rendermode == kRenderTransAdd )
		GX_Cull( XASH_CULL_BACK );

	tr.num_draw_decals = 0;
}

static qboolean R_DecalUnProject( decal_t *pdecal, decallist_t *entry )
{
	if( !pdecal || !( pdecal->psurface ))
		return false;

	VectorCopy( pdecal->position, entry->position );
	entry->entityIndex = pdecal->entityIndex;

	if( pdecal->psurface->flags & SURF_PLANEBACK )
		VectorNegate( pdecal->psurface->plane->normal, entry->impactPlaneNormal );
	else VectorCopy( pdecal->psurface->plane->normal, entry->impactPlaneNormal );

	return true;
}

static int DecalListAdd( decallist_t *pList, int count )
{
	decallist_t *pdecal = pList + count;

	for( int i = 0; i < count; i++ )
	{
		if( !Q_strcmp( pdecal->name, pList[i].name ) &&  pdecal->entityIndex == pList[i].entityIndex )
		{
			vec3_t tmp;
			VectorSubtract( pdecal->position, pList[i].position, tmp );

			if( VectorLength( tmp ) < DECAL_OVERLAP_DISTANCE )
				return count;
		}
	}

	return count + 1;
}

static int DecalDepthCompare( const void *a, const void *b )
{
	const decallist_t *elem1 = (const decallist_t *)a;
	const decallist_t *elem2 = (const decallist_t *)b;

	if( elem1->depth > elem2->depth )
		return 1;
	if( elem1->depth < elem2->depth )
		return -1;

	return 0;
}

int R_CreateDecalList( decallist_t *pList )
{
	int total = 0;

	if( WORLDMODEL )
	{
		for( int i = 0; i < MAX_RENDER_DECALS; i++ )
		{
			decal_t *decal = &gDecalPool[i];

			if( decal->psurface == NULL || FBitSet( decal->flags, FDECAL_DONTSAVE ))
				 continue;

			int depth = 0;
			decal_t *pdecals = decal->psurface->pdecals;

			while( pdecals && pdecals != decal )
			{
				depth++;
				pdecals = pdecals->pnext;
			}

			pList[total].depth = depth;
			pList[total].flags = decal->flags;
			pList[total].scale = decal->scale;

			R_DecalUnProject( decal, &pList[total] );
			COM_FileBase( R_GetTexture( decal->texture )->name, pList[total].name, sizeof( pList[total].name ));

			total = DecalListAdd( pList, total );
		}

		if( gEngfuncs.drawFuncs->R_CreateStudioDecalList )
		{
			total += gEngfuncs.drawFuncs->R_CreateStudioDecalList( pList, total );
		}
	}

	qsort( pList, total, sizeof( decallist_t ), DecalDepthCompare );

	return total;
}

void R_DecalRemoveAll( int textureIndex )
{
	if( textureIndex < 0 || textureIndex >= MAX_TEXTURES )
		return;

	for( int i = 0; i < gDecalCount; i++ )
	{
		decal_t *pdecal = &gDecalPool[i];

		if( FBitSet( pdecal->flags, FDECAL_PERMANENT ))
			continue;

		if( !textureIndex || ( pdecal->texture == textureIndex ))
			R_DecalUnlink( pdecal );
	}
}

void R_EntityRemoveDecals( model_t *mod )
{
	if( !mod || mod->type != mod_brush )
		return;

	msurface_t *psurf = &mod->surfaces[mod->firstmodelsurface];
	for( int i = 0; i < mod->nummodelsurfaces; i++, psurf++ )
	{
		for( decal_t *p = psurf->pdecals; p; p = p->pnext )
			R_DecalUnlink( p );
	}
}

void R_ClearAllDecals( void )
{
	for( int i = 0; i < MAX_RENDER_DECALS; i++ )
	{
		decal_t *pdecal = &gDecalPool[i];
		R_DecalUnlink( pdecal );
	}

	if( gEngfuncs.drawFuncs->R_ClearStudioDecals )
	{
		gEngfuncs.drawFuncs->R_ClearStudioDecals();
	}
}