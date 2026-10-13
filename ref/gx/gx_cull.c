/*
gx_cull.c - render culling routines (Wii GX native port)
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
#include "entity_types.h"

qboolean R_CullBox( const vec3_t mins, const vec3_t maxs )
{
	return GL_FrustumCullBox( &RI.frustum, mins, maxs, 0 );
}

qboolean R_CullModel( const cl_entity_t *e, const vec3_t absmin, const vec3_t absmax )
{
	if( e == tr.viewent )
	{
		if( ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
			return true;

		if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) && !ENGINE_GET_PARM( PARM_THIRDPERSON ) && CL_IsViewEntityLocalPlayer())
			return false;

		return true;
	}

	return R_CullBox( absmin, absmax );
}

int R_CullSurface( const msurface_t *surf, const gl_frustum_t *frustum, uint clipflags )
{
	const cl_entity_t *e = RI.currententity;

	if( surf->visframe != tr.framecount && e == CL_GetEntityByIndex( 0 ))
		return CULL_VISFRAME;

	if( unlikely( !surf->texinfo || !surf->texinfo->texture ))
		return CULL_OTHER;

	if( unlikely( r_nocull.value ))
		return CULL_VISIBLE;

	if( !R_StaticEntity( e ))
		frustum = NULL;

	if( glState.faceCull != XASH_CULL_NONE && !VectorIsNull( surf->plane->normal ))
	{
		float	dist;

		if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
		{
			vec3_t	orthonormal;

			if( e == CL_GetEntityByIndex( 0 )) orthonormal[2] = surf->plane->normal[2];
			else Matrix4x4_VectorRotate( RI.objectMatrix, surf->plane->normal, orthonormal );
			dist = orthonormal[2];
		}
		else dist = PlaneDiff( tr.modelorg, surf->plane );

		if( FBitSet( surf->flags, SURF_PLANEBACK ))
			dist = -dist;

		/* La convencion del culling por CPU es siempre la de GL_FRONT (la que usa el mundo). */
		/* glState.faceCull cambia con GX_Cull(BACK/NONE) de modelos, sprites, particulas y decals; */
		/* si se respetara BACK aqui, el test se invertia y se descartaban TODAS las caras visibles. */
		if( dist <= BACKFACE_EPSILON )
			return CULL_BACKSIDE;
	}

	if( frustum && clipflags && GL_FrustumCullBox( frustum, surf->info->mins, surf->info->maxs, clipflags ))
		return CULL_FRUSTUM;

	return CULL_VISIBLE;
}