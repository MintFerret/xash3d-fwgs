/*
gx_rmain.c - renderer main loop (Wii GX port)
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
#include "xash3d_mathlib.h"
#include "library.h"
#include "beamdef.h"
#include "entity_types.h"

/* Contador de rendimiento para cl_showfps 3 */
extern double g_gx_frame_time_ms;
extern double g_gx_cpu_time_ms;


#include <gccore.h>
#include <ogc/gx.h>

extern void GX_DbgWait( const char *tag );
extern void GX_MemReport( const char *tag );

#define GXT( s ) do { static int _n; if( _n++ < 1 ) GX_LOG( "[GX] " s "\n" ); } while( 0 )

#define IsLiquidContents( cnt ) ( cnt == CONTENTS_WATER || cnt == CONTENTS_SLIME || cnt == CONTENTS_LAVA )

float           gldepthmin, gldepthmax;
extern int      gx_testmode;
ref_instance_t  RI;

float gx_pending_texcoord[MAX_TEXTURE_UNITS][2];

static Mtx44 gx_projection_mtx;
static u8    gx_projection_type = GX_PERSPECTIVE;

void GX_SaveProjectionMtx( const Mtx44 src, u8 type )
{
    memcpy( gx_projection_mtx, src, sizeof( Mtx44 ));
    gx_projection_type = type;
}

void GX_ReloadProjectionMtx( float zbias )
{
    Mtx44 biased;
    memcpy( biased, gx_projection_mtx, sizeof( Mtx44 ));
    /* zbias > 0 acerca a la camara. En perspectiva el desplazamiento va en [2][2]
     * (constante en espacio NDC, como glPolygonOffset); en ortografica en [2][3] */
    if( gx_projection_type == GX_PERSPECTIVE )
        biased[2][2] += zbias;
    else
        biased[2][3] -= zbias;
    GX_LoadProjectionMtx( biased, gx_projection_type );
}

/*
 * The engine's matrix4x4 is row-major with the translation in column 3
 * (m[0][3], m[1][3], m[2][3]) and a GL-style clip space (z in [-1, 1]).
 * GX's Mtx/Mtx44 use the very same row-major layout, so the matrix must be
 * copied as is, NOT transposed. The only difference is the depth range: GX
 * clips z to [-w, 0], so remap z_gx = ( z_gl - w ) / 2.
 */
void Matrix4x4_ToMtx44( Mtx44 dst, const matrix4x4 src )
{
    for( int r = 0; r < 4; r++ )
        for( int c = 0; c < 4; c++ )
            dst[r][c] = src[r][c];

    for( int c = 0; c < 4; c++ )
        dst[2][c] = ( src[2][c] - src[3][c] ) * 0.5f;
}

void Matrix4x4_ToMtx( Mtx dst, const matrix4x4 src )
{
    for( int r = 0; r < 3; r++ )
        for( int c = 0; c < 4; c++ )
            dst[r][c] = src[r][c];
}

static void GX_EnableFog( qboolean enable,
                           float fogStart, float fogEnd,
                           float r, float g, float b, float a,
                           u8 fogType )
{
    if( enable )
    {
        GXColor fogColor = {
            (u8)(r * 255.0f),
            (u8)(g * 255.0f),
            (u8)(b * 255.0f),
            (u8)(a * 255.0f)
        };
        GX_SetFog( fogType, fogStart, fogEnd, 4.0f, Q_max( 256.0f, RI.farClip ), fogColor );
    }
    else
    {
        GXColor black = { 0, 0, 0, 0 };
        GX_SetFog( GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, black );
    }
}

static int R_RankForRenderMode( int rendermode )
{
    switch( rendermode )
    {
    case kRenderTransTexture:
        return 1;
    case kRenderTransAdd:
        return 2;
    case kRenderGlow:
        return 3;
    }
    return 0;
}

void R_AllowFog( qboolean allowed )
{
    if( allowed )
    {
        if( glState.isFogEnabled && gl_fog.value )
        {
            u8 fogType = FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE )
                         ? GX_FOG_EXP2 : GX_FOG_EXP;
            float endZ = ( RI.fogDensity > 0.0f ) ? ( 4.605f / RI.fogDensity ) : 8192.0f;
            GX_EnableFog( true, 0.0f, endZ,
                          RI.fogColor[0], RI.fogColor[1], RI.fogColor[2], RI.fogColor[3],
                          fogType );
        }
    }
    else
    {
        if( glState.isFogEnabled )
            GX_EnableFog( false, 0, 0, 0, 0, 0, 0, GX_FOG_NONE );
    }
}

qboolean R_OpaqueEntity( cl_entity_t *ent )
{
    if( R_GetEntityRenderMode( ent ) == kRenderNormal )
    {
        switch( ent->curstate.renderfx )
        {
        case kRenderFxNone:
        case kRenderFxDeadPlayer:
        case kRenderFxLightMultiplier:
        case kRenderFxExplode:
            return true;
        }
    }
    return false;
}

