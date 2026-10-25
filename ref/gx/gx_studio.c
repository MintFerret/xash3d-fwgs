/*
gx_studio.c - studio model renderer (Wii GX native port)
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


/* Prototipos: las definiciones están más abajo en este archivo. */
static void R_StudioSetupRenderer( int rendermode );
static void R_StudioSetCullState( int cull );
static void R_StudioRenderShadow( int isprite, float *p1, float *p2, float *p3, float *p4 );


#include "xash3d_mathlib.h"
#include "const.h"
#include "r_studioint.h"
#include "triangleapi.h"
#include "studio.h"
#include "pm_local.h"

#define EVENT_CLIENT	5000
#define MAX_LOCALLIGHTS	4



typedef struct
{
	char		name[MAX_OSPATH];
	char		modelname[MAX_OSPATH];
	model_t		*model;
} player_model_t;

CVAR_DEFINE_AUTO( r_shadows, "0", 0, "draw ugly shadows" );

static const vec3_t hullcolor[8] =
{
{ 1.0f, 1.0f, 1.0f },
{ 1.0f, 0.5f, 0.5f },
{ 0.5f, 1.0f, 0.5f },
{ 1.0f, 1.0f, 0.5f },
{ 0.5f, 0.5f, 1.0f },
{ 1.0f, 0.5f, 1.0f },
{ 0.5f, 1.0f, 1.0f },
{ 1.0f, 1.0f, 1.0f },
};

typedef struct sortedmesh_s
{
	mstudiomesh_t	*mesh;
	int		flags;
} sortedmesh_t;

typedef struct
{
	double		time;
	double		frametime;
	int		framecount;
	qboolean		interpolate;
	int		rendermode;
	float		blend;

	matrix3x4		rotationmatrix;
	matrix3x4		bonestransform[MAXSTUDIOBONES];
	matrix3x4		lighttransform[MAXSTUDIOBONES];

	matrix3x4		worldtransform[MAXSTUDIOBONES];

	matrix3x4		cached_bonestransform[MAXSTUDIOBONES];
	matrix3x4		cached_lighttransform[MAXSTUDIOBONES];
	char		cached_bonenames[MAXSTUDIOBONES][32];
	int		cached_numbones;

	sortedmesh_t	meshes[MAXSTUDIOMESHES];
	vec3_t		verts[MAXSTUDIOVERTS];
	vec3_t		norms[MAXSTUDIOVERTS];

	float		ambientlight;
	float		shadelight;
	vec3_t		lightvec;
	vec3_t		lightspot;
	vec3_t		lightcolor;
	vec3_t		blightvec[MAXSTUDIOBONES];
	vec3_t		lightvalues[MAXSTUDIOVERTS];

	vec3_t		chrome_origin;
	vec2_t		chrome[MAXSTUDIOVERTS];
	vec3_t		chromeright[MAXSTUDIOBONES];
	vec3_t		chromeup[MAXSTUDIOBONES];
	int		chromeage[MAXSTUDIOBONES];

	int		normaltable[MAXSTUDIOVERTS];

	int		numlocallights;
	int		lightage[MAXSTUDIOBONES];
	dlight_t		*locallight[MAX_LOCALLIGHTS];
	int		locallightcolor[MAX_LOCALLIGHTS][3];
	vec4_t		lightpos[MAXSTUDIOVERTS][MAX_LOCALLIGHTS];
	vec3_t		lightbonepos[MAXSTUDIOBONES][MAX_LOCALLIGHTS];
	float		locallightR2[MAX_LOCALLIGHTS];

	player_model_t  player_models[MAX_CLIENTS];

	vec3_t			arrayverts[MAXSTUDIOVERTS];
	vec2_t			arraycoord[MAXSTUDIOVERTS];
	unsigned short	arrayelems[MAXSTUDIOVERTS*6];
	byte			arraycolor[MAXSTUDIOVERTS][4];
	uint			numverts;
	uint			numelems;
} studio_draw_state_t;

CVAR_DEFINE_AUTO( r_studio_sort_textures, "0", FCVAR_GLCONFIG, "change draw order for additive meshes" );
CVAR_DEFINE_AUTO( r_studio_drawelements, "1", FCVAR_GLCONFIG, "use glDrawElements for studiomodels" );
CVAR_DEFINE_AUTO( r_studio_builtin_renderer, "0", 0, "use built-in studio model renderer instead of the one provided by client library (debugging)" );
CVAR_DEFINE_AUTO( gx_studio_maxdist, "0", FCVAR_ARCHIVE, "do not draw monsters and players farther than this many units (0 = always draw)" );
static cvar_t			*cl_righthand = NULL;

static r_studio_interface_t	*pStudioDraw;
static studio_draw_state_t	g_studio;

static qboolean m_fDoRemap;
static mstudiomodel_t *m_pSubModel;
static mstudiobodyparts_t *m_pBodyPart;
static player_info_t *m_pPlayerInfo;
static studiohdr_t *m_pStudioHeader;
static float m_flGaitMovement;
static int g_nTopColor, g_nBottomColor;
static int g_nFaceFlags, g_nForceFaceFlags;

static void GX_SetupVtxFormatStudio( qboolean useColor, qboolean useTex )
{
	GX_SetChanVtxColor( useColor );
	GX_SetNumTevStages( 1 );
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

void R_StudioInit( void )
{
#if XASH_PSVITA
	gEngfuncs.Cvar_FullSet( "r_studio_drawelements", "0", FCVAR_READ_ONLY );
#endif

	Matrix3x4_LoadIdentity( g_studio.rotationmatrix );

	g_studio.interpolate = true;
	g_studio.framecount = 0;
	m_fDoRemap = false;
}

static void R_StudioSetupTimings( void )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
	{
		g_studio.time = gp_cl->time;
		g_studio.frametime = gp_cl->time - gp_cl->oldtime;
	}
	else
	{
		g_studio.time = gp_host->realtime;
		g_studio.frametime = gp_host->frametime;
	}
}

static qboolean R_AllowFlipViewModel( cl_entity_t *e )
{
	if( cl_righthand && cl_righthand->value > 0 )
	{
		if( e == tr.viewent )
			return true;
	}
	return false;
}

static qboolean R_StudioComputeBBox( vec3_t bbox[8] )
{
	cl_entity_t	*e = RI.currententity;

	if( !m_pStudioHeader )
		return false;

	vec3_t mins, maxs;
	if( !VectorIsNull( RI.currentmodel->mins ) && !VectorIsNull( RI.currentmodel->maxs ))
	{
		VectorCopy( RI.currentmodel->mins, mins );
		VectorCopy( RI.currentmodel->maxs, maxs );
	}
	else
	{
		ClearBounds( mins, maxs );
	}

	if( e->curstate.sequence < 0 || e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;

	AddPointToBounds( pseqdesc->bbmin, mins, maxs );
	AddPointToBounds( pseqdesc->bbmax, mins, maxs );
	vec3_t studio_mins, studio_maxs;
	ClearBounds( studio_mins, studio_maxs );

	for( int i = 0; i < 8; i++ )
	{
		vec3_t p1, p2;
  		p1[0] = ( i & 1 ) ? mins[0] : maxs[0];
  		p1[1] = ( i & 2 ) ? mins[1] : maxs[1];
  		p1[2] = ( i & 4 ) ? mins[2] : maxs[2];

		Matrix3x4_VectorTransform( g_studio.rotationmatrix, p1, p2 );
		AddPointToBounds( p2, studio_mins, studio_maxs );
		if( bbox ) VectorCopy( p2, bbox[i] );
	}

	if( !bbox && R_CullModel( e, studio_mins, studio_maxs ))
		return false;
	return true;
}

static void R_StudioComputeSkinMatrix( mstudioboneweight_t *boneweights, matrix3x4 result )
{
	int	numbones = 0;

	for( int i = 0; i < MAXSTUDIOBONEWEIGHTS; i++ )
	{
		if( boneweights->bone[i] != -1 )
			numbones++;
	}

	if( numbones == 4 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		vec4_t *boneMat2 = (vec4_t *)g_studio.worldtransform[boneweights->bone[2]];
		vec4_t *boneMat3 = (vec4_t *)g_studio.worldtransform[boneweights->bone[3]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flWeight2 = boneweights->weight[2] / 255.0f;
		float flWeight3 = boneweights->weight[3] / 255.0f;
		float flTotal = flWeight0 + flWeight1 + flWeight2 + flWeight3;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1 + boneMat2[0][0] * flWeight2 + boneMat3[0][0] * flWeight3;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1 + boneMat2[0][1] * flWeight2 + boneMat3[0][1] * flWeight3;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1 + boneMat2[0][2] * flWeight2 + boneMat3[0][2] * flWeight3;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1 + boneMat2[0][3] * flWeight2 + boneMat3[0][3] * flWeight3;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1 + boneMat2[1][0] * flWeight2 + boneMat3[1][0] * flWeight3;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1 + boneMat2[1][1] * flWeight2 + boneMat3[1][1] * flWeight3;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1 + boneMat2[1][2] * flWeight2 + boneMat3[1][2] * flWeight3;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1 + boneMat2[1][3] * flWeight2 + boneMat3[1][3] * flWeight3;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1 + boneMat2[2][0] * flWeight2 + boneMat3[2][0] * flWeight3;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1 + boneMat2[2][1] * flWeight2 + boneMat3[2][1] * flWeight3;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1 + boneMat2[2][2] * flWeight2 + boneMat3[2][2] * flWeight3;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1 + boneMat2[2][3] * flWeight2 + boneMat3[2][3] * flWeight3;
	}
	else if( numbones == 3 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		vec4_t *boneMat2 = (vec4_t *)g_studio.worldtransform[boneweights->bone[2]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flWeight2 = boneweights->weight[2] / 255.0f;
		float flTotal = flWeight0 + flWeight1 + flWeight2;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1 + boneMat2[0][0] * flWeight2;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1 + boneMat2[0][1] * flWeight2;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1 + boneMat2[0][2] * flWeight2;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1 + boneMat2[0][3] * flWeight2;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1 + boneMat2[1][0] * flWeight2;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1 + boneMat2[1][1] * flWeight2;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1 + boneMat2[1][2] * flWeight2;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1 + boneMat2[1][3] * flWeight2;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1 + boneMat2[2][0] * flWeight2;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1 + boneMat2[2][1] * flWeight2;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1 + boneMat2[2][2] * flWeight2;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1 + boneMat2[2][3] * flWeight2;
	}
	else if( numbones == 2 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flTotal = flWeight0 + flWeight1;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1;
	}
	else
	{
		Matrix3x4_Copy( result, g_studio.worldtransform[boneweights->bone[0]] );
	}
}

static cl_entity_t *pfnGetCurrentEntity( void )
{
	return RI.currententity;
}

player_info_t *pfnPlayerInfo( int index )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		index = -1;

	return gEngfuncs.pfnPlayerInfo( index );
}

static entity_state_t *R_StudioGetPlayerState( int index )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return &RI.currententity->curstate;

	return gEngfuncs.pfnGetPlayerState( index );
}

static void pfnGetEngineTimes( int *framecount, double *current, double *old )
{
	if( framecount ) *framecount = tr.realframecount;
	if( current ) *current = gp_cl->time;
	if( old ) *old =   gp_cl->oldtime;
}

static void pfnGetViewInfo( float *origin, float *upv, float *rightv, float *forwardv )
{
	if( origin ) VectorCopy( RI.rvp.vieworigin, origin );
	if( forwardv ) VectorCopy( RI.vforward, forwardv );
	if( rightv ) VectorCopy( RI.vright, rightv );
	if( upv ) VectorCopy( RI.vup, upv );
}

static void pfnGetModelCounters( int **s, int **a )
{
	*s = &g_studio.framecount;
	*a = &r_stats.c_studio_models_drawn;
}

static float ****pfnStudioGetBoneTransform( void )
{
	return (float ****)g_studio.bonestransform;
}

static float ****pfnStudioGetLightTransform( void )
{
	return (float ****)g_studio.lighttransform;
}

static float ***pfnStudioGetRotationMatrix( void )
{
	return (float ***)g_studio.rotationmatrix;
}

static void R_StudioPlayerBlend( mstudioseqdesc_t *pseqdesc, int *pBlend, float *pPitch )
{
	*pBlend = (*pPitch * 3.0f);

	if( *pBlend < pseqdesc->blendstart[0] )
	{
		*pPitch -= pseqdesc->blendstart[0] / 3.0f;
		*pBlend = 0;
	}
	else if( *pBlend > pseqdesc->blendend[0] )
	{
		*pPitch -= pseqdesc->blendend[0] / 3.0f;
		*pBlend = 255;
	}
	else
	{
		if( pseqdesc->blendend[0] - pseqdesc->blendstart[0] < 0.1f )
			*pBlend = 127;
		else *pBlend = 255 * (*pBlend - pseqdesc->blendstart[0]) / (pseqdesc->blendend[0] - pseqdesc->blendstart[0]);
		*pPitch = 0.0f;
	}
}

void R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles )
{
	float	f = 1.0f;

	if( g_studio.interpolate && ( time < e->curstate.animtime + 1.0f ) && ( e->curstate.animtime != e->latched.prevanimtime ))
		f = ( time - e->curstate.animtime ) / ( e->curstate.animtime - e->latched.prevanimtime );

	VectorLerp( e->latched.prevorigin, f, e->curstate.origin, origin );

	if( !VectorCompareEpsilon( e->curstate.angles, e->latched.prevangles, ON_EPSILON ))
	{
		vec4_t	q, q1, q2;
		AngleQuaternion( e->curstate.angles, q1, false );
		AngleQuaternion( e->latched.prevangles, q2, false );
		QuaternionSlerp( q2, q1, f, q );
		QuaternionAngle( q, angles );
	}
	else VectorCopy( e->curstate.angles, angles );
}

