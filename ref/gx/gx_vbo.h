#ifndef GX_VBO_H
#define GX_VBO_H

#define GX_VBO_VERT_SIZE 28
#define GX_VBO_HARD_MAX_VERTS 65535 // indices are 16 bit

extern int gx_testmode;

typedef struct {
u8 *vertex_data;
u16 *index_data;
int num_verts_used;
int max_verts;
int num_indices;
int max_idx;
int overflow;
int initialized;
} gx_vbo_t;

static int gx_world_dbg;
static gx_vbo_t g_vbo = { NULL, NULL, 0, 0, 0, 0, 0, 0 };

static void GX_VBO_Free(void)
{
if (g_vbo.vertex_data) free(g_vbo.vertex_data);
if (g_vbo.index_data) free(g_vbo.index_data);
memset(&g_vbo, 0, sizeof(g_vbo));
}

static void GX_VBO_Alloc(int max_verts)
{
GX_VBO_Free();
if (max_verts < 3) return;
g_vbo.max_verts = max_verts;
g_vbo.max_idx = max_verts * 3;
g_vbo.vertex_data = (u8 *)memalign(32, (size_t)max_verts * GX_VBO_VERT_SIZE);
g_vbo.index_data = (u16 *)memalign(32, (size_t)g_vbo.max_idx * sizeof(u16));
if (!g_vbo.vertex_data || !g_vbo.index_data)
{
GX_VBO_Free();
gEngfuncs.Con_Printf("VBO: out of memory for %d verts, using direct drawing\n", max_verts);
return;
}
g_vbo.initialized = 1;
}

static void GX_SetupVtxFormat_VBO(void)
{
GX_SetChanVtxColor(false);
GX_ClearVtxDesc();
GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX16);
GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,
GX_POS_XYZ, GX_F32, 0);
GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0,
GX_TEX_ST, GX_F32, 0);
GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1,
GX_TEX_ST, GX_F32, 0);
}

static void GX_VBO_BuildSurface(msurface_t *surf)
{
glpoly2_t *p;
int surf_start = g_vbo.num_verts_used;
int need = 0;

surf->info->reserved[0] = -1;
surf->info->reserved[1] = 0;

for (p = surf->polys; p; p = p->chain)
if (p->numverts >= 3)
need += p->numverts;

if (need == 0 || g_vbo.num_verts_used + need > g_vbo.max_verts)
{
if (need) g_vbo.overflow++;
return;
}

surf->info->reserved[0] = surf_start;

for (p = surf->polys; p; p = p->chain)
{
int i;
if (p->numverts < 3)
continue;
for (i = 0; i < p->numverts; i++)
{
float *src = p->verts[i];
float *dst = (float *)(g_vbo.vertex_data + g_vbo.num_verts_used * GX_VBO_VERT_SIZE);
dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
dst[3] = src[3]; dst[4] = src[4];
dst[5] = src[5]; dst[6] = src[6];
g_vbo.num_verts_used++;
}
surf->info->reserved[1] += (p->numverts - 2) * 3;
}

DCFlushRange(g_vbo.vertex_data + surf_start * GX_VBO_VERT_SIZE,
(g_vbo.num_verts_used - surf_start) * GX_VBO_VERT_SIZE);
}

static qboolean GX_VBO_ModelWanted(model_t *m)
{
return m != NULL && m->name[0] != '*' && m->type == mod_brush;
}



static void GX_VBO_BuildWorld(void)
{
int i, j, total = 0;

GX_VBO_Free();
gx_world_dbg = 3;

for (i = 0; i < gp_cl->nummodels; i++)
{
model_t *m = CL_ModelHandle(i + 1);
if (!GX_VBO_ModelWanted(m))
continue;
for (j = 0; j < m->numsurfaces; j++)
{
glpoly2_t *p;
for (p = m->surfaces[j].polys; p; p = p->chain)
if (p->numverts >= 3)
total += p->numverts;
}
}

GX_VBO_Alloc(total < GX_VBO_HARD_MAX_VERTS ? total : GX_VBO_HARD_MAX_VERTS);

for (i = 0; i < gp_cl->nummodels; i++)
{
model_t *m = CL_ModelHandle(i + 1);
if (!GX_VBO_ModelWanted(m))
continue;
for (j = 0; j < m->numsurfaces; j++)
{
if (g_vbo.initialized)
GX_VBO_BuildSurface(m->surfaces + j);
else
{
m->surfaces[j].info->reserved[0] = -1;
m->surfaces[j].info->reserved[1] = 0;
}
}
}

GX_LOG("VBO: %d of %d verts (%.2f MB), %d surfaces drawn directly\n",
g_vbo.num_verts_used, total,
(g_vbo.num_verts_used * GX_VBO_VERT_SIZE) / (1024.0f * 1024.0f),
g_vbo.overflow);
DCFlushRange(g_vbo.vertex_data, g_vbo.num_verts_used * GX_VBO_VERT_SIZE);
GX_InvVtxCache();
}