static int R_TransEntityCompare( const void *a, const void *b )
{
    cl_entity_t *ent1 = *(cl_entity_t **)a;
    cl_entity_t *ent2 = *(cl_entity_t **)b;
    int rendermode1 = R_GetEntityRenderMode( ent1 );
    int rendermode2 = R_GetEntityRenderMode( ent2 );

    vec3_t vecLen, org;
    float dist1, dist2;

    if( ent1->model->type != mod_brush || rendermode1 != kRenderTransAlpha )
    {
        VectorAverage( ent1->model->mins, ent1->model->maxs, org );
        VectorAdd( ent1->origin, org, org );
        VectorSubtract( RI.rvp.vieworigin, org, vecLen );
        dist1 = DotProduct( vecLen, vecLen );
    }
    else dist1 = 1000000000;

    if( ent2->model->type != mod_brush || rendermode2 != kRenderTransAlpha )
    {
        VectorAverage( ent2->model->mins, ent2->model->maxs, org );
        VectorAdd( ent2->origin, org, org );
        VectorSubtract( RI.rvp.vieworigin, org, vecLen );
        dist2 = DotProduct( vecLen, vecLen );
    }
    else dist2 = 1000000000;

    if( dist1 > dist2 ) return -1;
    if( dist1 < dist2 ) return 1;

    if( R_RankForRenderMode( rendermode1 ) > R_RankForRenderMode( rendermode2 )) return 1;
    if( R_RankForRenderMode( rendermode1 ) < R_RankForRenderMode( rendermode2 )) return -1;

    return 0;
}

int R_WorldToScreen( const vec3_t point, vec3_t screen )
{
    if( !point || !screen )
        return true;

    matrix4x4 worldToScreen;
    Matrix4x4_Copy( worldToScreen, RI.worldviewProjectionMatrix );
    screen[0] = worldToScreen[0][0] * point[0] + worldToScreen[0][1] * point[1] + worldToScreen[0][2] * point[2] + worldToScreen[0][3];
    screen[1] = worldToScreen[1][0] * point[0] + worldToScreen[1][1] * point[1] + worldToScreen[1][2] * point[2] + worldToScreen[1][3];
    float w   = worldToScreen[3][0] * point[0] + worldToScreen[3][1] * point[1] + worldToScreen[3][2] * point[2] + worldToScreen[3][3];
    screen[2] = 0.0f;

    qboolean behind;
    if( w < 0.001f )
    {
        behind = true;
    }
    else
    {
        float invw = 1.0f / w;
        screen[0] *= invw;
        screen[1] *= invw;
        behind = false;
    }

    return behind;
}

void R_ScreenToWorld( const vec3_t screen, vec3_t point )
{
    if( !point || !screen )
        return;

    matrix4x4 screenToWorld;
    Matrix4x4_Invert_Full( screenToWorld, RI.worldviewProjectionMatrix );

    point[0] = screen[0] * screenToWorld[0][0] + screen[1] * screenToWorld[0][1] + screen[2] * screenToWorld[0][2] + screenToWorld[0][3];
    point[1] = screen[0] * screenToWorld[1][0] + screen[1] * screenToWorld[1][1] + screen[2] * screenToWorld[1][2] + screenToWorld[1][3];
    point[2] = screen[0] * screenToWorld[2][0] + screen[1] * screenToWorld[2][1] + screen[2] * screenToWorld[2][2] + screenToWorld[2][3];
    float w  = screen[0] * screenToWorld[3][0] + screen[1] * screenToWorld[3][1] + screen[2] * screenToWorld[3][2] + screenToWorld[3][3];
    if( w != 0.0f ) VectorScale( point, ( 1.0f / w ), point );
}

void R_PushScene( void )
{
    if( ++tr.draw_stack_pos >= MAX_DRAW_STACK )
        gEngfuncs.Host_Error( "draw stack overflow\n" );
    tr.draw_list = &tr.draw_stack[tr.draw_stack_pos];
}

void R_PopScene( void )
{
    if( --tr.draw_stack_pos < 0 )
        gEngfuncs.Host_Error( "draw stack underflow\n" );
    tr.draw_list = &tr.draw_stack[tr.draw_stack_pos];
}

void R_ClearScene( void )
{
    tr.draw_list->num_solid_entities = 0;
    tr.draw_list->num_trans_entities = 0;
    tr.draw_list->num_beam_entities  = 0;

    if( gEngfuncs.drawFuncs->R_ClearScene != NULL )
        gEngfuncs.drawFuncs->R_ClearScene();
}

qboolean R_AddEntity( struct cl_entity_s *clent, int type )
{
    if( !r_drawentities->value )
        return false;

    if( FBitSet( clent->curstate.effects, EF_NODRAW ))
        return false;

    if( !R_ModelOpaque( clent->curstate.rendermode ) && CL_FxBlend( clent ) <= 0 )
        return true;

    switch( type )
    {
    case ET_FRAGMENTED:
        r_stats.c_client_ents++;
        break;
    case ET_TEMPENTITY:
        r_stats.c_active_tents_count++;
        break;
    }

    if( type == ET_BEAM )
    {
        if( tr.draw_list->num_beam_entities >= MAX_VISIBLE_PACKET )
        {
            gEngfuncs.Con_Printf( S_ERROR "Too many beams %d!\n", tr.draw_list->num_beam_entities );
            return false;
        }
        tr.draw_list->beam_entities[tr.draw_list->num_beam_entities] = clent;
        tr.draw_list->num_beam_entities++;
        return true;
    }
    else if( R_OpaqueEntity( clent ))
    {
        if( tr.draw_list->num_solid_entities >= MAX_VISIBLE_PACKET )
            return false;
        tr.draw_list->solid_entities[tr.draw_list->num_solid_entities] = clent;
        tr.draw_list->num_solid_entities++;
    }
    else
    {
        if( tr.draw_list->num_trans_entities >= MAX_VISIBLE_PACKET )
            return false;
        tr.draw_list->trans_entities[tr.draw_list->num_trans_entities] = clent;
        tr.draw_list->num_trans_entities++;
    }

    return true;
}