static void R_StudioSetUpTransform( cl_entity_t *e )
{
	vec3_t origin = Vec3( e->origin );
	vec3_t angles = Vec3( e->angles );

	if( e->curstate.movetype == MOVETYPE_STEP && !FBitSet( gp_host->features, ENGINE_COMPUTE_STUDIO_LERP ))
	{
		R_StudioLerpMovement( e, g_studio.time, origin, angles );
	}

	if( !FBitSet( gp_host->features, ENGINE_COMPENSATE_QUAKE_BUG ))
		angles[PITCH] = -angles[PITCH];

	if( e->player ) angles[PITCH] = 0.0f;

	Matrix3x4_CreateFromEntity( g_studio.rotationmatrix, angles, origin, 1.0f );

	if( tr.fFlipViewModel )
	{
		g_studio.rotationmatrix[0][1] = -g_studio.rotationmatrix[0][1];
		g_studio.rotationmatrix[1][1] = -g_studio.rotationmatrix[1][1];
		g_studio.rotationmatrix[2][1] = -g_studio.rotationmatrix[2][1];
	}
}

float R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time )
{
	double	dfdt, f;

	if( g_studio.interpolate )
	{
		if( time < e->curstate.animtime ) dfdt = 0.0;
		else dfdt = (time - e->curstate.animtime) * e->curstate.framerate * pseqdesc->fps;
	}
	else dfdt = 0;

	if( pseqdesc->numframes <= 1 ) f = 0.0;
	else f = (e->curstate.frame * (pseqdesc->numframes - 1)) / 256.0f;

	f += dfdt;

	if( pseqdesc->flags & STUDIO_LOOPING )
	{
		if( pseqdesc->numframes > 1 )
			f -= (int)(f / (pseqdesc->numframes - 1)) *  (pseqdesc->numframes - 1);
		if( f < 0 ) f += (pseqdesc->numframes - 1);
	}
	else
	{
		if( f >= pseqdesc->numframes - 1.001 )
			f = pseqdesc->numframes - 1.001;
		if( f < 0.0 )  f = 0.0;
	}
	return f;
}

static float R_StudioEstimateInterpolant( cl_entity_t *e )
{
	float	dadt = 1.0f;

	if( g_studio.interpolate && ( e->curstate.animtime >= e->latched.prevanimtime + 0.01f ))
	{
		dadt = ( g_studio.time - e->curstate.animtime ) / 0.1f;
		if( dadt > 2.0f ) dadt = 2.0f;
	}

	return dadt;
}

static void R_StudioFxTransform( cl_entity_t *ent, matrix3x4 transform )
{
	switch( ent->curstate.renderfx )
	{
	case kRenderFxDistort:
	case kRenderFxHologram:
		if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			int	axis = gEngfuncs.COM_RandomLong( 0, 1 );
			if( axis == 1 ) axis = 2;
			VectorScale( transform[axis], gEngfuncs.COM_RandomFloat( 1.0f, 1.484f ), transform[axis] );
		}
		else if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			int	axis = gEngfuncs.COM_RandomLong( 0, 1 );
			if( axis == 1 ) axis = 2;
			float offset = gEngfuncs.COM_RandomFloat( -10.0f, 10.0f );
			transform[gEngfuncs.COM_RandomLong( 0, 2 )][3] += offset;
		}
		break;
	case kRenderFxExplode:
		{
			float scale = 1.0f + ( g_studio.time - ent->curstate.animtime ) * 10.0f;
			if( scale > 2.0f ) scale = 2.0f;
			transform[0][1] *= scale;
			transform[1][1] *= scale;
			transform[2][1] *= scale;
		}
		break;
	}
}

static void R_StudioCalcBoneAdj( float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	mstudiobonecontroller_t	*pbonecontroller = (mstudiobonecontroller_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bonecontrollerindex);

	for( int j = 0; j < m_pStudioHeader->numbonecontrollers; j++ )
	{
		float value = 0.0f;
		int i = pbonecontroller[j].index;

		if( i == STUDIO_MOUTH )
		{
			value = (float)mouthopen / 64.0f;
			value = bound( 0.0f, value, 1.0f );
			value = (1.0f - value) * pbonecontroller[j].start + value * pbonecontroller[j].end;
		}
		else if( i < 4 )
		{
			if( FBitSet( pbonecontroller[j].type, STUDIO_RLOOP ))
			{
				if( abs( pcontroller1[i] - pcontroller2[i] ) > 128 )
				{
					int a = (pcontroller1[i] + 128) % 256;
					int b = (pcontroller2[i] + 128) % 256;
					value = (( a * dadt ) + ( b * ( 1.0f - dadt )) - 128) * (360.0f / 256.0f) + pbonecontroller[j].start;
				}
				else
				{
					value = ((pcontroller1[i] * dadt + (pcontroller2[i]) * (1.0f - dadt))) * (360.0f / 256.0f) + pbonecontroller[j].start;
				}
			}
			else
			{
				value = (pcontroller1[i] * dadt + pcontroller2[i] * (1.0f - dadt)) / 255.0f;
				value = bound( 0.0f, value, 1.0f );
				value = (1.0f - value) * pbonecontroller[j].start + value * pbonecontroller[j].end;
			}
		}

		switch( pbonecontroller[j].type & STUDIO_TYPES )
		{
		case STUDIO_XR:
		case STUDIO_YR:
		case STUDIO_ZR:
			adj[j] = DEG2RAD( value );
			break;
		case STUDIO_X:
		case STUDIO_Y:
		case STUDIO_Z:
			adj[j] = value;
			break;
		}
	}
}

static void R_StudioCalcRotations( cl_entity_t *e, float pos[][3], vec4_t *q, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, float f )
{
	float adj[MAXSTUDIOCONTROLLERS];

	if( f > pseqdesc->numframes - 1 )
	{
		f = 0.0f;
	}
	else if( f < -0.01f )
	{
		f = -0.01f;
	}

	int frame = (int)f;
	float dadt = R_StudioEstimateInterpolant( e );
	float s = (f - frame);

	mstudiobone_t *pbone = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	R_StudioCalcBoneAdj( dadt, adj, e->curstate.controller, e->latched.prevcontroller, e->mouth.mouthopen );

	for( int i = 0; i < m_pStudioHeader->numbones; i++, pbone++, panim++ )
		R_StudioCalcBones( frame, s, pbone, panim, adj, pos[i], q[i] );

	if( pseqdesc->motiontype & STUDIO_X ) pos[pseqdesc->motionbone][0] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Y ) pos[pseqdesc->motionbone][1] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Z ) pos[pseqdesc->motionbone][2] = 0.0f;
}

static void R_StudioMergeBones( cl_entity_t *e, model_t *m_pSubModel )
{
	static vec4_t	q[MAXSTUDIOBONES];
	static float	pos[MAXSTUDIOBONES][3];

	if( e->curstate.sequence >=  m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	float f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	mstudioanim_t *panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, m_pSubModel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		int j;
		for( j = 0; j < g_studio.cached_numbones; j++ )
		{
			if( !Q_stricmp( pbones[i].name, g_studio.cached_bonenames[j] ))
			{
				Matrix3x4_Copy( g_studio.bonestransform[i], g_studio.cached_bonestransform[j] );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.cached_lighttransform[j] );
				break;
			}
		}

		if( j >= g_studio.cached_numbones )
		{
			matrix3x4 bonematrix;
			Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );
			if( pbones[i].parent == -1 )
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );

				R_StudioFxTransform( e, g_studio.bonestransform[i] );
			}
			else
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
				Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
			}
		}
	}
}

