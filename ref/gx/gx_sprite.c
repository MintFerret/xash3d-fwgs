/*
gx_sprite.c - sprite rendering (Wii GX native port)
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
#include "pm_local.h"
#include "sprite.h"
#include "studio.h"
#include "entity_types.h"

#define GLARE_FALLOFF	19000.0f

static void GX_SetupVtxFormatSprite( qboolean useColor, qboolean useTex )
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

static float R_GetSpriteFrameInterpolant( cl_entity_t *ent, mspriteframe_t **oldframe, mspriteframe_t **curframe )
{
	msprite_t *psprite = ent->model->cache.data;
	int frame = (int)ent->curstate.frame;
	float lerpFrac = 1.0f;

	int m_fDoInterp = (ent->curstate.effects & EF_NOINTERP) ? false : true;

	if( frame < 0 )
	{
		frame = 0;
	}
	else if( frame >= psprite->numframes )
	{
		gEngfuncs.Con_Reportf( S_WARN "%s: no such frame %d (%s)\n", __func__, frame, ent->model->name );
		frame = psprite->numframes - 1;
	}

	if( psprite->frames[frame].type == FRAME_SINGLE )
	{
		if( m_fDoInterp )
		{
			if( ent->latched.prevblending[0] >= psprite->numframes || psprite->frames[ent->latched.prevblending[0]].type != FRAME_SINGLE )
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 1.0f;
			}

			if( ent->latched.sequencetime < gp_cl->time )
			{
				if( frame != ent->latched.prevblending[1] )
				{
					ent->latched.prevblending[0] = ent->latched.prevblending[1];
					ent->latched.prevblending[1] = frame;
					ent->latched.sequencetime = gp_cl->time;
					lerpFrac = 0.0f;
				}
				else lerpFrac = (gp_cl->time - ent->latched.sequencetime) * 11.0f;
			}
			else
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 0.0f;
			}
		}
		else
		{
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			lerpFrac = 1.0f;
		}

		if( ent->latched.prevblending[0] >= psprite->numframes )
		{
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			ent->latched.sequencetime = gp_cl->time;
			lerpFrac = 0.0f;
		}

		if( oldframe ) *oldframe = psprite->frames[ent->latched.prevblending[0]].frameptr;
		if( curframe ) *curframe = psprite->frames[frame].frameptr;
	}
	else if( psprite->frames[frame].type == FRAME_GROUP )
	{
		mspritegroup_t *pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		float *pintervals = pspritegroup->intervals;
		int numframes = pspritegroup->numframes;
		float fullinterval = pintervals[numframes-1];
		float jinterval = pintervals[1] - pintervals[0];
		float time = gp_cl->time;
		float jtime = 0.0f;

		float targettime = time - ((int)(time / fullinterval)) * fullinterval;

		int i, j;
		for( i = 0, j = numframes - 1; i < (numframes - 1); i++ )
		{
			if( pintervals[i] > targettime )
				break;
			j = i;
			jinterval = pintervals[i] - jtime;
			jtime = pintervals[i];
		}

		if( m_fDoInterp )
			lerpFrac = (targettime - jtime) / jinterval;
		else j = i;

		if( oldframe ) *oldframe = pspritegroup->frames[j];
		if( curframe ) *curframe = pspritegroup->frames[i];
	}
	else if( psprite->frames[frame].type == FRAME_ANGLED )
	{
		float	yaw = ent->angles[YAW];
		int	angleframe = (int)(Q_rint(( RI.rvp.viewangles[1] - yaw + 45.0f ) / 360 * 8) - 4) & 7;

		if( m_fDoInterp )
		{
			if( ent->latched.prevblending[0] >= psprite->numframes || psprite->frames[ent->latched.prevblending[0]].type != FRAME_ANGLED )
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 1.0f;
			}

			if( ent->latched.sequencetime < gp_cl->time )
			{
				if( frame != ent->latched.prevblending[1] )
				{
					ent->latched.prevblending[0] = ent->latched.prevblending[1];
					ent->latched.prevblending[1] = frame;
					ent->latched.sequencetime = gp_cl->time;
					lerpFrac = 0.0f;
				}
				else lerpFrac = (gp_cl->time - ent->latched.sequencetime) * ent->curstate.framerate;
			}
			else
			{
				ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
				ent->latched.sequencetime = gp_cl->time;
				lerpFrac = 0.0f;
			}
		}
		else
		{
			ent->latched.prevblending[0] = ent->latched.prevblending[1] = frame;
			lerpFrac = 1.0f;
		}

		mspritegroup_t *pspritegroup = (mspritegroup_t *)psprite->frames[ent->latched.prevblending[0]].frameptr;
		if( oldframe ) *oldframe = pspritegroup->frames[angleframe];

		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		if( curframe ) *curframe = pspritegroup->frames[angleframe];
	}

	return lerpFrac;
}

static qboolean R_CullSpriteModel( cl_entity_t *e, vec3_t origin )
{
	vec3_t	sprite_mins, sprite_maxs;
	float	scale = 1.0f;

	if( !e->model->cache.data )
		return true;

	if( e->curstate.scale > 0.0f )
		scale = e->curstate.scale;

	VectorScale( e->model->mins, scale, sprite_mins );
	VectorScale( e->model->maxs, scale, sprite_maxs );

	VectorAdd( sprite_mins, origin, sprite_mins );
	VectorAdd( sprite_maxs, origin, sprite_maxs );

	return R_CullModel( e, sprite_mins, sprite_maxs );
}

static float R_SpriteGlowBlend( vec3_t origin, int rendermode, int renderfx, float *pscale )
{
	vec3_t glowDist;
	VectorSubtract( origin, RI.rvp.vieworigin, glowDist );
	float dist = VectorLength( glowDist );

	if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
	{
		pmtrace_t *tr = gEngfuncs.EV_VisTraceLine( RI.rvp.vieworigin, origin, r_traceglow.value ? PM_GLASS_IGNORE : (PM_GLASS_IGNORE|PM_STUDIO_IGNORE));

		if(( 1.0f - tr->fraction ) * dist > 8.0f )
			return 0.0f;
	}

	if( renderfx == kRenderFxNoDissipation )
		return 1.0f;

	float brightness = GLARE_FALLOFF / ( dist * dist );
	brightness = bound( 0.05f, brightness, 1.0f );
	*pscale *= dist * ( 1.0f / 200.0f );

	return brightness;
}

static qboolean R_SpriteOccluded( cl_entity_t *e, vec3_t origin, float *pscale )
{
	if( e->curstate.rendermode == kRenderGlow )
	{
		float	blend;
		vec3_t	v;

		TriWorldToScreen( origin, v );

		if( v[0] < RI.rvp.viewport[0] || v[0] > RI.rvp.viewport[0] + RI.rvp.viewport[2] )
			return true;
		if( v[1] < RI.rvp.viewport[1] || v[1] > RI.rvp.viewport[1] + RI.rvp.viewport[3] )
			return true;

		blend = R_SpriteGlowBlend( origin, e->curstate.rendermode, e->curstate.renderfx, pscale );
		tr.blend *= blend;

		if( blend <= 0.01f )
			return true;
	}
	else
	{
		if( R_CullSpriteModel( e, origin ))
			return true;
	}

	return false;
}

static void GX_DrawSpriteQuad( mspriteframe_t *frame, vec3_t org, vec3_t v_right, vec3_t v_up, float scale )
{
	vec3_t	point;

	r_stats.c_sprite_polys++;

	GX_SetupVtxFormatSprite( false, true );

	GX_Begin( GX_QUADS, GX_VTXFMT0, 4 );

	VectorMA( org, frame->down * scale, v_up, point );
	VectorMA( point, frame->left * scale, v_right, point );
	GX_Position3f32( point[0], point[1], point[2] );
	GX_TexCoord2f32( 0.0f, 1.0f );

	VectorMA( org, frame->up * scale, v_up, point );
	VectorMA( point, frame->left * scale, v_right, point );
	GX_Position3f32( point[0], point[1], point[2] );
	GX_TexCoord2f32( 0.0f, 0.0f );

	VectorMA( org, frame->up * scale, v_up, point );
	VectorMA( point, frame->right * scale, v_right, point );
	GX_Position3f32( point[0], point[1], point[2] );
	GX_TexCoord2f32( 1.0f, 0.0f );

	VectorMA( org, frame->down * scale, v_up, point );
	VectorMA( point, frame->right * scale, v_right, point );
	GX_Position3f32( point[0], point[1], point[2] );
	GX_TexCoord2f32( 1.0f, 1.0f );

	GX_End();
}

static qboolean R_SpriteHasLightmap( cl_entity_t *e, int texFormat )
{
	if( !r_sprite_lighting->value )
		return false;

	if( texFormat != SPR_ALPHTEST )
		return false;

	if( FBitSet( e->curstate.effects, EF_FULLBRIGHT ))
		return false;

	if( e->curstate.renderamt <= 127 )
		return false;

	switch( e->curstate.rendermode )
	{
	case kRenderNormal:
	case kRenderTransAlpha:
	case kRenderTransTexture:
		break;
	default:
		return false;
	}

	return true;
}

static qboolean R_SpriteAllowLerping( cl_entity_t *e, msprite_t *psprite )
{
	if( !r_sprite_lerping->value )
		return false;

	if( psprite->numframes <= 1 )
		return false;

	if( psprite->texFormat != SPR_ADDITIVE )
		return false;

	if( e->curstate.rendermode == kRenderNormal || e->curstate.rendermode == kRenderTransAlpha )
		return false;

	return true;
}

static void R_DrawSpriteModelInt( cl_entity_t *e )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		return;

	model_t *model = e->model;
	msprite_t *psprite = (msprite_t *)model->cache.data;
	vec3_t color, color2 = { 0.0f };
	vec3_t v_right, v_up;
	vec3_t origin = Vec3( e->origin );

	if( e->curstate.aiment > 0 && e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		cl_entity_t *parent = CL_GetEntityByIndex( e->curstate.aiment );

		if( parent && parent->model )
		{
			if( parent->model->type == mod_studio && e->curstate.body > 0 )
			{
				int num = bound( 1, e->curstate.body, MAXSTUDIOATTACHMENTS );
				VectorCopy( parent->attachment[num-1], origin );
			}
			else VectorCopy( parent->origin, origin );
		}
	}

	float scale = e->curstate.scale;
	if( !scale ) scale = 1.0f;

	if( R_SpriteOccluded( e, origin, &scale ))
		return;

	R_LoadIdentity();

	r_stats.c_sprite_models_drawn++;

	if( e->curstate.rendermode == kRenderGlow || e->curstate.rendermode == kRenderTransAdd )
		R_AllowFog( false );

	switch( e->curstate.rendermode )
	{
	case kRenderTransAlpha:
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
	case kRenderTransColor:
	case kRenderTransTexture:
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR );
		break;
	case kRenderGlow:
		GX_SetZMode( GX_FALSE, GX_ALWAYS, GX_FALSE );
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR );
		break;
	case kRenderTransAdd:
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_FALSE );
		break;
	case kRenderNormal:
	default:
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		break;
	}

	GX_SetNumChans( 1 );
	GX_SetChanCtrl( GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE );
	GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
	GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
	GX_SetAlphaCompare( GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0 );

	if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
	{
		color[0] = (float)e->curstate.rendercolor.r * ( 1.0f / 255.0f );
		color[1] = (float)e->curstate.rendercolor.g * ( 1.0f / 255.0f );
		color[2] = (float)e->curstate.rendercolor.b * ( 1.0f / 255.0f );
	}
	else
	{
		color[0] = 1.0f;
		color[1] = 1.0f;
		color[2] = 1.0f;
	}

	if( R_SpriteHasLightmap( e, psprite->texFormat ))
	{
		colorVec lightColor = R_LightPoint( origin );
		color2[0] = (float)lightColor.r * ( 1.0f / 255.0f );
		color2[1] = (float)lightColor.g * ( 1.0f / 255.0f );
		color2[2] = (float)lightColor.b * ( 1.0f / 255.0f );
		GX_SetAlphaCompare( GX_GREATER, (u8)((1.0f / 3.0f) * 255.0f), GX_AOP_AND, GX_ALWAYS, 0 );
	}

	mspriteframe_t *frame = NULL, *oldframe = NULL;
	float lerp = 1.0f;
	if( R_SpriteAllowLerping( e, psprite ))
		lerp = R_GetSpriteFrameInterpolant( e, &oldframe, &frame );
	else frame = oldframe = gEngfuncs.R_GetSpriteFrame( model, e->curstate.frame, e->angles[YAW] );

	int type = psprite->type;

	if( e->angles[ROLL] != 0.0f && type == SPR_FWD_PARALLEL )
		type = SPR_FWD_PARALLEL_ORIENTED;

	switch( type )
	{
	case SPR_ORIENTED:
	{
		vec3_t v_forward;
		AngleVectors( e->angles, v_forward, v_right, v_up );
		VectorScale( v_forward, 0.01f, v_forward );
		VectorSubtract( origin, v_forward, origin );
		break;
	}
	case SPR_FACING_UPRIGHT:
		VectorSet( v_right, origin[1] - RI.rvp.vieworigin[1], -(origin[0] - RI.rvp.vieworigin[0]), 0.0f );
		VectorSet( v_up, 0.0f, 0.0f, 1.0f );
		VectorNormalize( v_right );
		break;
	case SPR_FWD_PARALLEL_UPRIGHT:
	{
		float dot = RI.vforward[2];
		if(( dot > 0.999848f ) || ( dot < -0.999848f ))
			return;
		VectorSet( v_up, 0.0f, 0.0f, 1.0f );
		VectorSet( v_right, RI.vforward[1], -RI.vforward[0], 0.0f );
		VectorNormalize( v_right );
		break;
	}
	case SPR_FWD_PARALLEL_ORIENTED:
	{
		float angle = e->angles[ROLL] * (M_PI2 / 360.0f);
		float sr, cr;
		SinCos( angle, &sr, &cr );
		for( int i = 0; i < 3; i++ )
		{
			v_right[i] = (RI.vright[i] * cr + RI.vup[i] * sr);
			v_up[i] = RI.vright[i] * -sr + RI.vup[i] * cr;
		}
		break;
	}
	case SPR_FWD_PARALLEL:
	default:
		VectorCopy( RI.vright, v_right );
		VectorCopy( RI.vup, v_up );
		break;
	}

	/* el culling nunca se hereda: en GL el estado normal es GL_FRONT, y los sprites con cara
	 * unica desaparecian segun lo que se hubiera dibujado antes (orden por distancia = titileo) */
	GX_Cull( psprite->facecull == SPR_CULL_NONE ? XASH_CULL_NONE : XASH_CULL_FRONT );

	GXColor matColor;
	matColor.r = (u8)(color[0] * 255.0f);
	matColor.g = (u8)(color[1] * 255.0f);
	matColor.b = (u8)(color[2] * 255.0f);
	matColor.a = (u8)(tr.blend * 255.0f);
	GX_SetChanMatColor( GX_COLOR0A0, matColor );

	if( oldframe == frame )
	{
		GX_Bind( XASH_TEXTURE0, frame->gl_texturenum );
		GX_DrawSpriteQuad( frame, origin, v_right, v_up, scale );
	}
	else
	{
		lerp = bound( 0.0f, lerp, 1.0f );
		float ilerp = 1.0f - lerp;

		if( ilerp != 0.0f )
		{
			matColor.a = (u8)(tr.blend * ilerp * 255.0f);
			GX_SetChanMatColor( GX_COLOR0A0, matColor );
			GX_Bind( XASH_TEXTURE0, oldframe->gl_texturenum );
			GX_DrawSpriteQuad( oldframe, origin, v_right, v_up, scale );
		}

		if( lerp != 0.0f )
		{
			matColor.a = (u8)(tr.blend * lerp * 255.0f);
			GX_SetChanMatColor( GX_COLOR0A0, matColor );
			GX_Bind( XASH_TEXTURE0, frame->gl_texturenum );
			GX_DrawSpriteQuad( frame, origin, v_right, v_up, scale );
		}
	}

	if( R_SpriteHasLightmap( e, psprite->texFormat ))
	{
		GX_SetBlendMode( GX_BM_BLEND, GX_BL_ZERO, GX_BL_SRCCLR, GX_LO_CLEAR );
		GX_SetZMode( GX_TRUE, GX_EQUAL, GX_TRUE );
		GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
		GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );

		matColor.r = (u8)(color2[0] * 255.0f);
		matColor.g = (u8)(color2[1] * 255.0f);
		matColor.b = (u8)(color2[2] * 255.0f);
		matColor.a = (u8)(tr.blend * 255.0f);
		GX_SetChanMatColor( GX_COLOR0A0, matColor );

		GX_Bind( XASH_TEXTURE0, tr.whiteTexture );
		GX_DrawSpriteQuad( frame, origin, v_right, v_up, scale );

		GX_SetAlphaCompare( GX_GREATER, (u8)(DEFAULT_ALPHATEST * 255.0f), GX_AOP_AND, GX_ALWAYS, 0 );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
	}

	GX_Cull( XASH_CULL_FRONT );

	GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );
	GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );

	if( e->curstate.rendermode == kRenderGlow || e->curstate.rendermode == kRenderTransAdd )
		R_AllowFog( true );

	if( e->curstate.rendermode != kRenderNormal )
	{
		GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
		GX_SetTevOp( GX_TEVSTAGE0, GX_REPLACE );
		GX_SetTevOrder( GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0 );
		GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
	}
}
void R_DrawSpriteModel( cl_entity_t *e )
{
	tr.modelviewIdentity = false;	// recargar siempre la matriz del mundo
	/* los sprites pegados a la pared (humo, impactos) peleaban con la profundidad */
	qboolean bias = gl_polyoffset.value && e->curstate.rendermode != kRenderGlow;

	if( bias ) GX_PushPolygonOffset( -1.0f, -gl_polyoffset.value * 2.0f );
	R_DrawSpriteModelInt( e );
	if( bias ) GX_PopPolygonOffset();
}