static void R_Clear( int bitMask )
{
    GXColor clearColor;

    if( ENGINE_GET_PARM( PARM_DEV_OVERVIEW ))
    {
        clearColor.r = 0;   clearColor.g = 255;
        clearColor.b = 0;   clearColor.a = 255;
    }
    else
    {
        clearColor.r = 127; clearColor.g = 127;
        clearColor.b = 127; clearColor.a = 255;
    }

    u32 clearZ;

    if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
    {
        gldepthmin = 1.0f;
        gldepthmax = 0.0f;
        clearZ = 0x000000;
    }
    else
    {
        gldepthmin = 0.0f;
        gldepthmax = 1.0f;
        clearZ = 0xFFFFFF;
    }

    GX_SetCopyClear( clearColor, clearZ );
    GX_SetZMode( GX_TRUE, GX_LEQUAL, GX_TRUE );
}

static float R_GetFarClip( void )
{
    if( WORLDMODEL && FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
        return gp_movevars->zmax * 1.73f;
    return 2048.0f;
}

void R_SetupFrustum( void )
{
	/* RI.vforward/vright/vup nunca se calculaban: particulas, sprites, beams y el frustum quedaban con vectores en cero */
	AngleVectors( RI.rvp.viewangles, RI.vforward, RI.vright, RI.vup );


    if( !r_lockfrustum.value )
    {
        VectorCopy( RI.rvp.vieworigin, RI.cullorigin );
        VectorCopy( RI.vforward, RI.cull_vforward );
        VectorCopy( RI.vright,   RI.cull_vright );
        VectorCopy( RI.vup,      RI.cull_vup );
    }

    if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
    {
        const ref_overview_t *ov = gEngfuncs.GetOverviewParms();
        GL_FrustumInitOrtho( &RI.frustum, ov->xLeft, ov->xRight, ov->yTop, ov->yBottom, ov->zNear, ov->zFar );
    }
    else
    {
        GL_FrustumInitProj( &RI.frustum, 0.0f, R_GetFarClip(), RI.rvp.fov_x, RI.rvp.fov_y );
    }
}

static void R_SetupProjectionMatrix( matrix4x4 m )
{
    if( FBitSet( RI.rvp.flags, RF_DRAW_OVERVIEW ))
    {
        const ref_overview_t *ov = gEngfuncs.GetOverviewParms();
        Matrix4x4_CreateOrtho( m, ov->xLeft, ov->xRight, ov->yTop, ov->yBottom, ov->zNear, ov->zFar );

        Mtx44 gxProj;
        Matrix4x4_ToMtx44( gxProj, m );
        GX_LoadProjectionMtx( gxProj, GX_ORTHOGRAPHIC );
        GX_SaveProjectionMtx( gxProj, GX_ORTHOGRAPHIC );
        return;
    }

    RI.farClip = R_GetFarClip();

    float zNear = 4.0f;
    float zFar  = Q_max( 256.0f, RI.farClip );

    float yMax = zNear * tan( RI.rvp.fov_y * M_PI_F / 360.0f );
    float yMin = -yMax;
    float xMax = zNear * tan( RI.rvp.fov_x * M_PI_F / 360.0f );
    float xMin = -xMax;

    if( tr.rotation & 1 )
        Matrix4x4_CreateProjection( m, yMax, yMin, xMax, xMin, zNear, zFar );
    else
        Matrix4x4_CreateProjection( m, xMax, xMin, yMax, yMin, zNear, zFar );

    Mtx44 gxProj;
    Matrix4x4_ToMtx44( gxProj, m );
    GX_LoadProjectionMtx( gxProj, GX_PERSPECTIVE );
    GX_SaveProjectionMtx( gxProj, GX_PERSPECTIVE );
}

static void R_SetupModelviewMatrix( matrix4x4 m )
{
    Matrix4x4_CreateModelview( m );
    if( tr.rotation & 1 )
    {
        Matrix4x4_ConcatRotate( m, anglemod( -RI.rvp.viewangles[2] + 90 ), 1, 0, 0 );
        Matrix4x4_ConcatRotate( m, -RI.rvp.viewangles[0], 0, 1, 0 );
        Matrix4x4_ConcatRotate( m, -RI.rvp.viewangles[1], 0, 0, 1 );
    }
    else
    {
        Matrix4x4_ConcatRotate( m, -RI.rvp.viewangles[2], 1, 0, 0 );
        Matrix4x4_ConcatRotate( m, -RI.rvp.viewangles[0], 0, 1, 0 );
        Matrix4x4_ConcatRotate( m, -RI.rvp.viewangles[1], 0, 0, 1 );
    }
    Matrix4x4_ConcatTranslate( m, -RI.rvp.vieworigin[0], -RI.rvp.vieworigin[1], -RI.rvp.vieworigin[2] );

    Mtx gxMv;
    Matrix4x4_ToMtx( gxMv, m );
    GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );
}