static void R_StudioSetupBones( cl_entity_t *e )
{
	static vec3_t	pos[MAXSTUDIOBONES];
	static vec4_t	q[MAXSTUDIOBONES];
	static vec3_t	pos2[MAXSTUDIOBONES];
	static vec4_t	q2[MAXSTUDIOBONES];
	static vec3_t	pos3[MAXSTUDIOBONES];
	static vec4_t	q3[MAXSTUDIOBONES];
	static vec3_t	pos4[MAXSTUDIOBONES];
	static vec4_t	q4[MAXSTUDIOBONES];

	if( e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	float f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	mstudioanim_t *panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );

	if( pseqdesc->numblends > 1 )
	{
		panim += m_pStudioHeader->numbones;
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, f );

		float dadt = R_StudioEstimateInterpolant( e );
		float s = (e->curstate.blending[0] * dadt + e->latched.prevblending[0] * (1.0f - dadt)) / 255.0f;

		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q2, pos2, s );

		if( pseqdesc->numblends == 4 )
		{
			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, f );

			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, f );

			s = (e->curstate.blending[0] * dadt + e->latched.prevblending[0] * (1.0f - dadt)) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q3, pos3, q4, pos4, s );

			s = (e->curstate.blending[1] * dadt + e->latched.prevblending[1] * (1.0f - dadt)) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q3, pos3, s );
		}
	}

	if( g_studio.interpolate && e->latched.sequencetime && ( e->latched.sequencetime + 0.2f > g_studio.time ) && ( e->latched.prevsequence < m_pStudioHeader->numseq ))
	{
		static vec3_t	pos1b[MAXSTUDIOBONES];
		static vec4_t	q1b[MAXSTUDIOBONES];

		pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->latched.prevsequence;
		panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );

		R_StudioCalcRotations( e, pos1b, q1b, pseqdesc, panim, e->latched.prevframe );

		if( pseqdesc->numblends > 1 )
		{
			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, e->latched.prevframe );

			float s = (e->latched.prevseqblending[0]) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q1b, pos1b, q2, pos2, s );

			if( pseqdesc->numblends == 4 )
			{
				panim += m_pStudioHeader->numbones;
				R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, e->latched.prevframe );

				panim += m_pStudioHeader->numbones;
				R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, e->latched.prevframe );

				s = (e->latched.prevseqblending[0]) / 255.0f;
				R_StudioSlerpBones( m_pStudioHeader->numbones, q3, pos3, q4, pos4, s );

				s = (e->latched.prevseqblending[1]) / 255.0f;
				R_StudioSlerpBones( m_pStudioHeader->numbones, q1b, pos1b, q3, pos3, s );
			}
		}

		float s = 1.0f - ( g_studio.time - e->latched.sequencetime ) / 0.2f;
		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q1b, pos1b, s );
	}
	else
	{
		e->latched.prevframe = f;
	}

	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	if( m_pPlayerInfo && m_pPlayerInfo->gaitsequence != 0 )
	{
		qboolean	copy_bones = true;

		if( m_pPlayerInfo->gaitsequence >= m_pStudioHeader->numseq )
			m_pPlayerInfo->gaitsequence = 0;

		pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + m_pPlayerInfo->gaitsequence;

		panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, m_pPlayerInfo->gaitframe );

		for( int i = 0; i < m_pStudioHeader->numbones; i++ )
		{
			if( !Q_strcmp( pbones[i].name, "Bip01 Spine" ))
				copy_bones = false;
			else if( !Q_strcmp( pbones[pbones[i].parent].name, "Bip01 Pelvis" ))
				copy_bones = true;

			if( !copy_bones ) continue;

			VectorCopy( pos2[i], pos[i] );
			Vector4Copy( q2[i], q[i] );
		}
	}

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		matrix3x4 bonematrix;
		Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );

		if( pbones[i].parent == -1 )
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
			Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );

			R_StudioFxTransform( e, g_studio.bonestransform[i] );
		}
		else
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
			Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
		}
	}
}

static void R_StudioSaveBones( void )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	g_studio.cached_numbones = m_pStudioHeader->numbones;

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_Copy( g_studio.cached_bonestransform[i], g_studio.bonestransform[i] );
		Matrix3x4_Copy( g_studio.cached_lighttransform[i], g_studio.lighttransform[i] );
		Q_strncpy( g_studio.cached_bonenames[i], pbones[i].name, 32 );
	}
}

static void R_StudioBuildNormalTable( void )
{
	cl_entity_t	*e = RI.currententity;

	Assert( m_pSubModel != NULL );

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
		g_studio.chromeage[i] = 0;

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		g_studio.normaltable[i] = -1;

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + j;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		int i;
		while(( i = *( ptricmds++ )))
		{
			if( i < 0 ) i = -i;

			for( ; i > 0; i--, ptricmds += 4 )
			{
				if( g_studio.normaltable[ptricmds[0]] < 0 )
					g_studio.normaltable[ptricmds[0]] = ptricmds[1];
			}
		}
	}

	g_studio.chrome_origin[0] = cos( r_glowshellfreq->value * g_studio.time ) * 4000.0f;
	g_studio.chrome_origin[1] = sin( r_glowshellfreq->value * g_studio.time ) * 4000.0f;
	g_studio.chrome_origin[2] = cos( r_glowshellfreq->value * g_studio.time * 0.33f ) * 4000.0f;

	if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
		TriColor4ub( e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, 255 );
	else TriColor4ub( 255, 255, 255, 255 );
}

static void R_StudioGenerateNormals( void )
{
	Assert( m_pSubModel != NULL );

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		VectorClear( g_studio.norms[i] );

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + j;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		int i;
		while(( i = *( ptricmds++ )))
		{
			if( i < 0 )
			{
				i = -i;

				if( i > 2 )
				{
					vec3_t e0, e1, norm;
					int v0 = ptricmds[0]; ptricmds += 4;
					int v1 = ptricmds[0]; ptricmds += 4;

					for( i -= 2; i > 0; i--, ptricmds += 4 )
					{
						int v2 = ptricmds[0];

						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						v1 = v2;
					}
				}
				else
				{
					ptricmds += i;
				}
			}
			else
			{
				if( i > 2 )
				{
					qboolean	odd = false;
					vec3_t e0, e1, norm;
					int v0 = ptricmds[0]; ptricmds += 4;
					int v1 = ptricmds[0]; ptricmds += 4;

					for( i -= 2; i > 0; i--, ptricmds += 4 )
					{
						int v2 = ptricmds[0];

						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						if( odd ) v1 = v2;
						else v0 = v2;

						odd = !odd;
					}
				}
				else
				{
					ptricmds += i;
				}
			}
		}
	}

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		VectorNormalize( g_studio.norms[i] );
}

static void R_StudioSetupChrome( float *pchrome, int bone, vec3_t normal )
{
	if( g_studio.chromeage[bone] != g_studio.framecount )
	{
		vec3_t	chromeupvec;
		vec3_t	chromerightvec;
		vec3_t	tmp;

		VectorNegate( g_studio.chrome_origin, tmp );
		tmp[0] += g_studio.lighttransform[bone][0][3];
		tmp[1] += g_studio.lighttransform[bone][1][3];
		tmp[2] += g_studio.lighttransform[bone][2][3];

		VectorNormalize( tmp );
		CrossProduct( tmp, RI.vright, chromeupvec );
		VectorNormalize( chromeupvec );
		CrossProduct( chromeupvec, tmp, chromerightvec );
		VectorNormalize( chromerightvec );

		Matrix3x4_VectorIRotate( g_studio.lighttransform[bone], chromeupvec, g_studio.chromeup[bone] );
		Matrix3x4_VectorIRotate( g_studio.lighttransform[bone], chromerightvec, g_studio.chromeright[bone] );

		g_studio.chromeage[bone] = g_studio.framecount;
	}

	float n = DotProduct( normal, g_studio.chromeright[bone] );
	pchrome[0] = (n + 1.0f) * 32.0f;

	n = DotProduct( normal, g_studio.chromeup[bone] );
	pchrome[1] = (n + 1.0f) * 32.0f;
}

static void R_StudioCalcAttachments( void )
{
	mstudioattachment_t *pAtt = (mstudioattachment_t *)((byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex);

	for( int i = 0; i < Q_min( MAXSTUDIOATTACHMENTS, m_pStudioHeader->numattachments ); i++ )
	{
		Matrix3x4_VectorTransform( g_studio.lighttransform[pAtt[i].bone], pAtt[i].org, RI.currententity->attachment[i] );
	}
}

static void R_StudioSetupModel( int bodypart, void **ppbodypart, void **ppsubmodel )
{
	if( bodypart > m_pStudioHeader->numbodyparts )
		bodypart = 0;

	m_pBodyPart = (mstudiobodyparts_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bodypartindex) + bodypart;

	int index = RI.currententity->curstate.body / m_pBodyPart->base;
	index = index % m_pBodyPart->nummodels;

	m_pSubModel = (mstudiomodel_t *)((byte *)m_pStudioHeader + m_pBodyPart->modelindex) + index;

	if( ppbodypart ) *ppbodypart = m_pBodyPart;
	if( ppsubmodel ) *ppsubmodel = m_pSubModel;
}

static int R_StudioCheckBBox( void )
{
	if( !RI.currententity || !RI.currentmodel )
		return false;

	// con muchos enemigos lejanos cada modelo cuesta huesos + vertices + GX:
	// no dibujar los que estan mas alla de la distancia configurada
	if( gx_studio_maxdist.value > 64.0f && RI.currententity != tr.viewent )
	{
		vec3_t d;

		VectorSubtract( RI.currententity->origin, RI.rvp.vieworigin, d );
		if( DotProduct( d, d ) > gx_studio_maxdist.value * gx_studio_maxdist.value )
			return false;
	}

	return R_StudioComputeBBox( NULL );
}

static void R_StudioEntityLight( alight_t *lightinfo )
{
	float		lstrength[MAX_LOCALLIGHTS];
	cl_entity_t	*ent = RI.currententity;

	g_studio.numlocallights = 0;

	if( !ent || !r_dynamic->value )
		return;

	for( int i = 0; i < MAX_LOCALLIGHTS; i++ )
		lstrength[i] = 0;

	vec3_t origin;
	Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );
	float dist2 = 1000000.0f;
	int k = 0;

	for( int lnum = 0; lnum < MAX_ELIGHTS; lnum++ )
	{
		dlight_t *el = &tr.elights[lnum];

		if( el->die < g_studio.time || el->radius <= 0.0f )
			continue;

		if(( el->key & 0xFFF ) == ent->index )
		{
			int	att = (el->key >> 12) & 0xF;

			if( att ) VectorCopy( ent->attachment[att], el->origin );
			else VectorCopy( ent->origin, el->origin );
		}

		vec3_t mid;
		VectorSubtract( origin, el->origin, mid );

		float f = DotProduct( mid, mid );
		float r2 = el->radius * el->radius;

		float minstrength;
		if( f > r2 ) minstrength = r2 / f;
		else minstrength = 1.0f;

		if( minstrength > 0.05f )
		{
			if( g_studio.numlocallights >= MAX_LOCALLIGHTS )
			{
				k = -1;
				for( int j = 0; j < g_studio.numlocallights; j++ )
				{
					if( lstrength[j] < dist2 && lstrength[j] < minstrength )
					{
						dist2 = lstrength[j];
						k = j;
					}
				}
			}
			else k = g_studio.numlocallights;

			if( k != -1 )
			{
				g_studio.locallightcolor[k][0] = LinearGammaTable( el->color.r << 2 );
				g_studio.locallightcolor[k][1] = LinearGammaTable( el->color.g << 2 );
				g_studio.locallightcolor[k][2] = LinearGammaTable( el->color.b << 2 );
				g_studio.locallightR2[k] = r2;
				g_studio.locallight[k] = el;
				lstrength[k] = minstrength;

				if( k >= g_studio.numlocallights )
					g_studio.numlocallights = k + 1;
			}
		}
	}
}

static void R_StudioSetupLighting( alight_t *plight )
{
	float	scale = 1.0f;

	if( !m_pStudioHeader || !plight )
		return;

	if( RI.currententity != NULL )
		scale = RI.currententity->curstate.scale;

	g_studio.ambientlight = plight->ambientlight;
	g_studio.shadelight = plight->shadelight;
	VectorCopy( plight->plightvec, g_studio.lightvec );

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_VectorIRotate( g_studio.lighttransform[i], plight->plightvec, g_studio.blightvec[i] );
		if( scale > 1.0f ) VectorNormalize( g_studio.blightvec[i] );
	}

	VectorCopy( plight->color, g_studio.lightcolor );
}