static void GX_SetupVtxFormat_Direct(void)
{
GX_SetChanVtxColor(false);
GX_ClearVtxDesc();
GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
}

static void GX_VBO_DrawDirect(msurface_t *head, qboolean only_unbuilt)
{
msurface_t *s;
glpoly2_t *p;
int tris = 0;

for (s = head; s != NULL; s = s->texturechain)
{
if (only_unbuilt && s->info->reserved[0] >= 0)
continue;
for (p = s->polys; p; p = p->chain)
if (p->numverts >= 3)
tris += p->numverts - 2;
}

if (tris == 0)
return;

GX_SetupVtxFormat_Direct();
GX_Begin(GX_TRIANGLES, GX_VTXFMT0, tris * 3);

for (s = head; s != NULL; s = s->texturechain)
{
if (only_unbuilt && s->info->reserved[0] >= 0)
continue;
for (p = s->polys; p; p = p->chain)
{
float *base = p->verts[0];
int i;
for (i = 1; i < p->numverts - 1; i++)
{
float *v[3] = { base, base + i * VERTEXSIZE, base + (i + 1) * VERTEXSIZE };
int k;
for (k = 0; k < 3; k++)
{
GX_Position3f32(v[k][0], v[k][1], v[k][2]);
GX_TexCoord2f32(v[k][3], v[k][4]);
GX_TexCoord2f32(v[k][5], v[k][6]);
}
}
}
}

GX_End();
}

static void GX_VBO_DrawChain(msurface_t *head)
{
msurface_t *s;
qboolean has_direct = false;

if (!g_vbo.initialized || gx_testmode == 4)
{
GX_VBO_DrawDirect(head, false);
return;
}

g_vbo.num_indices = 0;

for (s = head; s != NULL; s = s->texturechain)
{
glpoly2_t *p;
int local = (int)s->info->reserved[0];

if (local < 0)
{
has_direct = true;
continue;
}

for (p = s->polys; p; p = p->chain)
{
int i;
if (p->numverts < 3)
continue;
for (i = 1; i < p->numverts - 1; i++)
{
if (g_vbo.num_indices + 3 > g_vbo.max_idx)
break;
g_vbo.index_data[g_vbo.num_indices++] = (u16)local;
g_vbo.index_data[g_vbo.num_indices++] = (u16)(local + i);
g_vbo.index_data[g_vbo.num_indices++] = (u16)(local + i + 1);
}
local += p->numverts;
}
}

if (g_vbo.num_indices > 0)
{
int i;

DCFlushRange(g_vbo.index_data, g_vbo.num_indices * sizeof(u16));

GX_SetupVtxFormat_VBO();
GX_SetArray(GX_VA_POS, g_vbo.vertex_data, GX_VBO_VERT_SIZE);
GX_SetArray(GX_VA_TEX0, g_vbo.vertex_data + 12, GX_VBO_VERT_SIZE);
GX_SetArray(GX_VA_TEX1, g_vbo.vertex_data + 20, GX_VBO_VERT_SIZE);

for (i = 0; i < g_vbo.num_indices; )
{
int n = g_vbo.num_indices - i;
int j;
if (n > 60000) n = 60000;
GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)n);
for (j = 0; j < n; j++, i++)
{
u16 idx = g_vbo.index_data[i];
GX_Position1x16(idx);
GX_TexCoord1x16(idx);
GX_TexCoord1x16(idx);
}
GX_End();
}
}

if (has_direct)
GX_VBO_DrawDirect(head, true);
}



static void DrawGLPolyBatchChain_VBO(msurface_t *head)
{
if (!head)
return;
GX_VBO_DrawChain(head);
}

#endif