void R_LoadIdentity( void )
{
    if( tr.modelviewIdentity ) return;

    Matrix4x4_LoadIdentity( RI.objectMatrix );
    Matrix4x4_Copy( RI.modelviewMatrix, RI.worldviewMatrix );

    Mtx gxMv;
    Matrix4x4_ToMtx( gxMv, RI.modelviewMatrix );
    GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );

    tr.modelviewIdentity = true;
}

void R_RotateForEntity( cl_entity_t *e )
{
    float scale = 1.0f;

    if( e == CL_GetEntityByIndex( 0 ))
    {
        R_LoadIdentity();
        return;
    }

    if( e->model->type != mod_brush && e->curstate.scale > 0.0f )
        scale = e->curstate.scale;

    Matrix4x4_CreateFromEntity( RI.objectMatrix, e->angles, e->origin, scale );
    Matrix4x4_ConcatTransforms( RI.modelviewMatrix, RI.worldviewMatrix, RI.objectMatrix );

    Mtx gxMv;
    Matrix4x4_ToMtx( gxMv, RI.modelviewMatrix );
    GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );

    tr.modelviewIdentity = false;
}

void R_TranslateForEntity( cl_entity_t *e )
{
    float scale = 1.0f;

    if( e == CL_GetEntityByIndex( 0 ))
    {
        R_LoadIdentity();
        return;
    }

    if( e->model->type != mod_brush && e->curstate.scale > 0.0f )
        scale = e->curstate.scale;

    Matrix4x4_CreateFromEntity( RI.objectMatrix, vec3_origin, e->origin, scale );
    Matrix4x4_ConcatTransforms( RI.modelviewMatrix, RI.worldviewMatrix, RI.objectMatrix );

    Mtx gxMv;
    Matrix4x4_ToMtx( gxMv, RI.modelviewMatrix );
    GX_LoadPosMtxImm( gxMv, GX_PNMTX0 );

    tr.modelviewIdentity = false;
}

void R_FindViewLeaf( void )
{
    RI.oldviewleaf = RI.viewleaf;
    RI.viewleaf = gEngfuncs.Mod_PointInLeaf( RI.rvp.vieworigin, WORLDMODEL->nodes, WORLDMODEL );
}

static void R_SetupFrame( void )
{
    RI.viewplanedist = DotProduct( RI.rvp.vieworigin, RI.vforward );

    if( !gl_nosort.value )
    {
        qsort( tr.draw_list->trans_entities, tr.draw_list->num_trans_entities,

               sizeof( cl_entity_t* ), R_TransEntityCompare );
    }

    if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
        R_FindViewLeaf();
}

// ultimo viewport aplicado (x, y, ancho, alto), para poder cambiar solo el rango Z
static f32 gx_vp[4];

void GX_SetDepthRange( float zmin, float zmax )
{
    if( gx_vp[2] <= 0.0f || gx_vp[3] <= 0.0f )
        return;
    GX_SetViewport( gx_vp[0], gx_vp[1], gx_vp[2], gx_vp[3], zmin, zmax );
}

void R_SetupGL( qboolean set_gl_state )
{
    R_SetupModelviewMatrix( RI.worldviewMatrix );
    R_SetupProjectionMatrix( RI.projectionMatrix );

    Matrix4x4_Concat( RI.worldviewProjectionMatrix, RI.projectionMatrix, RI.worldviewMatrix );

    if( !set_gl_state ) return;

    if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
    {
        int x  = floor( RI.rvp.viewport[0] * gpGlobals->width  / gpGlobals->width );
        int x2 = ceil(( RI.rvp.viewport[0] + RI.rvp.viewport[2] ) * gpGlobals->width  / gpGlobals->width );
        int y  = floor( gpGlobals->height - RI.rvp.viewport[1] * gpGlobals->height / gpGlobals->height );
        int y2 = ceil(  gpGlobals->height - ( RI.rvp.viewport[1] + RI.rvp.viewport[3] ) * gpGlobals->height / gpGlobals->height );

        if( tr.rotation & 1 )
        {
            gx_vp[0] = (f32)y2; gx_vp[1] = (f32)x; gx_vp[2] = (f32)(y - y2); gx_vp[3] = (f32)(x2 - x);
        }
        else
        {
            gx_vp[0] = (f32)x; gx_vp[1] = (f32)y2; gx_vp[2] = (f32)(x2 - x); gx_vp[3] = (f32)(y - y2);
        }
        GX_SetViewport( gx_vp[0], gx_vp[1], gx_vp[2], gx_vp[3], gldepthmin, gldepthmax );
    }
    else
    {
        gx_vp[0] = (f32)RI.rvp.viewport[0]; gx_vp[1] = (f32)RI.rvp.viewport[1];
        gx_vp[2] = (f32)RI.rvp.viewport[2]; gx_vp[3] = (f32)RI.rvp.viewport[3];
        GX_SetViewport( gx_vp[0], gx_vp[1], gx_vp[2], gx_vp[3], gldepthmin, gldepthmax );
    }

    GX_SetCullMode( GX_CULL_FRONT );
    GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );
    GX_SetAlphaCompare( GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0 );

    GXColor white = { 255, 255, 255, 255 };
    GX_SetChanMatColor( GX_COLOR0A0, white );
}