static void R_StudioLighting( float *lv, int bone, int flags, vec3_t normal )
{
	if( FBitSet( flags, STUDIO_NF_FULLBRIGHT ))
	{
		*lv = 1.0f;
		return;
	}

	float illum = g_studio.ambientlight;

	if( FBitSet( flags, STUDIO_NF_FLATSHADE ))
	{
		illum += g_studio.shadelight * 0.8f;
	}
	else
	{
		float lightcos;
		if( bone != -1 ) lightcos = DotProduct( normal, g_studio.blightvec[bone] );
		else lightcos = DotProduct( normal, g_studio.lightvec );
		if( lightcos > 1.0f ) lightcos = 1.0f;

		illum += g_studio.shadelight;

		float r = SHADE_LAMBERT;

 		if( r <= 1.0f )
		{
			r += 1.0f;
			lightcos = (( r - 1.0f ) - lightcos) / r;
			if( lightcos > 0.0f )
				illum += g_studio.shadelight * lightcos;
		}
		else
		{
			lightcos = (lightcos + ( r - 1.0f )) / r;
			if( lightcos > 0.0f )
				illum -= g_studio.shadelight * lightcos;
		}

		illum = Q_max( illum, 0.0f );
	}

	illum = Q_min( illum, 255.0f );

	*lv = LightToTexGamma( illum * 4 ) / 1023.0f;
}

static void R_LightLambert( vec4_t light[MAX_LOCALLIGHTS], const vec3_t normal, const vec3_t color, byte *out )
{
	if( !g_studio.numlocallights )
	{
		VectorScale( color, 255.0f, out );
		return;
	}

	vec3_t finalLight = { 0, 0, 0 };

	for( int i = 0; i < g_studio.numlocallights; i++ )
	{
		float r = DotProduct( normal, light[i] );
		if( likely( !tr.fFlipViewModel ))
			r = -r;

		if( r > 0.0f )
		{
			if( light[i][3] == 0.0f )
			{
				float r2 = DotProduct( light[i], light[i] );

				if( r2 > 0.0f )
					light[i][3] = g_studio.locallightR2[i] / ( r2 * sqrt( r2 ));
				else light[i][3] = 0.0001f;
			}

			float temp = r * light[i][3];

			vec3_t localLight;
			VectorAddScalar( g_studio.locallightcolor[i], temp, localLight );
			VectorAdd( finalLight, localLight, finalLight );
		}
	}

	if( !VectorIsNull( finalLight ))
	{
		for( int i = 0; i < 3; i++ )
		{
			float c = finalLight[i] + LinearGammaTable( color[i] * 1023.0f );

			if( c > 1023.0f )
				out[i] = 255;
			else
				out[i] = ScreenGammaTable( c ) >> 2;
		}
	}
	else
	{
		VectorScale( color, 255.0f, out );
	}
}

static void R_StudioSetColorArray( short *ptricmds, vec3_t *pstudionorms, byte *color )
{
	float	*lv = (float *)g_studio.lightvalues[ptricmds[1]];

	color[3] = tr.blend * 255;

	if( likely( !g_studio.numlocallights ))
	{
		const byte *c = g_studio.arraycolor[ptricmds[1]];

		color[0] = c[0];
		color[1] = c[1];
		color[2] = c[2];
		return;
	}

	R_LightLambert( g_studio.lightpos[ptricmds[0]], pstudionorms[ptricmds[1]], lv, color );
}

static void R_StudioDrawNormalMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t )
{
	int	i;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(-i) );
			i = -i;
		}
		else
		{
			GX_Begin( GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)i );
		}

		for( ; i > 0; i--, ptricmds += 4 )
		{
			byte color[4];
			R_StudioSetColorArray( ptricmds, pstudionorms, color );

			GX_Position3f32( g_studio.verts[ptricmds[0]][0],
			                 g_studio.verts[ptricmds[0]][1],
			                 g_studio.verts[ptricmds[0]][2] );
			GX_Color4u8( color[0], color[1], color[2], color[3] );
			GX_TexCoord2f32( ptricmds[2] * s, ptricmds[3] * t );
		}

		GX_End();
	}
}

static void R_StudioDrawFloatMesh( short *ptricmds, vec3_t *pstudionorms )
{
	int	i;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(-i) );
			i = -i;
		}
		else
		{
			GX_Begin( GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)i );
		}

		for( ; i > 0; i--, ptricmds += 4 )
		{
			byte color[4];
			R_StudioSetColorArray( ptricmds, pstudionorms, color );

			GX_Position3f32( g_studio.verts[ptricmds[0]][0],
			                 g_studio.verts[ptricmds[0]][1],
			                 g_studio.verts[ptricmds[0]][2] );
			GX_Color4u8( color[0], color[1], color[2], color[3] );
			GX_TexCoord2f32( HalfToFloat( ptricmds[2] ), HalfToFloat( ptricmds[3] ) );
		}

		GX_End();
	}
}

static void R_StudioDrawChromeMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t, float scale )
{
	int	i;
	qboolean	glowShell = (scale > 0.0f) ? true : false;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(-i) );
			i = -i;
		}
		else
		{
			GX_Begin( GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)i );
		}

		for( ; i > 0; i--, ptricmds += 4 )
		{
			int idx;
			if( glowShell )
			{
				idx = g_studio.normaltable[ptricmds[0]];
				float *av = g_studio.verts[ptricmds[0]];
				float *lv = g_studio.norms[ptricmds[0]];
				vec3_t vert;
				VectorMA( av, scale, lv, vert );

				GX_Position3f32( vert[0], vert[1], vert[2] );
				GX_Color4u8( RI.currententity->curstate.rendercolor.r,
				             RI.currententity->curstate.rendercolor.g,
				             RI.currententity->curstate.rendercolor.b, 255 );
				GX_TexCoord2f32( g_studio.chrome[idx][0] * s, g_studio.chrome[idx][1] * t );
			}
			else
			{
				idx = ptricmds[1];
				byte color[4];
				R_StudioSetColorArray( ptricmds, pstudionorms, color );

				GX_Position3f32( g_studio.verts[ptricmds[0]][0],
				                 g_studio.verts[ptricmds[0]][1],
				                 g_studio.verts[ptricmds[0]][2] );
				GX_Color4u8( color[0], color[1], color[2], color[3] );
				GX_TexCoord2f32( g_studio.chrome[idx][0] * s, g_studio.chrome[idx][1] * t );
			}
		}

		GX_End();
	}
}

/*
===============
R_StudioSetupSkin

Bindea la textura de la skin del mesh (antes era un stub vacio: los modelos
usaban la ultima textura que hubiera quedado bindeada).
===============
*/
static void R_StudioSetupSkin( studiohdr_t *ptexturehdr, int index )
{
	mstudiotexture_t *ptexture = NULL;

	if( FBitSet( g_nForceFaceFlags, STUDIO_NF_CHROME ))
		return;

	if( ptexturehdr == NULL )
		return;

	/* el cliente puede no haber llamado a StudioRemapColors */
	if( m_fDoRemap ) ptexture = gEngfuncs.CL_GetRemapInfoForEntity( RI.currententity )->ptexture;
	if( !ptexture ) ptexture = (mstudiotexture_t *)((byte *)ptexturehdr + ptexturehdr->textureindex );

	GX_Bind( XASH_TEXTURE0, ptexture[index].index );
}

/* No cambia el estado global del studio renderer */
mstudiotexture_t *R_StudioGetTexture( cl_entity_t *e )
{
	studiohdr_t *phdr;
	studiohdr_t *thdr;
	mstudiotexture_t *ptexture;

	if(( phdr = gEngfuncs.Mod_Extradata( mod_studio, e->model )) == NULL )
		return NULL;

	thdr = m_pStudioHeader;
	if( !thdr ) return NULL;

	if( m_fDoRemap ) ptexture = gEngfuncs.CL_GetRemapInfoForEntity( e )->ptexture;
	else ptexture = (mstudiotexture_t *)((byte *)thdr + thdr->textureindex );

	return ptexture;
}

static void R_StudioSubmitMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t, float shellscale, int tesslevel )
{
	if( tesslevel > 0 )
	{
	}
	else
	{
		if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
			R_StudioDrawChromeMesh( ptricmds, pstudionorms, s, t, shellscale );
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_UV_COORDS ))
			R_StudioDrawFloatMesh( ptricmds, pstudionorms );
		else
			R_StudioDrawNormalMesh( ptricmds, pstudionorms, s, t );
	}
}

