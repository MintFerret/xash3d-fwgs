/*
gx_rpart.c - particles and tracers (Wii GX native port)
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
#include "r_efx.h"
#include "event_flags.h"
#include "entity_types.h"
#include "triangleapi.h"
#include "pm_local.h"
#include "studio.h"

static float gTracerSize[11] = { 1.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
static color24 gTracerColors[] =
{
{ 255, 255, 255 },
{ 255, 0, 0 },
{ 0, 255, 0 },
{ 0, 0, 255 },
{ 0, 0, 0 },
{ 255, 167, 17 },
{ 255, 130, 90 },
{ 55, 60, 144 },
{ 255, 130, 90 },
{ 255, 140, 90 },
{ 200, 130, 90 },
{ 255, 120, 70 },
};

void CL_DrawParticles( double frametime, particle_t *cl_active_particles, float partsize )
{
	if( !cl_active_particles )
		return;

	R_LoadIdentity();

	GX_Cull( XASH_CULL_NONE );
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );

	GX_Bind( XASH_TEXTURE0, tr.particleTexture );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

	GX_SetChanVtxColor( true );
	GX_SetNumTevStages( 1 );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS,  GX_DIRECT );
	GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0 );

	int totalVerts = 0;
	for( particle_t *p = cl_active_particles; p; p = p->next )
	{
		if( ( p->type != pt_blob ) || ( p->unused == 255 ) )
			totalVerts += 4;
	}

	if( totalVerts == 0 )
	{
		GX_Cull( XASH_CULL_BACK );
		return;
	}

	GX_Begin( GX_QUADS, GX_VTXFMT0, totalVerts );

	for( particle_t *p = cl_active_particles; p; p = p->next )
	{
		if( ( p->type != pt_blob ) || ( p->unused == 255 ) )
		{
			float size = partsize;

			size += (p->org[0] - RI.rvp.vieworigin[0]) * RI.cull_vforward[0];
			size += (p->org[1] - RI.rvp.vieworigin[1]) * RI.cull_vforward[1];
			size += (p->org[2] - RI.rvp.vieworigin[2]) * RI.cull_vforward[2];

			if( size < 20.0f ) size = partsize;
			else size = partsize + size * 0.002f;

			vec3_t right, up;
			VectorScale( RI.cull_vright, size, right );
			VectorScale( RI.cull_vup, size, up );

			p->color = bound( 0, p->color, 255 );
			color24 color = tr.palette[p->color];

			int alpha = 255 * (p->die - gp_cl->time) * 16.0f;
			if( alpha > 255 || p->type == pt_static )
				alpha = 255;

			vec3_t v1, v2, v3, v4;
			VectorAdd( p->org, right, v1 );
			VectorAdd( v1, up, v1 );

			VectorAdd( p->org, right, v2 );
			VectorSubtract( v2, up, v2 );

			VectorSubtract( p->org, right, v3 );
			VectorSubtract( v3, up, v3 );

			VectorSubtract( p->org, right, v4 );
			VectorAdd( v4, up, v4 );

			GX_Position3f32( v1[0], v1[1], v1[2] );
			GX_Color4u8( color.r, color.g, color.b, alpha );
			GX_TexCoord2f32( 0.0f, 1.0f );

			GX_Position3f32( v2[0], v2[1], v2[2] );
			GX_Color4u8( color.r, color.g, color.b, alpha );
			GX_TexCoord2f32( 0.0f, 0.0f );

			GX_Position3f32( v3[0], v3[1], v3[2] );
			GX_Color4u8( color.r, color.g, color.b, alpha );
			GX_TexCoord2f32( 1.0f, 0.0f );

			GX_Position3f32( v4[0], v4[1], v4[2] );
			GX_Color4u8( color.r, color.g, color.b, alpha );
			GX_TexCoord2f32( 1.0f, 1.0f );

			r_stats.c_particle_count++;
		}

		gEngfuncs.CL_ThinkParticle( frametime, p );
	}

	GX_End();

	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_Cull( XASH_CULL_BACK );
}

static qboolean CL_CullTracer( particle_t *p, const vec3_t start, const vec3_t end )
{
	vec3_t mins, maxs;

	for( int i = 0; i < 3; i++ )
	{
		if( start[i] < end[i] )
		{
			mins[i] = start[i];
			maxs[i] = end[i];
		}
		else
		{
			mins[i] = end[i];
			maxs[i] = start[i];
		}

		if( mins[i] == maxs[i] )
		{
			maxs[i] += gTracerSize[p->type] * 2.0f;
		}
	}

	return R_CullBox( mins, maxs );
}

void CL_DrawTracers( double frametime, particle_t *cl_active_tracers )
{
	if( FBitSet( tracerred->flags|tracergreen->flags|tracerblue->flags|traceralpha->flags, FCVAR_CHANGED ))
	{
		color24 *customColors = &gTracerColors[4];
		customColors->r = (byte)(tracerred->value * traceralpha->value * 255);
		customColors->g = (byte)(tracergreen->value * traceralpha->value * 255);
		customColors->b = (byte)(tracerblue->value * traceralpha->value * 255);
		ClearBits( tracerred->flags, FCVAR_CHANGED );
		ClearBits( tracergreen->flags, FCVAR_CHANGED );
		ClearBits( tracerblue->flags, FCVAR_CHANGED );
		ClearBits( traceralpha->flags, FCVAR_CHANGED );
	}

	if( !cl_active_tracers )
		return;

	if( !TriSpriteTexture( gEngfuncs.GetDefaultSprite( REF_DOT_SPRITE ), 0 ))
		return;

	R_LoadIdentity();

	R_AllowFog( false );
	GX_Cull( XASH_CULL_NONE );
	GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR );
	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );

	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

	GX_SetChanVtxColor( true );
	GX_SetNumTevStages( 1 );
	GX_ClearVtxDesc();
	GX_SetVtxDesc( GX_VA_POS,  GX_DIRECT );
	GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
	GX_SetVtxDesc( GX_VA_TEX0, GX_DIRECT );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_POS,  GX_POS_XYZ, GX_F32, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0 );
	GX_SetVtxAttrFmt( GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0 );

	float gravity = frametime * gp_movevars->gravity;
	float scale = 1.0 - (frametime * 0.9);
	if( scale < 0.0f ) scale = 0.0f;

	int totalVerts = 0;
	for( particle_t *p = cl_active_tracers; p; p = p->next )
	{
		float atten = (p->die - gp_cl->time);
		if( atten > 0.1f ) atten = 0.1f;

		vec3_t start, end, delta;
		VectorScale( p->vel, ( p->ramp * atten ), delta );
		VectorAdd( p->org, delta, end );
		VectorCopy( p->org, start );

		if( !CL_CullTracer( p, start, end ) )
			totalVerts += 4;
	}

	if( totalVerts == 0 )
	{
		GX_Cull( XASH_CULL_BACK );
		R_AllowFog( true );
		return;
	}

	GX_Begin( GX_QUADS, GX_VTXFMT0, totalVerts );

	for( particle_t *p = cl_active_tracers; p; p = p->next )
	{
		float atten = (p->die - gp_cl->time);
		if( atten > 0.1f ) atten = 0.1f;

		vec3_t start, end, delta;
		VectorScale( p->vel, ( p->ramp * atten ), delta );
		VectorAdd( p->org, delta, end );
		VectorCopy( p->org, start );

		if( !CL_CullTracer( p, start, end ))
		{
			vec3_t	verts[4], tmp2;
			vec3_t	tmp, normal;
			vec3_t	screen, screenLast;

			TriWorldToScreen( start, screen );
			TriWorldToScreen( end, screenLast );

			VectorSubtract( screen, screenLast, tmp );

			tmp[2] = 0;
			VectorNormalize( tmp );

			VectorScale( RI.cull_vup, tmp[0] * gTracerSize[p->type], normal );
			VectorScale( RI.cull_vright, -tmp[1] * gTracerSize[p->type], tmp2 );
			VectorSubtract( normal, tmp2, normal );

			VectorSubtract( start, normal, verts[0] );
			VectorAdd( start, normal, verts[1] );
			VectorAdd( verts[0], delta, verts[2] );
			VectorAdd( verts[1], delta, verts[3] );

			if( p->color < 0 || p->color >= sizeof( gTracerColors ) / sizeof( gTracerColors[0] ))
			{
				p->color = TRACER_COLORINDEX_DEFAULT;
			}

			color24 color = gTracerColors[p->color];
			u8 alpha = p->unused;

			GX_Position3f32( verts[2][0], verts[2][1], verts[2][2] );
			GX_Color4u8( color.r, color.g, color.b, alpha );
			GX_TexCoord2f32( 0.0f, 0.8f );

			GX_Position3f32( verts[3][0], verts[3][1], verts[3][2] );
			GX_Color4u8( color.r, color.g, color.b, alpha );
			GX_TexCoord2f32( 1.0f, 0.8f );

			GX_Position3f32( verts[1][0], verts[1][1], verts[1][2] );
			GX_Color4u8( color.r, color.g, color.b, alpha );
			GX_TexCoord2f32( 1.0f, 0.0f );

			GX_Position3f32( verts[0][0], verts[0][1], verts[0][2] );
			GX_Color4u8( color.r, color.g, color.b, alpha );
			GX_TexCoord2f32( 0.0f, 0.0f );
		}

		VectorMA( p->org, frametime, p->vel, p->org );

		if( p->type == pt_grav )
		{
			p->vel[0] *= scale;
			p->vel[1] *= scale;
			p->vel[2] -= gravity;

			p->unused = 255 * (p->die - gp_cl->time) * 2;
			if( p->unused > 255 ) p->unused = 255;
		}
		else if( p->type == pt_slowgrav )
		{
			p->vel[2] = gravity * 0.05f;
		}
	}

	GX_End();

	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	GX_Cull( XASH_CULL_BACK );
	R_AllowFog( true );
}

void CL_DrawParticlesExternal( const ref_viewpass_t *rvp, qboolean trans_pass, float frametime )
{
	ref_instance_t	oldRI = RI;

	R_SetupRefParams( rvp );
	R_SetupFrustum();
	R_SetupGL( false );
	tr.frametime = frametime;

	gEngfuncs.CL_DrawEFX( frametime, trans_pass );

	RI = oldRI;
}