static gl_texture_t *R_RecursiveFindWaterTexture( const mnode_t *node, const mnode_t *ignore, qboolean down )
{
    gl_texture_t *tex = NULL;

    Assert( node != NULL );

    if( node->contents == CONTENTS_SOLID )
        return NULL;

    if( node->contents < 0 )
    {
        if( node->contents != CONTENTS_WATER && node->contents != CONTENTS_LAVA && node->contents != CONTENTS_SLIME )
            return NULL;

        mleaf_t *pleaf = (mleaf_t *)node;
        msurface_t **mark = pleaf->firstmarksurface;
        int c = pleaf->nummarksurfaces;

        for( int i = 0; i < c; i++, mark++ )
        {
            if( (*mark)->flags & SURF_DRAWTURB && (*mark)->texinfo && (*mark)->texinfo->texture )
                return R_GetTexture( (*mark)->texinfo->texture->gl_texturenum );
        }
        return NULL;
    }

    mnode_t *child = node_child( node, 0, WORLDMODEL );
    if( child && ( child != ignore ))
    {
        tex = R_RecursiveFindWaterTexture( child, node, true );
        if( tex ) return tex;
    }

    child = node_child( node, 1, WORLDMODEL );
    if( child && ( child != ignore ))
    {
        tex = R_RecursiveFindWaterTexture( child, node, true );
        if( tex ) return tex;
    }

    if( down ) return NULL;

    if( node->parent )
        return R_RecursiveFindWaterTexture( node->parent, node, false );

    return NULL;
}

static void R_CheckFog( void )
{
    if( FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE ))
    {
        if( !gp_movevars->fog_settings )
        {
            if( glState.isFogEnabled )
                GX_EnableFog( false, 0, 0, 0, 0, 0, 0, GX_FOG_NONE );
            glState.isFogEnabled = false;
            RI.fogEnabled = false;
            return;
        }

        RI.fogColor[0] = ((gp_movevars->fog_settings & 0xFF000000) >> 24) / 255.0f;
        RI.fogColor[1] = ((gp_movevars->fog_settings & 0xFF0000)   >> 16) / 255.0f;
        RI.fogColor[2] = ((gp_movevars->fog_settings & 0xFF00)     >>  8) / 255.0f;
        RI.fogDensity  = ((gp_movevars->fog_settings & 0xFF) / 255.0f) * 0.01f;
        RI.fogStart    = RI.fogEnd = 0.0f;
        RI.fogColor[3] = 1.0f;
        RI.fogCustom   = false;
        RI.fogEnabled  = true;
        RI.fogSkybox   = true;
        return;
    }

    RI.fogEnabled = false;

    if( FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ) || ENGINE_GET_PARM( PARM_WATER_LEVEL ) < 3
        || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) || !RI.viewleaf )
    {
        if( RI.cached_waterlevel == 3 )
        {
            RI.cached_waterlevel = ENGINE_GET_PARM( PARM_WATER_LEVEL );
            RI.cached_contents   = CONTENTS_EMPTY;
            if( !RI.fogCustom )
            {
                glState.isFogEnabled = false;
                GX_EnableFog( false, 0, 0, 0, 0, 0, 0, GX_FOG_NONE );
            }
        }
        return;
    }

    cl_entity_t *ent = gEngfuncs.CL_GetWaterEntity( RI.rvp.vieworigin );
    int cnt;
    if( ent && ent->model && ent->model->type == mod_brush && ent->curstate.skin < 0 )
        cnt = ent->curstate.skin;
    else cnt = RI.viewleaf->contents;

    RI.cached_waterlevel = ENGINE_GET_PARM( PARM_WATER_LEVEL );

    if( !IsLiquidContents( RI.cached_contents ) && IsLiquidContents( cnt ))
    {
        gl_texture_t *tex = NULL;

        if( ent && ent->model && ent->model->type == mod_brush )
        {
            int count = ent->model->nummodelsurfaces;
            msurface_t *surf = &ent->model->surfaces[ent->model->firstmodelsurface];
            for( int i = 0; i < count; i++, surf++ )
            {
                if( surf->flags & SURF_DRAWTURB && surf->texinfo && surf->texinfo->texture )
                {
                    tex = R_GetTexture( surf->texinfo->texture->gl_texturenum );
                    RI.cached_contents = ent->curstate.skin;
                    break;
                }
            }
        }
        else
        {
            tex = R_RecursiveFindWaterTexture( RI.viewleaf->parent, NULL, false );
            if( tex ) RI.cached_contents = RI.viewleaf->contents;
        }

        if( !tex ) return;

        RI.fogColor[0] = tex->fogParams[0] / 255.0f;
        RI.fogColor[1] = tex->fogParams[1] / 255.0f;
        RI.fogColor[2] = tex->fogParams[2] / 255.0f;
        RI.fogDensity  = tex->fogParams[3] * 0.000025f;
        RI.fogStart    = RI.fogEnd = 0.0f;
        RI.fogColor[3] = 1.0f;
        RI.fogCustom   = false;
        RI.fogEnabled  = true;
        RI.fogSkybox   = true;
    }
    else
    {
        RI.fogCustom  = false;
        RI.fogEnabled = true;
        RI.fogSkybox  = true;
    }
}