static void R_StudioDrawPoints( void )
{
	float		shellscale = 0.0f;
	qboolean		need_sort = false;

	if( !m_pStudioHeader ) return;

	g_studio.numverts = 0;

	int m_skinnum = RI.currententity->curstate.skin;
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)m_pStudioHeader + m_pStudioHeader->textureindex);
	byte *pvertbone = ((byte *)m_pStudioHeader + m_pSubModel->vertinfoindex);
	byte *pnormbone = ((byte *)m_pStudioHeader + m_pSubModel->norminfoindex);

	mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex);
	vec3_t *pstudioverts = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->vertindex);
	vec3_t *pstudionorms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex);

	short *pskinref = (short *)((byte *)m_pStudioHeader + m_pStudioHeader->skinindex);
	if( m_skinnum > 0 && m_skinnum < m_pStudioHeader->numskinfamilies )
		pskinref += (m_skinnum * m_pStudioHeader->numskinref);

	const int numverts = m_pSubModel->numverts;
	const int numnorms = m_pSubModel->numnorms;

	if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ) && m_pSubModel->blendvertinfoindex != 0 && m_pSubModel->blendnorminfoindex != 0 )
	{
		mstudioboneweight_t	*pvertweight = (mstudioboneweight_t *)((byte *)m_pStudioHeader + m_pSubModel->blendvertinfoindex);
		mstudioboneweight_t	*pnormweight = (mstudioboneweight_t *)((byte *)m_pStudioHeader + m_pSubModel->blendnorminfoindex);
		matrix3x4		skinMat;

		for( int i = 0; i < numverts; i++ )
		{
			R_StudioComputeSkinMatrix( &pvertweight[i], skinMat );
			Matrix3x4_VectorTransform( skinMat, pstudioverts[i], g_studio.verts[i] );
		}

		for( int i = 0; i < numnorms; i++ )
		{
			R_StudioComputeSkinMatrix( &pnormweight[i], skinMat );
			Matrix3x4_VectorRotate( skinMat, pstudionorms[i], g_studio.norms[i] );
		}
	}
	else
	{
		for( int i = 0; i < numverts; i++ )
			Matrix3x4_VectorTransform( g_studio.bonestransform[pvertbone[i]], pstudioverts[i], g_studio.verts[i] );
	}

	if( RI.currententity->curstate.renderfx == kRenderFxGlowShell )
	{
		float factor = (1.0f / 128.0f);
		shellscale = Q_max( factor, RI.currententity->curstate.renderamt * factor );
		R_StudioBuildNormalTable();
		R_StudioGenerateNormals();
	}

	int k = 0;
	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		g_nFaceFlags = ptexture[pskinref[pmesh[j].skinref]].flags | g_nForceFaceFlags;

		g_studio.meshes[j].flags = g_nFaceFlags;
		g_studio.meshes[j].mesh = &pmesh[j];

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED|STUDIO_NF_ADDITIVE ))
			need_sort = true;

		if( RI.currententity->curstate.rendermode == kRenderTransAdd )
		{
			for( int i = 0; i < pmesh[j].numnorms; i++, k++, pstudionorms++, pnormbone++ )
			{
				if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
					R_StudioSetupChrome( g_studio.chrome[k], *pnormbone, (float *)pstudionorms );
				VectorSet( g_studio.lightvalues[k], tr.blend, tr.blend, tr.blend );
			}
		}
		else
		{
			for( int i = 0; i < pmesh[j].numnorms; i++, k++, pstudionorms++, pnormbone++ )
			{
				float lv_tmp;
				if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ))
					R_StudioLighting( &lv_tmp, -1, g_nFaceFlags, g_studio.norms[k] );
				else R_StudioLighting( &lv_tmp, *pnormbone, g_nFaceFlags, (float *)pstudionorms );

				if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
					R_StudioSetupChrome( g_studio.chrome[k], *pnormbone, (float *)pstudionorms );
				VectorScale( g_studio.lightcolor, lv_tmp, g_studio.lightvalues[k] );
			}
		}
	}

	// sin luces locales el color final depende solo de la normal: calcularlo una
	// vez por normal en vez de una vez por cada vertice de cada triangulo
	if( !g_studio.numlocallights )
	{
		for( int n = 0; n < k; n++ )
		{
			g_studio.arraycolor[n][0] = (byte)( g_studio.lightvalues[n][0] * 255.0f );
			g_studio.arraycolor[n][1] = (byte)( g_studio.lightvalues[n][1] * 255.0f );
			g_studio.arraycolor[n][2] = (byte)( g_studio.lightvalues[n][2] * 255.0f );
		}
	}

	if( r_studio_sort_textures.value && need_sort )
	{
		/* qsort con R_StudioMeshCompare eliminado: función no existe en GX */
	}

	pstudionorms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex);

	// El estado de culling NO se puede heredar: sprites, particulas, beams, decals
	// y TriAPI del cliente lo dejan en NONE/FRONT segun el caso, y el viewmodel
	// se veia con las caras traseras (transparente) cuando heredaba NONE.
	// Se fija siempre. Un viewmodel espejado (cl_righthand) invierte el
	// winding, asi que culla la cara contraria en vez de desactivar el culling.
	if( R_AllowFlipViewModel( RI.currententity ))
	{
		tr.fFlipViewModel = true;
		GX_Cull( XASH_CULL_FRONT );
	}
	else
	{
		tr.fFlipViewModel = false;
		GX_Cull( XASH_CULL_BACK );
	}

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		float	oldblend = tr.blend;

		pmesh = g_studio.meshes[j].mesh;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		g_nFaceFlags = ptexture[pskinref[pmesh->skinref]].flags | g_nForceFaceFlags;

		float s = 1.0f / (float)ptexture[pskinref[pmesh->skinref]].width;
		float t = 1.0f / (float)ptexture[pskinref[pmesh->skinref]].height;

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
		{
			GX_SetAlphaCompare( GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0 );
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
			if( R_ModelOpaque( RI.currententity->curstate.rendermode ))
				tr.blend = 1.0f;
		}
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ))
		{
			if( R_ModelOpaque( RI.currententity->curstate.rendermode ))
			{
				GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR );
				GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
				R_AllowFog( false );
			}
			else
			{
				GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR );
			}
		}

		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

		R_StudioSetupSkin( m_pStudioHeader, pskinref[pmesh->skinref] );

		GX_SetupVtxFormatStudio( true, true );

		R_StudioSubmitMesh( ptricmds, pstudionorms, s, t, shellscale, 0 );

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
		{
			GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		}
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ) && R_ModelOpaque( RI.currententity->curstate.rendermode ))
		{
			GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
			GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
			R_AllowFog( true );
		}

		r_stats.c_studio_polys += pmesh->numtris;
		tr.blend = oldblend;
	}
}

static void R_StudioDrawHulls( void )
{
	float alpha = ( r_drawentities->value == 4 ) ? 0.5f : 1.0f;

	GX_Bind( XASH_TEXTURE0, tr.whiteTexture );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetupVtxFormatStudio( true, true );

	for( int i = 0; i < m_pStudioHeader->numhitboxes; i++ )
	{
		mstudiobbox_t	*pbbox = (mstudiobbox_t *)((byte *)m_pStudioHeader + m_pStudioHeader->hitboxindex);
		vec3_t		tmp, p[8];

		for( int j = 0; j < 8; j++ )
		{
			tmp[0] = (j & 1) ? pbbox[i].bbmin[0] : pbbox[i].bbmax[0];
			tmp[1] = (j & 2) ? pbbox[i].bbmin[1] : pbbox[i].bbmax[1];
			tmp[2] = (j & 4) ? pbbox[i].bbmin[2] : pbbox[i].bbmax[2];

			Matrix3x4_VectorTransform( g_studio.bonestransform[pbbox[i].bone], tmp, p[j] );
		}

		int j = (pbbox[i].group % 8);

		TriBegin( TRI_QUADS );
		TriColor4f( hullcolor[j][0], hullcolor[j][1], hullcolor[j][2], alpha );

		for( j = 0; j < 6; j++ )
		{
			float lv;
			VectorClear( tmp );
			tmp[j % 3] = (j < 3) ? 1.0f : -1.0f;
			R_StudioLighting( &lv, pbbox[i].bone, 0, tmp );

			TriBrightness( lv );
			TriVertex3fv( p[boxpnt[j][0]] );
			TriVertex3fv( p[boxpnt[j][1]] );
			TriVertex3fv( p[boxpnt[j][2]] );
			TriVertex3fv( p[boxpnt[j][3]] );
		}
		TriEnd();
	}
}

static void R_StudioDrawAbsBBox( void )
{
	vec3_t	p[8], tmp;

	if( RI.currententity == tr.viewent )
		return;

	if( !R_StudioComputeBBox( p ))
		return;

	GX_Bind( XASH_TEXTURE0, tr.whiteTexture );
	TriColor4f( 0.5f, 0.5f, 1.0f, 0.5f );
	TriRenderMode( kRenderTransAdd );

	TriBegin( TRI_QUADS );
	for( int i = 0; i < 6; i++ )
	{
		float lv;
		VectorClear( tmp );
		tmp[i % 3] = (i < 3) ? 1.0f : -1.0f;
		R_StudioLighting( &lv, -1, 0, tmp );

		TriBrightness( lv );
		TriVertex3fv( p[boxpnt[i][0]] );
		TriVertex3fv( p[boxpnt[i][1]] );
		TriVertex3fv( p[boxpnt[i][2]] );
		TriVertex3fv( p[boxpnt[i][3]] );
	}
	TriEnd();
	TriRenderMode( kRenderNormal );
}

static void R_StudioDrawBones( void )
{
	mstudiobone_t	*pbones = (mstudiobone_t *) ((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	vec3_t		point;

	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
	GX_SetupVtxFormatStudio( true, false );

	GXColor col;

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		if( pbones[i].parent >= 0 )
		{
			GX_SetPointSize( 3.0f, GX_POINTS );
			col.r = 255; col.g = 178; col.b = 0; col.a = 255;
			GX_SetChanMatColor( GX_COLOR0A0, col );

			GX_Begin( GX_LINES, GX_VTXFMT0, 2 );
			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[pbones[i].parent], point );
			GX_Position3f32( point[0], point[1], point[2] );
			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[i], point );
			GX_Position3f32( point[0], point[1], point[2] );
			GX_End();

			col.r = 0; col.g = 0; col.b = 204; col.a = 255;
			GX_SetChanMatColor( GX_COLOR0A0, col );
			GX_Begin( GX_POINTS, GX_VTXFMT0, 1 );
			if( pbones[pbones[i].parent].parent != -1 )
			{
				Matrix3x4_OriginFromMatrix( g_studio.bonestransform[pbones[i].parent], point );
				GX_Position3f32( point[0], point[1], point[2] );
			}
			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[i], point );
			GX_Position3f32( point[0], point[1], point[2] );
			GX_End();
		}
		else
		{
			GX_SetPointSize( 5.0f, GX_POINTS );
			col.r = 204; col.g = 0; col.b = 0; col.a = 255;
			GX_SetChanMatColor( GX_COLOR0A0, col );
			GX_Begin( GX_POINTS, GX_VTXFMT0, 1 );
			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[i], point );
			GX_Position3f32( point[0], point[1], point[2] );
			GX_End();
		}
	}

	GX_SetPointSize( 1.0f, GX_POINTS );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
}

static void R_StudioDrawAttachments( void )
{
	GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
	GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
	GX_SetupVtxFormatStudio( true, false );

	GXColor col;

	for( int i = 0; i < m_pStudioHeader->numattachments; i++ )
	{
		vec3_t		v[4];

		mstudioattachment_t *pattachments = (mstudioattachment_t *)((byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex);
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].org, v[0] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[0], v[1] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[1], v[2] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[2], v[3] );

		col.r = 255; col.g = 0; col.b = 0; col.a = 255;
		GX_SetChanMatColor( GX_COLOR0A0, col );
		GX_Begin( GX_LINES, GX_VTXFMT0, 6 );
		GX_Position3f32( v[0][0], v[0][1], v[0][2] );
		col.r = 255; col.g = 255; col.b = 255; col.a = 255;
		GX_SetChanMatColor( GX_COLOR0A0, col );
		GX_Position3f32( v[1][0], v[1][1], v[1][2] );
		col.r = 255; col.g = 0; col.b = 0; col.a = 255;
		GX_SetChanMatColor( GX_COLOR0A0, col );
		GX_Position3f32( v[0][0], v[0][1], v[0][2] );
		col.r = 255; col.g = 255; col.b = 255; col.a = 255;
		GX_SetChanMatColor( GX_COLOR0A0, col );
		GX_Position3f32( v[2][0], v[2][1], v[2][2] );
		col.r = 255; col.g = 0; col.b = 0; col.a = 255;
		GX_SetChanMatColor( GX_COLOR0A0, col );
		GX_Position3f32( v[0][0], v[0][1], v[0][2] );
		col.r = 255; col.g = 255; col.b = 255; col.a = 255;
		GX_SetChanMatColor( GX_COLOR0A0, col );
		GX_Position3f32( v[3][0], v[3][1], v[3][2] );
		GX_End();

		GX_SetPointSize( 5.0f, GX_POINTS );
		col.r = 0; col.g = 255; col.b = 0; col.a = 255;
		GX_SetChanMatColor( GX_COLOR0A0, col );
		GX_Begin( GX_POINTS, GX_VTXFMT0, 1 );
		GX_Position3f32( v[0][0], v[0][1], v[0][2] );
		GX_End();
		GX_SetPointSize( 1.0f, GX_POINTS );
	}

	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
}

static void R_StudioSetRemapColors( int newTop, int newBottom )
{
	if( gEngfuncs.CL_EntitySetRemapColors( RI.currententity, RI.currentmodel, newTop, newBottom ))
		m_fDoRemap = true;
}

void R_StudioResetPlayerModels( void )
{
	memset( g_studio.player_models, 0, sizeof( g_studio.player_models ));
}

