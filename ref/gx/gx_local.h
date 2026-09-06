/*
gx_local.h - renderer local declarations (Wii GX native port)
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

#ifndef GX_LOCAL_H
#define GX_LOCAL_H

#include "ref_common.h"
#include "port.h"
#include "xash3d_types.h"
#include "cvardef.h"
#include "protocol.h"
#include "gl_frustum.h"
#include "ref_params.h"
#include "enginefeatures.h"
#include "com_strings.h"
#include "wadfile.h"
#include "common/mod_local.h"
#include "pmove.h"

#include <gccore.h>
#include <ogc/gx.h>

#ifndef offsetof
#ifdef __GNUC__
#define offsetof(s,m) __builtin_offsetof(s,m)
#else
#define offsetof(s,m) (size_t)&(((s *)0)->m)
#endif
#endif

#include <stdio.h>

#define LM_SAMPLE_SIZE             16

#define BLOCK_SIZE		tr.block_size
#define BLOCK_SIZE_DEFAULT	128
#define BLOCK_SIZE_MAX	1024

#define MAX_TEXTURES            8192
#define MAX_DETAIL_TEXTURES	256
#define MAX_LIGHTMAPS	256
#define SUBDIVIDE_SIZE	64
#define MAX_DECAL_SURFS	4096
#define MAX_DRAW_STACK	2

#define SHADEDOT_QUANT 	16
#define SHADE_LAMBERT	1.4953241
#define DEFAULT_ALPHATEST	0.0f

#define MAX_TEXTURE_UNITS 8

#define XASH_TEXTURE0	0
#define GL_KEEP_UNIT	(-1)

#define XASH_CULL_NONE   0
#define XASH_CULL_FRONT  1
#define XASH_CULL_BACK   2

#ifndef GL_FRONT
#define GL_FRONT         XASH_CULL_FRONT
#define GL_BACK          XASH_CULL_BACK
#define GL_FRONT_AND_BACK 3
#endif

#define R_ModelOpaque( rm )	( rm == kRenderNormal )
#define R_StaticEntity( ent )	( VectorIsNull( ent->origin ) && VectorIsNull( ent->angles ))
#define RP_LOCALCLIENT( e )	((e) != NULL && (e)->index == ( gp_cl->playernum + 1 ) && e->player )

#define CL_IsViewEntityLocalPlayer() ( gp_cl->viewentity == ( gp_cl->playernum + 1 ))

#define CULL_VISIBLE	0
#define CULL_BACKSIDE	1
#define CULL_FRUSTUM	2
#define CULL_VISFRAME	3
#define CULL_OTHER		4

#define HACKS_RELATED_HLMODS
#define SKYBOX_BASE_NUM 5800

typedef struct gltexture_s
{
	char		name[256];
	word		srcWidth;
	word		srcHeight;
	word		width;
	word		height;
	word		depth;
	byte		numMips;

	GXTexObj		texObj;
	void		*nativeData;
	u32		texnum;
	u8      encode;      
   u8		format;
	u8		wrapS;
	u8		wrapT;
	u8		minFilter;
	u8		magFilter;
	u8		tevOp;
	texFlags_t	flags;

	rgba_t		fogParams;
	rgbdata_t		*original;

	size_t		size;

	float		xscale;
	float		yscale;

	uint		hashValue;
	struct gltexture_s	*nextHash;
} gl_texture_t;

typedef struct
{
	ref_viewpass_t rvp;

	cl_entity_t	*currententity;
	model_t		*currentmodel;
	cl_entity_t	*currentbeam;

	gl_frustum_t	frustum;

	mleaf_t		*viewleaf;
	mleaf_t		*oldviewleaf;
	vec3_t		vforward;
	vec3_t		vright;
	vec3_t		vup;

	vec3_t		cullorigin;
	vec3_t		cull_vforward;
	vec3_t		cull_vright;
	vec3_t		cull_vup;

	float		farClip;

	qboolean		fogCustom;
	qboolean		fogEnabled;
	qboolean		fogSkybox;
	vec4_t		fogColor;
	float		fogDensity;
	float		fogStart;
	float		fogEnd;
	int		cached_contents;
	int		cached_waterlevel;

	float		skyMins[2][SKYBOX_MAX_SIDES];
	float		skyMaxs[2][SKYBOX_MAX_SIDES];

	matrix4x4		objectMatrix;
	matrix4x4		worldviewMatrix;
	matrix4x4		modelviewMatrix;
	matrix4x4		projectionMatrix;
	matrix4x4		worldviewProjectionMatrix;
	byte		visbytes[(MAX_MAP_LEAFS+7)/8];

	float		viewplanedist;
	mplane_t		clipPlane;
} ref_instance_t;

typedef struct
{
	cl_entity_t	*solid_entities[MAX_VISIBLE_PACKET];
	cl_entity_t	*trans_entities[MAX_VISIBLE_PACKET];
	cl_entity_t	*beam_entities[MAX_VISIBLE_PACKET];
	uint		num_solid_entities;
	uint		num_trans_entities;
	uint		num_beam_entities;
} draw_list_t;

typedef struct
{
	int		defaultTexture;
	int		particleTexture;
	int		whiteTexture;
	int		grayTexture;
	int		blackTexture;
	int		solidskyTexture;
	int		alphaskyTexture;
	int		lightmapTextures[MAX_LIGHTMAPS];
	int		dlightTexture;
	int		skyboxTextures[SKYBOX_MAX_SIDES];
	int		skytexturenum;
	int		skyboxbasenum;

	draw_list_t	draw_stack[MAX_DRAW_STACK];
	int		draw_stack_pos;
	draw_list_t	*draw_list;

	msurface_t	*draw_decals[MAX_DECAL_SURFS];
	int		num_draw_decals;

	qboolean		modelviewIdentity;

	int		visframecount;
	int		dlightframecount;
	int		realframecount;
	int		framecount;

	qboolean		fCustomRendering;
	qboolean		fResetVis;
	qboolean		fFlipViewModel;

	byte		visbytes[(MAX_MAP_LEAFS+7)/8];
	int		block_size;

	double		frametime;
	float		blend;

	vec3_t		modelorg;

	model_t *worldmodel;
	world_static_t *world;
	cl_entity_t *entities;
	color24 *palette;
	cl_entity_t *viewent;
	dlight_t *elights;
	byte *texgammatable;
	uint16_t *lightgammatable;
	uint16_t *lineargammatable;
	uint16_t *screengammatable;

	uint max_entities;

	ref_screen_rotation_t rotation;
} gl_globals_t;

typedef struct
{
	uint		c_world_polys;
	uint		c_studio_polys;
	uint		c_sprite_polys;
	uint		c_alias_polys;
	uint		c_world_leafs;

	uint		c_view_beams_count;
	uint		c_active_tents_count;
	uint		c_alias_models_drawn;
	uint		c_studio_models_drawn;
	uint		c_sprite_models_drawn;
	uint		c_particle_count;

	uint		c_client_ents;
	double		t_world_node;
	double		t_world_draw;
} ref_speeds_t;

extern ref_speeds_t		r_stats;
extern ref_instance_t	RI;
extern gl_globals_t	tr;

extern float		gldepthmin, gldepthmax;
#define r_numEntities	(tr.draw_list->num_solid_entities + tr.draw_list->num_trans_entities)
#define r_numStatics	(r_stats.c_client_ents)
#define Mod_AllowMaterials() (host_allow_materials->value && !FBitSet( gp_host->features, ENGINE_DISABLE_HDTEXTURES ))

typedef struct
{
	vec3_t xyz;
	vec2_t st;
} drawvert_t;

extern float gx_pending_texcoord[MAX_TEXTURE_UNITS][2];

void GX_SaveProjectionMtx( const Mtx44 src, u8 type );
void GX_ReloadProjectionMtx( float zbias );
void Matrix4x4_ToMtx44( Mtx44 dst, const matrix4x4 src );
void Matrix4x4_ToMtx( Mtx dst, const matrix4x4 src );

void GX_ReadPixelsRGBA( int x, int y, int w, int h, byte *out );
void GX_UpdateTexture( int texnum, int cols, int rows, int width, int height, const byte *buffer, pixformat_t fmt );

void GX_ApplyTextureParams( gl_texture_t *tex );
void GX_RebuildLightmaps( void );

void GX_BackendStartFrame( void );
void GX_BackendEndFrame( void );
void GX_CleanUpTextureUnits( int last );
void GX_Bind( int tmu, unsigned int texnum );
void GX_SetTexCoordGen2f( int tmu, u32 src, u32 mtx ); // corregido
void GX_SetTexCoordArrayMode( int tmu, qboolean enable );
void GX_LoadTexMatrix( int tmu, const float *matrix );
void GX_LoadMatrix( const matrix4x4 source );
void GX_SelectTexture( int tmu );
void GX_CleanupAllTextureUnits( void );
void GX_LoadIdentityTexMatrix( void );
void GX_DisableAllTexGens( void );
void GX_SetRenderMode( int mode );
void GX_EnableTextureUnit( int tmu, qboolean enable );
void GX_Cull( int cullMode );
void GX_DrawIndexedPrimitive( u8 primType, const void *verts, int numVerts, const u16 *indices, int numIndices );
void GX_PushPolygonOffset( float factor, float units );
void GX_PopPolygonOffset( void );
void SCR_TimeRefresh_f( void );

void CL_DrawBeams( int fTrans, BEAM *active_beams );

qboolean R_CullModel( const cl_entity_t *e, const vec3_t absmin, const vec3_t absmax );
qboolean R_CullBox( const vec3_t mins, const vec3_t maxs );
int R_CullSurface( const msurface_t *surf, const gl_frustum_t *frustum, uint clipflags );

void DrawSurfaceDecals( msurface_t *fa, qboolean single, qboolean reverse );
float *R_DecalSetupVerts( decal_t *pDecal, msurface_t *surf, int texture, int *outCount );
void DrawSingleDecal( decal_t *pDecal, msurface_t *fa );
void R_EntityRemoveDecals( model_t *mod );
void DrawDecalsBatch( void );
void R_ClearDecals( void );

void R_Set2DMode( qboolean enable );

void R_SetTextureParameters( void );
gl_texture_t *R_GetTexture( unsigned int texnum );
#define GX_LoadTextureInternal( name, pic, flags ) GX_LoadTextureFromBuffer( name, pic, flags, false )
#define GX_UpdateTextureInternal( name, pic, flags ) GX_LoadTextureFromBuffer( name, pic, flags, true )
int GX_LoadTexture( const char *name, const byte *buf, size_t size, int flags );
int GX_LoadTextureArray( const char **names, int flags );
int GX_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update );
int GX_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags );
int GX_CreateTextureArray( const char *name, int width, int height, int depth, const void *buffer, texFlags_t flags );
void GX_ProcessTexture( int texnum, float gamma, int topColor, int bottomColor );
void GX_UpdateTexSize( int texnum, int width, int height, int depth );
qboolean GX_TextureFilteringEnabled( const gl_texture_t *tex );
int GX_FindTexture( const char *name );
void GX_FreeTexture( unsigned int texnum );
void R_InitDlightTexture( void );
void R_TextureList_f( void );
void R_InitImages( void );
void R_ShutdownImages( void );
int GX_TexMemory( void );
qboolean R_SearchForTextureReplacement( char *out, size_t size, const char *modelname, const char *fmt, ... ) FORMAT_CHECK( 4 );
void R_TextureReplacementReport( const char *modelname, int gl_texturenum, const char *foundpath );
void R_ShowTextures( void );

void R_ClearScene( void );
void R_LoadIdentity( void );
void R_RenderScene( void );
void R_DrawCubemapView( const vec3_t origin, const vec3_t angles, int size );
void R_SetupRefParams( const struct ref_viewpass_s *rvp );
void R_TranslateForEntity( cl_entity_t *e );
void R_RotateForEntity( cl_entity_t *e );
void R_SetupGL( qboolean set_gl_state );
void R_AllowFog( qboolean allowed );
qboolean R_OpaqueEntity( cl_entity_t *ent );
void R_SetupFrustum( void );
void R_FindViewLeaf( void );
void R_PushScene( void );
void R_PopScene( void );
void R_DrawFog( void );
int CL_FxBlend( cl_entity_t *e );

void R_MarkLeaves( void );
void R_DrawWorld( void );
void R_DrawWaterSurfaces( void );
void R_DrawBrushModel( cl_entity_t *e );
void GX_SubdivideSurface( model_t *mod, msurface_t *fa );
void GX_SetupFogColorForSurfaces( void );
void R_DrawAlphaTextureChains( void );
void GX_RebuildLightmaps( void );
void GX_BuildLightmaps( void );
void GX_ResetFogColor( void );
void R_GenerateVBO( void );
void R_ClearVBO( void );
void R_AddDecalVBO( decal_t *pdecal, msurface_t *surf );
void R_LightmapCoord( const vec3_t v, const msurface_t *surf, const float sample_size, vec2_t coords );
qboolean R_HasGeneratedVBO( void );
void R_EnableVBO( qboolean enable );
qboolean R_HasEnabledVBO( void );

void CL_DrawParticlesExternal( const ref_viewpass_t *rvp, qboolean trans_pass, float frametime );
void CL_DrawParticles( double frametime, particle_t *cl_active_particles, float partsize );
void CL_DrawTracers( double frametime, particle_t *cl_active_tracers );

void R_DrawSpriteModel( cl_entity_t *e );

void R_StudioInit( void );
studiohdr_t *R_StudioGetHeader( void );
void R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles );
struct mstudiotex_s *R_StudioGetTexture( cl_entity_t *e );
int R_GetEntityRenderMode( cl_entity_t *ent );
void R_DrawStudioModel( cl_entity_t *e );
player_info_t *pfnPlayerInfo( int index );
float R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time );
void R_StudioResetPlayerModels( void );
qboolean R_StudioFillAPI( struct engine_studio_api_s *api, struct r_studio_interface_s *pDefaultDraw );
void R_StudioSetDrawInterface( struct r_studio_interface_s *pDraw );
void Mod_StudioLoadTextures( model_t *mod, void *data );
void Mod_StudioUnloadTextures( void *data );

void Mod_LoadAliasModel( model_t *mod, const void *buffer, qboolean *loaded );
void R_DrawAliasModel( cl_entity_t *e );
void R_AliasInit( void );

void R_AddSkyBoxSurface( msurface_t *fa );
void R_ClearSkyBox( void );
void R_DrawSkyBox( void );
void R_DrawClouds( void );
void R_UnloadSkybox( void );
void R_ResetRipples( void );
void R_AnimateRipples( void );
qboolean R_UploadRipples( texture_t *image );

qboolean R_Init( void );
void R_Shutdown( void );
void GX_SetupAttributes( int safegl );
void GX_OnContextCreated( void );
void GX_InitExtensions( void );
void GX_ClearExtensions( void );
int GX_LoadTexture( const char *name, const byte *buf, size_t size, int flags );
qboolean VID_ScreenShot( const char *filename, int shot_type );
qboolean VID_CubemapShot( const char *base, uint size, const float *vieworg, qboolean skyshot );
void R_GammaChanged( qboolean do_reset_gamma );
void R_BeginFrame( qboolean clearScene );
void R_RenderFrame( const struct ref_viewpass_s *vp );
void R_EndFrame( void );
void R_ClearScene( void );
void R_GetTextureParms( int *w, int *h, int texnum );
void R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum );
qboolean R_SpeedsMessage( char *out, size_t size );
qboolean R_CullBox( const vec3_t mins, const vec3_t maxs );
int R_WorldToScreen( const vec3_t point, vec3_t screen );
void R_ScreenToWorld( const vec3_t screen, vec3_t point );
qboolean R_AddEntity( struct cl_entity_s *pRefEntity, int entityType );
void Mod_UnloadAliasModel( struct model_s *mod );
void Mod_AliasUnloadTextures( void *data );
void GX_SetRenderMode( int mode );
void R_RunViewmodelEvents( void );
void R_DrawViewModel( void );
void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale );
void R_DecalRemoveAll( int texture );
int R_CreateDecalList( decallist_t *pList );
void R_ClearAllDecals( void );
byte *Mod_GetCurrentVis( void );
void Mod_SetOrthoBounds( const float *mins, const float *maxs );

void GX_Present( void );
#define GX_CheckForErrors() ((void)0)

void TriRenderMode( int mode );
void TriBegin( int mode );
void TriEnd( void );
void TriTexCoord2f( float u, float v );
void TriVertex3fv( const float *v );
void TriVertex3f( float x, float y, float z );
void _TriColor4f( float r, float g, float b, float a );
void _TriColor4ub( byte r, byte g, byte b, byte a );
void TriColor4f( float r, float g, float b, float a );
void TriColor4ub( byte r, byte g, byte b, byte a );
void TriBrightness( float brightness );
int TriWorldToScreen( const float *world, float *screen );
int TriSpriteTexture( model_t *pSpriteModel, int frame );
void TriFog( float flFogColor[3], float flStart, float flEnd, int bOn );
void TriGetMatrix( const int pname, float *matrix );
void TriFogParams( float flDensity, int iFogSkybox );
void TriCullFace( TRICULLSTYLE mode );

enum
{
	GX_HW_BASE = 0,
	GX_HW_WIDESCREEN,
	GX_HW_PROGRESSIVE,
	GX_HW_EFB_TO_TEXTURE,
	GX_HW_Z_COMPRESSION,
	GX_HW_MULTISAMPLE,
	GX_EXTCOUNT,
};

typedef enum
{
	GXHW_GENERIC,
	GXHW_DEVKIT
} glHWType_t;

typedef struct
{
	const char	*renderer_string;
	const char	*vendor_string;
	const char	*version_string;

	glHWType_t	hardware_type;

	const char	*extensions_string;
	byte		extension[GX_EXTCOUNT];

	int		max_texture_units;
	int		max_texture_coords;
	int		max_teximage_units;
	int		max_2d_texture_size;
	int		max_2d_rectangle_size;
	int		max_2d_texture_layers;
	int		max_3d_texture_size;
	int		max_cubemap_size;

	float		max_texture_anisotropy;
	float		max_texture_lod_bias;

	int		max_vertex_uniforms;
	int		max_vertex_attribs;

	int		max_multisamples;

	int		color_bits;
	int		alpha_bits;
	int		depth_bits;
	int		stencil_bits;
	int		msaasamples;
	int		version_major;
	int		version_minor;

	qboolean		softwareGammaUpdate;
	qboolean		fCustomRenderer;
	int		prev_width;
	int		prev_height;
} glconfig_t;

typedef struct polyoffset_state_s
{
	float factor;
	float units;
} polyoffset_state_t;

typedef struct
{
	int		activeTMU;
	GXTexObj		*currentTextures[MAX_TEXTURE_UNITS];
	int		currentTexturesIndex[MAX_TEXTURE_UNITS];
	u8		currentTextureFormats[MAX_TEXTURE_UNITS];
	qboolean	texIdentityMatrix[MAX_TEXTURE_UNITS];
	int		genSTEnabled[MAX_TEXTURE_UNITS];
	int		texCoordArrayMode[MAX_TEXTURE_UNITS];
	int		isFogEnabled;

	int		faceCull;

	qboolean		stencilEnabled;
	qboolean		in2DMode;

	polyoffset_state_t polyoffset_state[2];
	int num_polyoffsets;
} glstate_t;

typedef struct
{
	qboolean		initialized;
	qboolean		extended;
} glwstate_t;

extern glconfig_t		glConfig;
extern glstate_t		glState;
extern glwstate_t		glw_state;

static inline cl_entity_t *CL_GetEntityByIndex( int index )
{
	if( unlikely( index < 0 || index >= tr.max_entities || !tr.entities ))
		return NULL;

	return &tr.entities[index];
}

static inline model_t *CL_ModelHandle( int index )
{
	if( unlikely( index < 0 || index >= gp_cl->nummodels ))
		return NULL;

	return gp_cl->models[index];
}

static inline byte TextureToGamma( byte b )
{
	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? tr.texgammatable[b] : b;
}

static inline uint LightToTexGamma( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;

	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? tr.lightgammatable[b] : b;
}

static inline uint ScreenGammaTable( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;

	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? tr.screengammatable[b] : b;
}

static inline uint LinearGammaTable( uint b )
{
	if( unlikely( b >= 1024 ))
		return 0;

	return !FBitSet( gp_host->features, ENGINE_LINEAR_GAMMA_SPACE ) ? tr.lineargammatable[b] : b;
}

static inline qboolean GL_Support( int r_ext )
{
	if( r_ext >= 0 && r_ext < GX_EXTCOUNT )
		return glConfig.extension[r_ext] ? true : false;
	gEngfuncs.Con_Printf( S_ERROR "%s: invalid extension %d\n", __func__, r_ext );

	return false;
}

static inline int GL_MaxTextureUnits( void )
{
	return Q_min( glConfig.max_texture_units, MAX_TEXTURE_UNITS );
}

#define WORLDMODEL (tr.worldmodel)

extern convar_t	gl_texture_anisotropy;
extern convar_t	gl_extensions;
extern convar_t	gl_check_errors;
extern convar_t	gl_texture_lodbias;
extern convar_t	gl_texture_nearest;
extern convar_t	gl_lightmap_nearest;
extern convar_t	gl_keeptjunctions;
extern convar_t	gl_round_down;
extern convar_t	gl_wireframe;
extern convar_t	gl_polyoffset;
extern convar_t	gl_polyoffset_bmodels;
extern convar_t	gl_finish;
extern convar_t	gl_nosort;
extern convar_t	gl_test;
extern convar_t	gl_msaa;
extern convar_t	gl_stencilbits;
extern convar_t	gl_overbright;
extern convar_t gl_fog;
extern convar_t	gl_litwater_force;

extern convar_t	ref_gl_clear;

extern convar_t	r_lighting_ambient;
extern convar_t	r_studio_lambert;
extern convar_t	r_detailtextures;
extern convar_t	r_novis;
extern convar_t	r_nocull;
extern convar_t	r_lockpvs;
extern convar_t	r_lockfrustum;
extern convar_t	r_traceglow;
extern convar_t	r_vbo;
extern convar_t	r_vbo_dlightmode;
extern convar_t	r_vbo_detail;
extern convar_t	r_vbo_overbrightmode;
extern convar_t r_studio_sort_textures;
extern convar_t r_studio_drawelements;
extern convar_t r_studio_builtin_renderer;
extern convar_t r_shadows;
extern convar_t r_ripple;
extern convar_t r_ripple_updatetime;
extern convar_t r_ripple_spawntime;
extern convar_t r_large_lightmaps;
extern convar_t r_drawentities;
extern convar_t r_norefresh;

#include "crtlib.h"

#endif // GX_LOCAL_H