static void R_CheckGLFog( void )
{
#ifdef HACKS_RELATED_HLMODS
    if(( !RI.fogEnabled && !RI.fogCustom ) && glState.isFogEnabled && VectorIsNull( RI.fogColor ))
    {
        RI.fogSkybox = true;
    }
#endif
}

void R_DrawFog( void )
{
    if( !RI.fogEnabled || !gl_fog.value )
        return;

    u8 fogType = FBitSet( gp_host->features, ENGINE_QUAKE_COMPATIBLE )
                 ? GX_FOG_EXP2 : GX_FOG_EXP;

    float endZ = ( RI.fogDensity > 0.0f ) ? ( 4.605f / RI.fogDensity ) : 8192.0f;

    GX_EnableFog( true, 0.0f, endZ,
                  RI.fogColor[0], RI.fogColor[1], RI.fogColor[2], RI.fogColor[3],
                  fogType );

    glState.isFogEnabled = true;
}

static void R_DrawEntitiesOnList( void )
{
    tr.blend = 1.0f;

    for( int i = 0; i < tr.draw_list->num_solid_entities && !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ); i++ )
    {
        RI.currententity = tr.draw_list->solid_entities[i];
        RI.currentmodel  = RI.currententity->model;

        if( !RI.currentmodel && RI.currententity->player && !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
            continue;

        Assert( RI.currententity != NULL );
        Assert( RI.currentmodel  != NULL );

        switch( RI.currentmodel->type )
        {
        case mod_brush:  R_DrawBrushModel(  RI.currententity ); break;
        case mod_alias:  R_DrawAliasModel(  RI.currententity ); break;
        case mod_studio: R_DrawStudioModel( RI.currententity ); break;
        default: break;
        }
    }

    R_DrawAlphaTextureChains();

    for( int i = 0; i < tr.draw_list->num_solid_entities && !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ); i++ )
    {
        RI.currententity = tr.draw_list->solid_entities[i];
        RI.currentmodel  = RI.currententity->model;

        if( !RI.currentmodel && RI.currententity->player && !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
            continue;

        Assert( RI.currententity != NULL );
        Assert( RI.currentmodel  != NULL );

        if( RI.currentmodel->type == mod_sprite )
            R_DrawSpriteModel( RI.currententity );
    }

    if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
        gEngfuncs.CL_DrawEFX( tr.frametime, false );

    if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
        gEngfuncs.pfnDrawNormalTriangles();

    for( int i = 0; i < tr.draw_list->num_trans_entities && !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ); i++ )
    {
        RI.currententity = tr.draw_list->trans_entities[i];
        RI.currentmodel  = RI.currententity->model;

        if( RI.currententity->curstate.rendermode != kRenderNormal )
            tr.blend = CL_FxBlend( RI.currententity ) / 255.0f;
        else tr.blend = 1.0f;

        if( tr.blend <= 0.0f ) continue;

        if( !RI.currentmodel && RI.currententity->player && !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
            continue;

        Assert( RI.currententity != NULL );
        Assert( RI.currentmodel  != NULL );

        switch( RI.currentmodel->type )
        {
        case mod_brush:  R_DrawBrushModel(  RI.currententity ); break;
        case mod_alias:  R_DrawAliasModel(  RI.currententity ); break;
        case mod_studio: R_DrawStudioModel( RI.currententity ); break;
        case mod_sprite: R_DrawSpriteModel( RI.currententity ); break;
        default: break;
        }
    }

    if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
    {
        GX_SetTevOp( GX_TEVSTAGE0, GX_MODULATE );
        gEngfuncs.pfnDrawTransparentTriangles();
    }

    if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
    {
        R_AllowFog( false );
        gEngfuncs.CL_DrawEFX( tr.frametime, true );
        R_AllowFog( true );
    }

    GX_SetBlendMode( GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR );

    if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
        R_DrawViewModel();
    gEngfuncs.CL_ExtraUpdate();
    GXT( "extraupdate ok" );
}