static model_t *R_StudioSetupPlayerModel( int index )
{
	player_info_t  *info = gEngfuncs.pfnPlayerInfo( index );

	if( index < 0 || index >= gp_cl->maxclients )
		return NULL;

	player_model_t *state = &g_studio.player_models[index];

	if(( gpGlobals->developer || !ENGINE_GET_PARM( PARM_SINGLEPLAYER_GAME ) || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) ) && info->model[0] )
	{
		if( Q_strcmp( state->name, info->model ))
		{
			Q_strncpy( state->name, info->model, sizeof( state->name ));
			state->name[sizeof( state->name ) - 1] = 0;

			Q_snprintf( state->modelname, sizeof( state->modelname ), "models/player/%s/%s.mdl", info->model, info->model );

			if( gEngfuncs.fsapi->FileExists( state->modelname, false ))
				state->model = gEngfuncs.Mod_ForName( state->modelname, false, true );
			else
				state->model = NULL;

			if( !state->model )
				state->model = RI.currententity->model;
		}
	}
	else
	{
		if( state->model != RI.currententity->model )
			state->model = RI.currententity->model;
		state->name[0] = 0;
	}

	return state->model;
}

int R_GetEntityRenderMode( cl_entity_t *ent )
{
	model_t *model = NULL;

	cl_entity_t *oldent = RI.currententity;
	RI.currententity = ent;

	if( ent->player )
		model = R_StudioSetupPlayerModel( ent->curstate.number - 1 );

	if( !model )
		model = ent->model;

	RI.currententity = oldent;

	studiohdr_t *phdr;
	if(( phdr = gEngfuncs.Mod_Extradata( mod_studio, model )) == NULL )
	{
		if( R_ModelOpaque( ent->curstate.rendermode ))
		{
			if(( model && model->type == mod_brush ) && FBitSet( model->flags, MODEL_TRANSPARENT ))
				return kRenderTransAlpha;
		}
		return ent->curstate.rendermode;
	}
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);

	int opaque = 0, trans = 0;
	for( int i = 0; i < phdr->numtextures; i++, ptexture++ )
	{
		if( FBitSet( ptexture->flags, STUDIO_NF_ADDITIVE ) && !FBitSet( ptexture->flags, STUDIO_NF_CHROME ))
			trans++;
		else opaque++;
	}

	if( trans > opaque )
		return kRenderTransAdd;
	return ent->curstate.rendermode;
}

static void R_StudioClientEvents( void )
{
	cl_entity_t	*e = RI.currententity;

	if( g_studio.frametime == 0.0 )
		return;

	if( m_pStudioHeader->numattachments <= 0 )
	{
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[0] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[1] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[2] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[3] );
	}

	if( FBitSet( e->curstate.effects, EF_MUZZLEFLASH ))
	{
		dlight_t	*el = gEngfuncs.CL_AllocElight( 0 );

		ClearBits( e->curstate.effects, EF_MUZZLEFLASH );
		VectorCopy( e->attachment[0], el->origin );
		el->die = gp_cl->time + 0.05f;
		el->color.r = 255;
		el->color.g = 192;
		el->color.b = 64;
		el->decay = 320;
		el->radius = 24;
	}

	int sequence = bound( 0, e->curstate.sequence, m_pStudioHeader->numseq - 1 );
	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + sequence;

	if( pseqdesc->numevents == 0 )
		return;

	float end = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );
	float start = end - e->curstate.framerate * gp_host->frametime * pseqdesc->fps;
	mstudioevent_t *pevent = (mstudioevent_t *)((byte *)m_pStudioHeader + pseqdesc->eventindex);

	if( e->latched.sequencetime == e->curstate.animtime )
	{
		if( !FBitSet( pseqdesc->flags, STUDIO_LOOPING ))
			start = -0.01f;
	}

	for( int i = 0; i < pseqdesc->numevents; i++ )
	{
		if( pevent[i].event < EVENT_CLIENT )
			continue;

		if( (float)pevent[i].frame > start && pevent[i].frame <= end )
			gEngfuncs.pfnStudioEvent( &pevent[i], e );
	}
}

static int R_StudioGetForceFaceFlags( void )
{
	return g_nForceFaceFlags;
}

static void R_StudioSetForceFaceFlags( int flags )
{
	g_nForceFaceFlags = flags;
}

static void R_StudioSetHeader( studiohdr_t *pheader )
{
	m_pStudioHeader = pheader;
	m_fDoRemap = false;
}

studiohdr_t *R_StudioGetHeader( void )
{
	return m_pStudioHeader;
}

static void R_StudioSetRenderModel( model_t *model )
{
	RI.currentmodel = model;
}

static void R_StudioSetupRenderer( int rendermode )
{
	studiohdr_t	*phdr = m_pStudioHeader;

	if( rendermode > kRenderTransAdd ) rendermode = 0;
	g_studio.rendermode = bound( 0, rendermode, kRenderTransAdd );

	if( g_studio.rendermode == kRenderTransAdd || g_studio.rendermode == kRenderGlow )
		R_AllowFog( false );

	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );

	if( phdr && FBitSet( phdr->flags, STUDIO_HAS_BONEINFO ))
	{
		mstudioboneinfo_t *boneinfo = (mstudioboneinfo_t *)((byte *)phdr + phdr->boneindex + phdr->numbones * sizeof( mstudiobone_t ));

		for( int i = 0; i < phdr->numbones; i++ )
			Matrix3x4_ConcatTransforms( g_studio.worldtransform[i], g_studio.bonestransform[i], boneinfo[i].poseToBone );
	}
}

static void R_StudioRestoreRenderer( void )
{
	if( g_studio.rendermode != kRenderNormal )
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );

	if( g_studio.rendermode == kRenderTransAdd || g_studio.rendermode == kRenderGlow )
		R_AllowFog( true );

	GX_SetTevOp( GX_TEVSTAGE0, GX_REPLACE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	m_fDoRemap = false;
}

static void R_StudioSetChromeOrigin( void )
{
	VectorCopy( RI.rvp.vieworigin, g_studio.chrome_origin );
}

static void R_StudioDrawPointsShadow( void )
{
	vec3_t		point;

	if( FBitSet( RI.currententity->curstate.effects, EF_NOSHADOW ))
		return;

	float vec_x = -g_studio.lightvec[0] * 8.0f;
	float vec_y = -g_studio.lightvec[1] * 8.0f;

	GX_SetupVtxFormatStudio( false, false );

	for( int k = 0; k < m_pSubModel->nummesh; k++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + k;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		r_stats.c_studio_polys += pmesh->numtris;

		int i;
		while(( i = *( ptricmds++ )))
		{
			if( i < 0 )
			{
				GX_Begin( GX_TRIANGLEFAN, GX_VTXFMT0, (u16)(-i) );
				i = -i;
			}
			else
			{
				GX_Begin( GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)i );
			}

			for( ; i > 0; i--, ptricmds += 4 )
			{
				float *av = g_studio.verts[ptricmds[0]];
				point[0] = av[0] - (vec_x * ( av[2] - g_studio.lightspot[2] ));
				point[1] = av[1] - (vec_y * ( av[2] - g_studio.lightspot[2] ));
				point[2] = g_studio.lightspot[2] + 1.0f;

				GX_Position3f32( point[0], point[1], point[2] );
			}

			GX_End();
		}
	}
}

static void GX_StudioSetRenderMode( int rendermode )
{
	switch( rendermode )
	{
	case kRenderNormal:
		break;
	case kRenderTransColor:
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		break;
	case kRenderTransAdd:
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		{
			u8 a = (u8)(tr.blend * 255.0f);
			GXColor c = { 255, 255, 255, a };
			GX_SetChanMatColor( GX_COLOR0A0, c );
		}
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		break;
	default:
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		{
			u8 a = (u8)(tr.blend * 255.0f);
			GXColor c = { 255, 255, 255, a };
			GX_SetChanMatColor( GX_COLOR0A0, c );
		}
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		break;
	}
}

static void GX_StudioDrawShadow( void )
{
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );

	if( r_shadows.value && g_studio.rendermode != kRenderTransAdd && !FBitSet( RI.currentmodel->flags, STUDIO_AMBIENT_LIGHT ))
	{
		float	color = 1.0f - (tr.blend * 0.5f);

		GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		{
			u8 a = (u8)((1.0f - color) * 255.0f);
			GXColor c = { 0, 0, 0, a };
			GX_SetChanMatColor( GX_COLOR0A0, c );
		}
		GX_SetZMode( GX_TRUE, GX_LESS, GX_TRUE );
		R_StudioDrawPointsShadow();
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );

		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GXColor white = { 255, 255, 255, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, white );
	}
}

static void R_StudioRenderFinal( void )
{
	int rendermode = R_StudioGetForceFaceFlags() ? kRenderTransAdd : RI.currententity->curstate.rendermode;
	R_StudioSetupRenderer( rendermode );

	if( r_drawentities->value == 2 )
	{
		R_StudioDrawBones();
	}
	else if( r_drawentities->value == 3 )
	{
		R_StudioDrawHulls();
	}
	else
	{
		for( int i = 0; i < m_pStudioHeader->numbodyparts; i++ )
		{
			R_StudioSetupModel( i, (void**)&m_pBodyPart, (void**)&m_pSubModel );

			GX_StudioSetRenderMode( rendermode );
			R_StudioDrawPoints();
			GX_StudioDrawShadow();
		}
	}

	if( r_drawentities->value == 4 )
	{
		TriRenderMode( kRenderTransAdd );
		R_StudioDrawHulls( );
		TriRenderMode( kRenderNormal );
	}

	if( r_drawentities->value == 5 )
	{
		R_StudioDrawAbsBBox( );
	}

	if( r_drawentities->value == 6 )
	{
		R_StudioDrawAttachments();
	}

	if( r_drawentities->value == 7 )
	{
		vec3_t	origin;

		GX_SetTevOp( GX_TEVSTAGE0, GX_PASSCLR );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0 );
		GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
		GX_SetupVtxFormatStudio( true, false );

		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );

		GXColor c1 = { 255, 128, 0, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, c1 );
		GX_Begin( GX_LINES, GX_VTXFMT0, 2 );
		GX_Position3f32( origin[0], origin[1], origin[2] );
		GX_Position3f32( g_studio.lightspot[0], g_studio.lightspot[1], g_studio.lightspot[2] );
		GX_End();

		GXColor c2 = { 0, 128, 255, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, c2 );
		VectorMA( g_studio.lightspot, -64.0f, g_studio.lightvec, origin );
		GX_Begin( GX_LINES, GX_VTXFMT0, 2 );
		GX_Position3f32( g_studio.lightspot[0], g_studio.lightspot[1], g_studio.lightspot[2] );
		GX_Position3f32( origin[0], origin[1], origin[2] );
		GX_End();

		GX_SetPointSize( 5.0f, GX_POINTS );
		GXColor c3 = { 255, 0, 0, 255 };
		GX_SetChanMatColor( GX_COLOR0A0, c3 );
		GX_Begin( GX_POINTS, GX_VTXFMT0, 1 );
		GX_Position3f32( g_studio.lightspot[0], g_studio.lightspot[1], g_studio.lightspot[2] );
		GX_End();
		GX_SetPointSize( 1.0f, GX_POINTS );

		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	}

	R_StudioRestoreRenderer();
}

static void R_StudioRenderModel( void )
{
	R_StudioSetChromeOrigin();
	R_StudioSetForceFaceFlags( 0 );

	if( RI.currententity->curstate.renderfx == kRenderFxGlowShell )
	{
		RI.currententity->curstate.renderfx = kRenderFxNone;

		R_StudioRenderFinal( );

		R_StudioSetForceFaceFlags( STUDIO_NF_CHROME );
		TriSpriteTexture( gEngfuncs.GetDefaultSprite( REF_CHROME_SPRITE ), 0 );
		RI.currententity->curstate.renderfx = kRenderFxGlowShell;

		R_StudioRenderFinal( );
	}
	else
	{
		R_StudioRenderFinal( );
	}
}

static void R_StudioEstimateGait( entity_state_t *pplayer )
{
	vec3_t	est_velocity;
	float dt = bound( 0.0f, g_studio.frametime, 1.0f );

	if( dt == 0.0f || m_pPlayerInfo->renderframe == tr.realframecount )
	{
		m_flGaitMovement = 0;
		return;
	}

	VectorSubtract( RI.currententity->origin, m_pPlayerInfo->prevgaitorigin, est_velocity );
	VectorCopy( RI.currententity->origin, m_pPlayerInfo->prevgaitorigin );
	m_flGaitMovement = VectorLength( est_velocity );

	if( dt <= 0.0f || m_flGaitMovement / dt < 5.0f )
	{
		m_flGaitMovement = 0.0f;
		est_velocity[0] = 0.0f;
		est_velocity[1] = 0.0f;
	}

	if( est_velocity[1] == 0.0f && est_velocity[0] == 0.0f )
	{
		float	flYawDiff = RI.currententity->angles[YAW] - m_pPlayerInfo->gaityaw;

		flYawDiff = flYawDiff - (int)(flYawDiff / 360) * 360;
		if( flYawDiff > 180.0f ) flYawDiff -= 360.0f;
		if( flYawDiff < -180.0f ) flYawDiff += 360.0f;

		if( dt < 0.25f )
			flYawDiff *= dt * 4.0f;
		else flYawDiff *= dt;

		m_pPlayerInfo->gaityaw += flYawDiff;
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - (int)(m_pPlayerInfo->gaityaw / 360) * 360;

		m_flGaitMovement = 0.0f;
	}
	else
	{
		m_pPlayerInfo->gaityaw = ( atan2( est_velocity[1], est_velocity[0] ) * 180 / M_PI_F );
		if( m_pPlayerInfo->gaityaw > 180.0f ) m_pPlayerInfo->gaityaw = 180.0f;
		if( m_pPlayerInfo->gaityaw < -180.0f ) m_pPlayerInfo->gaityaw = -180.0f;
	}
}

static void R_StudioProcessGait( entity_state_t *pplayer )
{
	if( RI.currententity->curstate.sequence >= m_pStudioHeader->numseq )
		RI.currententity->curstate.sequence = 0;

	float dt = bound( 0.0f, g_studio.frametime, 1.0f );

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + RI.currententity->curstate.sequence;

	int iBlend;
	R_StudioPlayerBlend( pseqdesc, &iBlend, &RI.currententity->angles[PITCH] );

	RI.currententity->latched.prevangles[PITCH] = RI.currententity->angles[PITCH];
	RI.currententity->curstate.blending[0] = iBlend;
	RI.currententity->latched.prevblending[0] = RI.currententity->curstate.blending[0];
	RI.currententity->latched.prevseqblending[0] = RI.currententity->curstate.blending[0];
	R_StudioEstimateGait( pplayer );

	float flYaw = RI.currententity->angles[YAW] - m_pPlayerInfo->gaityaw;
	flYaw = flYaw - (int)(flYaw / 360) * 360;
	if( flYaw < -180.0f ) flYaw = flYaw + 360.0f;
	if( flYaw > 180.0f ) flYaw = flYaw - 360.0f;

	if( flYaw > 120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw - 180.0f;
	}
	else if( flYaw < -120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw + 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw + 180.0f;
	}

	RI.currententity->curstate.controller[0] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[1] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[2] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[3] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->latched.prevcontroller[0] = RI.currententity->curstate.controller[0];
	RI.currententity->latched.prevcontroller[1] = RI.currententity->curstate.controller[1];
	RI.currententity->latched.prevcontroller[2] = RI.currententity->curstate.controller[2];
	RI.currententity->latched.prevcontroller[3] = RI.currententity->curstate.controller[3];

	RI.currententity->angles[YAW] = m_pPlayerInfo->gaityaw;
	if( RI.currententity->angles[YAW] < -0 ) RI.currententity->angles[YAW] += 360.0f;
	RI.currententity->latched.prevangles[YAW] = RI.currententity->angles[YAW];

	if( pplayer->gaitsequence >= m_pStudioHeader->numseq )
		pplayer->gaitsequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + pplayer->gaitsequence;

	if( pseqdesc->linearmovement[0] > 0 )
		m_pPlayerInfo->gaitframe += (m_flGaitMovement / pseqdesc->linearmovement[0]) * pseqdesc->numframes;
	else m_pPlayerInfo->gaitframe += pseqdesc->fps * dt;

	m_pPlayerInfo->gaitframe = m_pPlayerInfo->gaitframe - (int)(m_pPlayerInfo->gaitframe / pseqdesc->numframes) * pseqdesc->numframes;
	if( m_pPlayerInfo->gaitframe < 0 ) m_pPlayerInfo->gaitframe += pseqdesc->numframes;
}

static int R_StudioDrawPlayer( int flags, entity_state_t *pplayer )
{
	alight_t	lighting;
	vec3_t	dir;

	int m_nPlayerIndex = pplayer->number - 1;

	if( m_nPlayerIndex < 0 || m_nPlayerIndex >= gp_cl->maxclients )
		return 0;

	RI.currentmodel = R_StudioSetupPlayerModel( m_nPlayerIndex );
	if( RI.currentmodel == NULL )
		return 0;

	R_StudioSetHeader((studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, RI.currentmodel ));

	if( pplayer->gaitsequence )
	{
		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
		vec3_t orig_angles = Vec3( RI.currententity->angles );

		R_StudioProcessGait( pplayer );

		m_pPlayerInfo->gaitsequence = pplayer->gaitsequence;
		m_pPlayerInfo = NULL;

		R_StudioSetUpTransform( RI.currententity );
		VectorCopy( orig_angles, RI.currententity->angles );
	}
	else
	{
		RI.currententity->curstate.controller[0] = 127;
		RI.currententity->curstate.controller[1] = 127;
		RI.currententity->curstate.controller[2] = 127;
		RI.currententity->curstate.controller[3] = 127;
		RI.currententity->latched.prevcontroller[0] = RI.currententity->curstate.controller[0];
		RI.currententity->latched.prevcontroller[1] = RI.currententity->curstate.controller[1];
		RI.currententity->latched.prevcontroller[2] = RI.currententity->curstate.controller[2];
		RI.currententity->latched.prevcontroller[3] = RI.currententity->curstate.controller[3];

		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
		m_pPlayerInfo->gaitsequence = 0;

		R_StudioSetUpTransform( RI.currententity );
	}

	if( flags & STUDIO_RENDER )
	{
		if( !R_StudioCheckBBox( ))
			return 0;

		r_stats.c_studio_models_drawn++;
		g_studio.framecount++;

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
	R_StudioSetupBones( RI.currententity );
	R_StudioSaveBones( );

	m_pPlayerInfo->renderframe = tr.realframecount;
	m_pPlayerInfo = NULL;

	if( flags & STUDIO_EVENTS )
	{
		R_StudioCalcAttachments( );
		R_StudioClientEvents( );

		if( RI.currententity->index > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( RI.currententity->index );
			memcpy( ent->attachment, RI.currententity->attachment, sizeof( vec3_t ) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		if( cl_himodels->value && ( RI.currentmodel != RI.currententity->model || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD )))
		{
			RI.currententity->curstate.body = 255;
		}

		if( !( !gpGlobals->developer && gp_cl->maxclients == 1 ) && ( RI.currentmodel == RI.currententity->model ))
			RI.currententity->curstate.body = 1;

		prof_t = Platform_DoubleTime();
		lighting.plightvec = dir;
		R_EntityDynamicLight( RI.currententity, &lighting, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );

		R_StudioEntityLight( &lighting );

		R_StudioSetupLighting( &lighting );

		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );

		g_nTopColor = m_pPlayerInfo->topcolor;
		g_nBottomColor = m_pPlayerInfo->bottomcolor;

		if( g_nTopColor < 0 ) g_nTopColor = 0;
		if( g_nTopColor > 360 ) g_nTopColor = 360;
		if( g_nBottomColor < 0 ) g_nBottomColor = 0;
		if( g_nBottomColor > 360 ) g_nBottomColor = 360;

		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );

		R_StudioRenderModel( );
		m_pPlayerInfo = NULL;

		if( pplayer->weaponmodel )
		{
			cl_entity_t	saveent = *RI.currententity;
			model_t		*pweaponmodel = CL_ModelHandle( pplayer->weaponmodel );

			m_pStudioHeader = (studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, pweaponmodel );

			R_StudioMergeBones( RI.currententity, pweaponmodel );
			R_StudioSetupLighting( &lighting );
			R_StudioRenderModel( );
			R_StudioCalcAttachments( );

			*RI.currententity = saveent;
		}
	}

	return 1;
}

static int R_StudioDrawModel( int flags )
{
	alight_t	lighting;
	vec3_t	dir;

	if( RI.currententity->curstate.renderfx == kRenderFxDeadPlayer )
	{
		if( RI.currententity->curstate.renderamt <= 0 ||
			RI.currententity->curstate.renderamt > gp_cl->maxclients )
			return 0;

		entity_state_t deadplayer = *R_StudioGetPlayerState( RI.currententity->curstate.renderamt - 1 );

		deadplayer.number = RI.currententity->curstate.renderamt;
		deadplayer.weaponmodel = 0;
		deadplayer.gaitsequence = 0;

		deadplayer.movetype = MOVETYPE_NONE;
		VectorCopy( RI.currententity->curstate.angles, deadplayer.angles );
		VectorCopy( RI.currententity->curstate.origin, deadplayer.origin );

		g_studio.interpolate = false;
		int result = R_StudioDrawPlayer( flags, &deadplayer );
		g_studio.interpolate = true;

		return result;
	}

	R_StudioSetHeader((studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, RI.currentmodel ));

	R_StudioSetUpTransform( RI.currententity );

	if( flags & STUDIO_RENDER )
	{
		if( !R_StudioCheckBBox( ))
			return 0;

		r_stats.c_studio_models_drawn++;
		g_studio.framecount++;

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	double prof_t = Platform_DoubleTime();
	if( RI.currententity->curstate.movetype == MOVETYPE_FOLLOW )
		R_StudioMergeBones( RI.currententity, RI.currentmodel );
	else R_StudioSetupBones( RI.currententity );

	R_StudioSaveBones();
	OGC_ProfAdd( 21, Platform_DoubleTime() - prof_t ); // huesos (animacion)

	if( flags & STUDIO_EVENTS )
	{
		R_StudioCalcAttachments( );
		R_StudioClientEvents( );

		if( RI.currententity->index > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( RI.currententity->index );
			memcpy( ent->attachment, RI.currententity->attachment, sizeof( vec3_t ) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		lighting.plightvec = dir;
		R_EntityDynamicLight( RI.currententity, &lighting, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );

		R_StudioEntityLight( &lighting );

		R_StudioSetupLighting( &lighting );

		g_nTopColor = RI.currententity->curstate.colormap & 0xFF;
		g_nBottomColor = (RI.currententity->curstate.colormap & 0xFF00) >> 8;

		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );
		OGC_ProfAdd( 22, Platform_DoubleTime() - prof_t ); // luz (trazado de luz del mapa)

		prof_t = Platform_DoubleTime();
		R_StudioRenderModel();
		OGC_ProfAdd( 23, Platform_DoubleTime() - prof_t ); // malla (transformar vertices + enviarlos)
	}

	return 1;
}

static void R_StudioDrawModelInternal( cl_entity_t *e, int flags )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, &e->curstate );
		else R_StudioDrawModel( flags );
	}
	else if( unlikely( r_studio_builtin_renderer.value ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else R_StudioDrawModel( flags );
	}
	else
	{
		if( e->player )
			pStudioDraw->StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else pStudioDraw->StudioDrawModel( flags );
	}
}