void R_RenderScene( void )
{
	double __t_start = Platform_DoubleTime();
    if( !WORLDMODEL && FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
        gEngfuncs.Host_Error( "%s: NULL worldmodel\n", __func__ );

    if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
        tr.frametime = gp_cl->time - gp_cl->oldtime;
    else tr.frametime = 0.0;

    tr.framecount++;

    /* Red de seguridad para estilos de luz conmutables (32..63: interruptores,
       apagar la luz de una sala). Si el cambio no llega por la actualizacion
       parcial de lightmaps, se reconstruyen todas (lo mismo que hace cambiar el
       brillo en las opciones de video). Como mucho una vez cada ~15 frames. */
    if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) && !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) && r_dynamic->value )
    {
        static int ls_prev[MAX_LIGHTSTYLES];
        static const void *ls_world;
        static int ls_last_rebuild;

        if( ls_world != WORLDMODEL )
        {
            ls_world = WORLDMODEL;
            for( int i = 0; i < MAX_LIGHTSTYLES; i++ ) ls_prev[i] = g_lightstylevalue[i];
        }
        else
        {
            int changed = 0;
            for( int i = 32; i < MAX_LIGHTSTYLES; i++ )
                if( ls_prev[i] != (int)g_lightstylevalue[i] ) changed = 1;

            if( changed && ( tr.framecount - ls_last_rebuild ) > 15 )
            {
                ls_last_rebuild = tr.framecount;
                for( int i = 0; i < MAX_LIGHTSTYLES; i++ ) ls_prev[i] = g_lightstylevalue[i];
                GX_RebuildLightmaps();
            }
        }
    }
    tr.dlightframecount = R_PushDlights( WORLDMODEL, tr.framecount );

    R_SetupFrustum();
    R_SetupFrame();
    R_SetupGL( true );
    R_Clear( ~0 );

    R_MarkLeaves();
    R_DrawFog();
    if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
        R_AnimateRipples();

    R_CheckGLFog();
    R_DrawWorld();
    R_CheckFog();

    gEngfuncs.CL_ExtraUpdate();

    R_DrawEntitiesOnList();
    R_DrawWaterSurfaces();
	g_gx_frame_time_ms = (Platform_DoubleTime() - __t_start) * 1000.0;
}

void R_GammaChanged( qboolean do_reset_gamma )
{
    if( do_reset_gamma )
    {
        if( gEngfuncs.drawFuncs->GL_BuildLightmaps )
            gEngfuncs.drawFuncs->GL_BuildLightmaps();
    }
    else
    {
        glConfig.softwareGammaUpdate = true;
        GX_RebuildLightmaps();
        glConfig.softwareGammaUpdate = false;
    }
}

static void R_CheckCvars( void )
{
    qboolean rebuild = false;

    if( FBitSet( gl_overbright.flags, FCVAR_CHANGED ))
    {
        ClearBits( gl_overbright.flags, FCVAR_CHANGED );
        rebuild = true;
    }

    if( FBitSet( r_vbo.flags, FCVAR_CHANGED ))
    {
        ClearBits( r_vbo.flags, FCVAR_CHANGED );
        R_EnableVBO( r_vbo.value ? true : false );
        if( R_HasEnabledVBO() )
            R_GenerateVBO();
        if( gl_overbright.value )
            rebuild = true;
    }

    if( FBitSet( r_vbo_overbrightmode.flags, FCVAR_CHANGED ) && gl_overbright.value )
    {
        ClearBits( r_vbo_overbrightmode.flags, FCVAR_CHANGED );
        rebuild = true;
    }

    if( rebuild )
        R_GammaChanged( false );
}


void R_BeginFrame( qboolean clearScene )
{
	GX_ResetRenderState();
	static int bf;
	if( bf < 5 ) GX_LOG( "[GX] BeginFrame %d\n", bf );
	bf++;
	GX_DbgWait( "beginframe start" );

    glConfig.softwareGammaUpdate = false;

#if XASH_OGC
    if(( ref_gl_clear->value || ENGINE_GET_PARM( PARM_DEV_OVERVIEW )) &&
        clearScene && ENGINE_GET_PARM( PARM_CONNSTATE ) != ca_cinematic )
#else
    if(( gl_clear->value || ENGINE_GET_PARM( PARM_DEV_OVERVIEW )) &&
        clearScene && ENGINE_GET_PARM( PARM_CONNSTATE ) != ca_cinematic )
#endif
    {
        // Limpiar el EFB con un quad negro de pantalla completa.
        // Sin esto el EFB conserva lo que dibujó el frame anterior
        // (o basura de la inicialización) y la pantalla muestra rayas.
        R_ClearScreen();
    }


    R_CheckCvars();
    GXT( "cvars ok" );
    R_Set2DMode( true );
    GXT( "2d on" );
    GX_DbgWait( "after 2d on" );

    if( FBitSet( gl_texture_nearest.flags | gl_lightmap_nearest.flags |
                 gl_texture_anisotropy.flags | gl_texture_lodbias.flags, FCVAR_CHANGED ))
        R_SetTextureParameters();

    gEngfuncs.CL_ExtraUpdate();
}

void R_SetupRefParams( const ref_viewpass_t *rvp )
{
    RI.rvp     = *rvp;
    RI.farClip = 0;
}

void R_RenderFrame( const ref_viewpass_t *rvp )
{
    if( r_norefresh->value )
        return;

    R_SetupRefParams( rvp );

    if( gl_finish.value && FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
    {
        GX_DrawDone();
    }

    if( gEngfuncs.drawFuncs->GL_RenderFrame != NULL )
    {
        tr.fCustomRendering = true;
        if( gEngfuncs.drawFuncs->GL_RenderFrame( rvp ))
        {
            R_GatherPlayerLight( tr.viewent );
            tr.realframecount++;
            tr.fResetVis = true;
            return;
        }
    }

    tr.fCustomRendering = false;
    if( !FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW ))
        R_RunViewmodelEvents();

    tr.realframecount++;
    R_RenderScene();
}