static cl_entity_t *R_FindParentEntity( cl_entity_t *e, cl_entity_t **entities, uint num_entities )
{
	for( uint i = 0; i < num_entities; i++ )
	{
		if( entities[i]->index == e->curstate.aiment )
			return entities[i];
	}

	return NULL;
}

void R_DrawStudioModel( cl_entity_t *e )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		return;

	R_StudioSetupTimings();

	if( e->player )
	{
		R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
	}
	else if( e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		cl_entity_t *parent = CL_GetEntityByIndex( e->curstate.aiment );

		if( !parent || !parent->model || parent->model->type != mod_studio )
			return;

		parent = R_FindParentEntity( e, tr.draw_list->solid_entities, tr.draw_list->num_solid_entities );

		if( !parent )
			parent = R_FindParentEntity( e, tr.draw_list->trans_entities, tr.draw_list->num_trans_entities );

		if( parent )
		{
			RI.currententity = parent;
			R_StudioDrawModelInternal( RI.currententity, 0 );
			VectorCopy( RI.currententity->curstate.origin, e->curstate.origin );
			VectorCopy( RI.currententity->origin, e->origin );
			RI.currententity = e;

			R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
		}
	}
	else
	{
		R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
	}
}

void R_RunViewmodelEvents( void )
{
	if( r_drawviewmodel->value == 0 )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return;

	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) || ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer())
		return;

	RI.currententity = tr.viewent;

	if( !RI.currententity->model || RI.currententity->model->type != mod_studio )
		return;

	R_StudioSetupTimings();

	vec3_t simorg = Vec3( gp_cl->simorg );
	for( int i = 0; i < 4; i++ )
		VectorCopy( simorg, RI.currententity->attachment[i] );
	RI.currentmodel = RI.currententity->model;

	R_StudioDrawModelInternal( RI.currententity, STUDIO_EVENTS );
}

void R_DrawViewModel( void )
{
	cl_entity_t	*view = tr.viewent;

	R_GatherPlayerLight( view );

	if( r_drawviewmodel->value == 0 )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return;

	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) || ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer())
		return;

	tr.blend = CL_FxBlend( view ) / 255.0f;
	if( !R_ModelOpaque( view->curstate.rendermode ) && tr.blend <= 0.0f )
		return;

	RI.currententity = view;

	if( !RI.currententity->model )
		return;

	RI.currentmodel = RI.currententity->model;

	// Igual que Quake/GL: el arma usa solo el primer 30% del rango de
	// profundidad, asi nunca queda tapada ni titila al pegarse a una pared.
	GX_SetDepthRange( gldepthmin, gldepthmin + 0.3f * ( gldepthmax - gldepthmin ));

	// Estado conocido: el arma se dibujaba con lo que dejara el ultimo sprite,
	// beam o entidad translucida (Z sin escritura, blend, alpha-test, color de
	// canal con alfa bajo...) y por eso alternaba entre solida y transparente.
	{
		GXColor white = { 255, 255, 255, 255 };
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		GX_SetChanMatColor( GX_COLOR0A0, white );
		R_AllowFog( true );
	}

	switch( RI.currententity->model->type )
	{
	case mod_alias:
		R_DrawAliasModel( RI.currententity );
		break;
	case mod_studio:
		R_StudioSetupTimings();
		R_StudioDrawModelInternal( RI.currententity, STUDIO_RENDER );
		break;
	}

	GX_SetDepthRange( gldepthmin, gldepthmax );
}

static void R_StudioLoadTexture( model_t *mod, studiohdr_t *phdr, mstudiotexture_t *ptexture )
{
	int		flags = 0;
	char			texname[128], name[128], mdlname[128];
	texture_t		*tx = NULL;
	qboolean load_external = false;

	if( ptexture->flags & STUDIO_NF_NORMALMAP )
		flags |= (TF_NORMALMAP);

	if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ) || !Q_strnicmp( ptexture->name, "remap", 5 ))
	{
		char	val[6];

		int i = mod->numtextures;
		mod->textures = (texture_t **)Mem_Realloc( mod->mempool, mod->textures, ( i + 1 ) * sizeof( texture_t* ));
		int size = ptexture->width * ptexture->height + 768;
		tx = Mem_Calloc( mod->mempool, sizeof( *tx ) + size );
		mod->textures[i] = tx;

		if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ))
		{
			Q_strncpy( tx->name, "DM_Base", sizeof( tx->name ));
			tx->anim_min = PLATE_HUE_START;
			tx->anim_max = PLATE_HUE_END;
			tx->anim_total = SUIT_HUE_END;
		}
		else
		{
			Q_strncpy( tx->name, "DM_User", sizeof( tx->name ));
			Q_strncpy( val, ptexture->name + 7, 4 );
			tx->anim_min = bound( 0, Q_atoi( val ), 255 );
			Q_strncpy( val, ptexture->name + 11, 4 );
			tx->anim_max = bound( 0, Q_atoi( val ), 255 );
			Q_strncpy( val, ptexture->name + 15, 4 );
			tx->anim_total = bound( 0, Q_atoi( val ), 255 );
		}

		tx->width = ptexture->width;
		tx->height = ptexture->height;

		byte *pixels = (byte *)phdr + ptexture->index;
		memcpy( tx+1, pixels, size );

		ptexture->flags |= STUDIO_NF_COLORMAP;
		flags |= TF_FORCE_COLOR;

		mod->numtextures++;
	}

	Q_strncpy( mdlname, mod->name, sizeof( mdlname ));
	COM_FileBase( ptexture->name, name, sizeof( name ));
	COM_StripExtension( mdlname );

	if( FBitSet( ptexture->flags, STUDIO_NF_NOMIPS ))
		SetBits( flags, TF_NOMIPMAP );

	if( FBitSet( gp_host->features, ENGINE_IMPROVED_LINETRACE ) && FBitSet( ptexture->flags, STUDIO_NF_MASKED ))
		flags |= TF_KEEP_SOURCE;

	if( Mod_AllowMaterials( ) && !FBitSet( ptexture->flags, STUDIO_NF_COLORMAP ))
	{
		if( R_SearchForTextureReplacement( texname, sizeof( texname ), mdlname, "materials/%s/%s.tga", mdlname, name ))
		{
			int gl_texturenum = GX_LoadTexture( texname, NULL, 0, flags );
			R_TextureReplacementReport( mdlname, gl_texturenum, texname );
			if(( load_external = gl_texturenum != 0 ))
				ptexture->index = gl_texturenum;
		}
	}

	if( !load_external )
	{
		gEngfuncs.Image_SetMDLPointer((byte *)phdr + ptexture->index);
		size_t size = sizeof( mstudiotexture_t ) + ptexture->width * ptexture->height + 768;

		Q_snprintf( texname, sizeof( texname ), "#%s/%s.mdl", mdlname, name );
		ptexture->index = GX_LoadTexture( texname, (byte *)ptexture, size, flags );
	}

	if( !ptexture->index )
	{
		ptexture->index = tr.defaultTexture;
	}
	else if( tx )
	{
		tx->gl_texturenum = ptexture->index;
	}
}

void Mod_StudioLoadTextures( model_t *mod, void *data )
{
	studiohdr_t	*phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr) + phdr->textureindex);
	if( phdr->textureindex > 0 )
	{
		for( int i = 0; i < phdr->numtextures; i++ )
			R_StudioLoadTexture( mod, phdr, &ptexture[i] );
	}
}

void Mod_StudioUnloadTextures( void *data )
{
	studiohdr_t	*phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr) + phdr->textureindex);

	for( int i = 0; i < phdr->numtextures; i++ )
	{
		if( ptexture[i].index == tr.defaultTexture )
			continue;
		GX_FreeTexture( ptexture[i].index );
	}
}

static void pfnStudioDynamicLight( cl_entity_t *ent, alight_t *plight )
{
	R_EntityDynamicLight( ent, plight, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );
}

qboolean R_StudioFillAPI( engine_studio_api_t *api, r_studio_interface_t *pDefaultDraw )
{
	cl_righthand = gEngfuncs.pfnGetCvarPointer( "cl_righthand" );

	api->GetCurrentEntity        = pfnGetCurrentEntity;
	api->PlayerInfo              = pfnPlayerInfo;
	api->GetPlayerState          = R_StudioGetPlayerState;
	api->GetTimes                = pfnGetEngineTimes;
	api->GetViewInfo             = pfnGetViewInfo;
	api->GetModelCounters        = pfnGetModelCounters;
	api->StudioGetBoneTransform  = pfnStudioGetBoneTransform;
	api->StudioGetLightTransform = pfnStudioGetLightTransform;
	api->StudioGetRotationMatrix = pfnStudioGetRotationMatrix;
	api->StudioSetupModel        = R_StudioSetupModel;
	api->StudioCheckBBox         = R_StudioCheckBBox;
	api->StudioDynamicLight      = pfnStudioDynamicLight;
	api->StudioEntityLight       = R_StudioEntityLight;
	api->StudioSetupLighting     = R_StudioSetupLighting;
	api->StudioDrawPoints        = R_StudioDrawPoints;
	api->StudioDrawHulls         = R_StudioDrawHulls;
	api->StudioDrawAbsBBox       = R_StudioDrawAbsBBox;
	api->StudioDrawBones         = R_StudioDrawBones;
	api->StudioSetupSkin = NULL;
	api->StudioSetRemapColors    = R_StudioSetRemapColors;
	api->SetupPlayerModel        = R_StudioSetupPlayerModel;
	api->StudioClientEvents      = R_StudioClientEvents;
	api->GetForceFaceFlags       = R_StudioGetForceFaceFlags;
	api->SetForceFaceFlags       = R_StudioSetForceFaceFlags;
	api->StudioSetHeader         = (void *)R_StudioSetHeader;
	api->SetRenderModel          = R_StudioSetRenderModel;
	api->SetupRenderer           = R_StudioSetupRenderer;
	api->RestoreRenderer         = R_StudioRestoreRenderer;
	api->SetChromeOrigin         = R_StudioSetChromeOrigin;
	api->GL_StudioDrawShadow     = GX_StudioDrawShadow;
	api->GL_SetRenderMode        = GX_StudioSetRenderMode;
	api->StudioSetCullState = NULL;
	api->StudioRenderShadow = NULL;

	pDefaultDraw->version         = STUDIO_INTERFACE_VERSION;
	pDefaultDraw->StudioDrawModel  = R_StudioDrawModel;
	pDefaultDraw->StudioDrawPlayer = R_StudioDrawPlayer;

	return true;
}

void R_StudioSetDrawInterface( r_studio_interface_t *pDraw )
{
	pStudioDraw = pDraw;
}