void R_EndFrame( void )
{
	static int ef;
	if( ef < 5 ) GX_LOG( "[GX] EndFrame %d\n", ef );
	R_Set2DMode( false );
	gEngfuncs.GL_SwapBuffers();
	if( ef < 5 ) GX_LOG( "[GX] EndFrame %d done\n", ef );
	ef++;
}

void R_DrawCubemapView( const vec3_t origin, const vec3_t angles, int size )
{
    ref_viewpass_t rvp;

    rvp.flags = rvp.viewentity = 0;
    SetBits( rvp.flags, RF_DRAW_WORLD );
    SetBits( rvp.flags, RF_DRAW_CUBEMAP );

    rvp.viewport[0] = rvp.viewport[1] = 0;
    rvp.viewport[2] = rvp.viewport[3] = size;
    rvp.fov_x = rvp.fov_y = 90.0f;

    VectorCopy( origin, rvp.vieworigin );
    VectorCopy( angles, rvp.viewangles );

    R_RenderFrame( &rvp );

    RI.viewleaf = NULL;
}

int CL_FxBlend( cl_entity_t *e )
{
    int blend = 0;
    float offset = ((int)e->index) * 363.0f;

    switch( e->curstate.renderfx )
    {
    case kRenderFxPulseSlowWide:
        blend = e->curstate.renderamt + 0x40 * sin( gp_cl->time * 2 + offset );
        break;
    case kRenderFxPulseFastWide:
        blend = e->curstate.renderamt + 0x40 * sin( gp_cl->time * 8 + offset );
        break;
    case kRenderFxPulseSlow:
        blend = e->curstate.renderamt + 0x10 * sin( gp_cl->time * 2 + offset );
        break;
    case kRenderFxPulseFast:
        blend = e->curstate.renderamt + 0x10 * sin( gp_cl->time * 8 + offset );
        break;
    case kRenderFxFadeSlow:
        if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
        {
            if( e->curstate.renderamt > 0 ) e->curstate.renderamt -= 1;
            else e->curstate.renderamt = 0;
        }
        blend = e->curstate.renderamt;
        break;
    case kRenderFxFadeFast:
        if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
        {
            if( e->curstate.renderamt > 3 ) e->curstate.renderamt -= 4;
            else e->curstate.renderamt = 0;
        }
        blend = e->curstate.renderamt;
        break;
    case kRenderFxSolidSlow:
        if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
        {
            if( e->curstate.renderamt < 255 ) e->curstate.renderamt += 1;
            else e->curstate.renderamt = 255;
        }
        blend = e->curstate.renderamt;
        break;
    case kRenderFxSolidFast:
        if( !FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
        {
            if( e->curstate.renderamt < 252 ) e->curstate.renderamt += 4;
            else e->curstate.renderamt = 255;
        }
        blend = e->curstate.renderamt;
        break;
    case kRenderFxStrobeSlow:
        blend = 20 * sin( gp_cl->time * 4 + offset );
        if( blend < 0 ) blend = 0;
        else blend = e->curstate.renderamt;
        break;
    case kRenderFxStrobeFast:
        blend = 20 * sin( gp_cl->time * 16 + offset );
        if( blend < 0 ) blend = 0;
        else blend = e->curstate.renderamt;
        break;
    case kRenderFxStrobeFaster:
        blend = 20 * sin( gp_cl->time * 36 + offset );
        if( blend < 0 ) blend = 0;
        else blend = e->curstate.renderamt;
        break;
    case kRenderFxFlickerSlow:
        blend = 20 * (sin( gp_cl->time * 2 ) + sin( gp_cl->time * 17 + offset ));
        if( blend < 0 ) blend = 0;
        else blend = e->curstate.renderamt;
        break;
    case kRenderFxFlickerFast:
        blend = 20 * (sin( gp_cl->time * 16 ) + sin( gp_cl->time * 23 + offset ));
        if( blend < 0 ) blend = 0;
        else blend = e->curstate.renderamt;
        break;
    case kRenderFxHologram:
    case kRenderFxDistort:
    {
        vec3_t tmp = Vec3( e->origin );
        VectorSubtract( tmp, RI.rvp.vieworigin, tmp );
        float dist = DotProduct( tmp, RI.vforward );

        if( e->curstate.renderfx == kRenderFxDistort )
            dist = 1;

        if( dist <= 0 )
        {
            blend = 0;
        }
        else
        {
            e->curstate.renderamt = 180;
            if( dist <= 100 ) blend = e->curstate.renderamt;
            else blend = (int)((1.0f - ( dist - 100 ) * ( 1.0f / 400.0f )) * e->curstate.renderamt);
            blend += gEngfuncs.COM_RandomLong( -32, 31 );
        }
        break;
    }
    default:
        blend = e->curstate.renderamt;
        break;
    }

    blend = bound( 0, blend, 255 );
    return blend;
}