/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2019, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "DNA_armature_types.h"
#include "DNA_constraint_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"
#include "DNA_object_types.h"

#include "DRW_render.h"

#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BKE_armature.h"

#include "DEG_depsgraph_query.h"

#include "ED_armature.h"
#include "ED_view3d.h"

#include "UI_resources.h"

#include "draw_common.h"
#include "draw_manager_text.h"

#include "overlay_private.h"

#define BONE_VAR(eBone, pchan, var) ((eBone) ? (eBone->var) : (pchan->var))
#define BONE_FLAG(eBone, pchan) ((eBone) ? (eBone->flag) : (pchan->bone->flag))

#define PT_DEFAULT_RAD 0.05f /* radius of the point batch. */

typedef struct ArmatureDrawContext {
  /* Current armature object */
  Object *ob;
  /* bArmature *arm; */ /* TODO */

  union {
    struct {
      DRWCallBuffer *outline;
      DRWCallBuffer *solid;
      DRWCallBuffer *wire;
    };
    struct {
      DRWCallBuffer *envelope_outline;
      DRWCallBuffer *envelope_solid;
      DRWCallBuffer *envelope_distance;
    };
    struct {
      DRWCallBuffer *stick;
    };
  };

  DRWCallBuffer *dof_lines;
  DRWCallBuffer *dof_sphere;
  DRWCallBuffer *point_solid;
  DRWCallBuffer *point_outline;
  DRWShadingGroup *custom_solid;
  DRWShadingGroup *custom_outline;
  DRWShadingGroup *custom_wire;

  OVERLAY_ExtraCallBuffers *extras;

  /**
   * Follow `TH_*` naming except for mixed colors.
   */
  struct {
    float select[4];
    float edge_select[4];
    float bone_select[4]; /* tint */
    float wire[4];
    float wire_edit[4];
    float bone_solid[4];
    float bone_active_unselect[4]; /* mix */
    float bone_pose[4];
    float bone_pose_active[4];
    float bone_pose_active_unselect[4]; /* mix */
    float text_hi[4];
    float text[4];
    float vertex_select[4];
    float vertex[4];
  } color;

  /* not a theme, this is an override */
  const float *const_color;
  float const_wire;

  bool do_relations;
  bool transparent;
  bool show_relations;

  const ThemeWireColor *bcolor; /* pchan color */
} ArmatureDrawContext;

void OVERLAY_armature_cache_init(OVERLAY_Data *vedata)
{
  OVERLAY_PassList *psl = vedata->psl;
  OVERLAY_PrivateData *pd = vedata->stl->pd;

  const DRWContextState *draw_ctx = DRW_context_state_get();
  pd->armature.transparent = (draw_ctx->v3d->shading.type == OB_WIRE) ||
                             XRAY_FLAG_ENABLED(draw_ctx->v3d);
  pd->armature.show_relations = ((draw_ctx->v3d->flag & V3D_HIDE_HELPLINES) == 0);

  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ADD;
  DRW_PASS_CREATE(psl->armature_transp_ps, state | pd->clipping_state);

  for (int i = 0; i < 2; i++) {
    struct GPUShader *sh;
    struct GPUVertFormat *format;
    DRWShadingGroup *grp = NULL, *grp_sub = NULL;

    OVERLAY_InstanceFormats *formats = OVERLAY_shader_instance_formats_get();
    OVERLAY_ArmatureCallBuffers *cb = &pd->armature_call_buffers[i];
    DRWPass **p_armature_ps = &psl->armature_ps[i];

    state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_CULL_BACK |
            (pd->armature.transparent ? DRW_STATE_BLEND_ALPHA : DRW_STATE_WRITE_DEPTH);
    DRW_PASS_CREATE(*p_armature_ps, state | pd->clipping_state);

    DRWPass *armature_ps = *p_armature_ps;

#define BUF_INSTANCE DRW_shgroup_call_buffer_instance
#define BUF_LINE(grp, format) DRW_shgroup_call_buffer(grp, format, GPU_PRIM_LINES)

    {
      format = formats->instance_bone;

      sh = OVERLAY_shader_armature_sphere(false);
      grp = DRW_shgroup_create(sh, armature_ps);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      cb->point_solid = BUF_INSTANCE(grp, format, DRW_cache_bone_point_get());

      sh = OVERLAY_shader_armature_sphere(true);
      grp = DRW_shgroup_create(sh, armature_ps);
      DRW_shgroup_state_disable(grp, DRW_STATE_CULL_BACK);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      cb->point_outline = BUF_INSTANCE(grp, format, DRW_cache_bone_point_wire_outline_get());

      sh = OVERLAY_shader_armature_shape(false);
      grp = DRW_shgroup_create(sh, armature_ps);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      DRW_shgroup_uniform_bool_copy(grp, "customShape", false);
      cb->box_solid = BUF_INSTANCE(grp, format, DRW_cache_bone_box_get());
      cb->octa_solid = BUF_INSTANCE(grp, format, DRW_cache_bone_octahedral_get());

      cb->custom_solid = grp_sub = DRW_shgroup_create_sub(grp);
      DRW_shgroup_uniform_bool_copy(grp_sub, "customShape", true);

      sh = OVERLAY_shader_armature_shape(true);
      grp = DRW_shgroup_create(sh, armature_ps);
      DRW_shgroup_state_disable(grp, DRW_STATE_CULL_BACK);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      DRW_shgroup_uniform_bool_copy(grp, "customShape", false);
      cb->box_outline = BUF_INSTANCE(grp, format, DRW_cache_bone_box_wire_get());
      cb->octa_outline = BUF_INSTANCE(grp, format, DRW_cache_bone_octahedral_wire_get());

      cb->custom_outline = grp_sub = DRW_shgroup_create_sub(grp);
      DRW_shgroup_state_disable(grp_sub, DRW_STATE_CULL_BACK);
      DRW_shgroup_uniform_bool_copy(grp_sub, "customShape", true);
    }
    {
      format = formats->instance_extra;

      sh = OVERLAY_shader_armature_degrees_of_freedom();
      grp = DRW_shgroup_create(sh, armature_ps);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      cb->dof_lines = BUF_INSTANCE(grp, format, DRW_cache_bone_dof_lines_get());

      grp = DRW_shgroup_create(sh, psl->armature_transp_ps);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      cb->dof_sphere = BUF_INSTANCE(grp, format, DRW_cache_bone_dof_sphere_get());
    }
    {
      format = formats->instance_bone_stick;

      sh = OVERLAY_shader_armature_stick();
      grp = DRW_shgroup_create(sh, armature_ps);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      cb->stick = BUF_INSTANCE(grp, format, DRW_cache_bone_stick_get());
    }
    {
      format = formats->instance_bone_envelope;

      sh = OVERLAY_shader_armature_envelope(false);
      grp = DRW_shgroup_create(sh, armature_ps);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      DRW_shgroup_uniform_bool_copy(grp, "isDistance", false);
      cb->envelope_solid = BUF_INSTANCE(grp, format, DRW_cache_bone_envelope_solid_get());

      format = formats->instance_bone_envelope_outline;

      sh = OVERLAY_shader_armature_envelope(true);
      grp = DRW_shgroup_create(sh, armature_ps);
      DRW_shgroup_state_disable(grp, DRW_STATE_CULL_BACK);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      cb->envelope_outline = BUF_INSTANCE(grp, format, DRW_cache_bone_envelope_outline_get());

      format = formats->instance_bone_envelope_distance;

      sh = OVERLAY_shader_armature_envelope(false);
      grp = DRW_shgroup_create(sh, psl->armature_transp_ps);
      DRW_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
      DRW_shgroup_uniform_bool_copy(grp, "isDistance", true);
      DRW_shgroup_state_enable(grp, DRW_STATE_CULL_FRONT);
      cb->envelope_distance = BUF_INSTANCE(grp, format, DRW_cache_bone_envelope_solid_get());
    }
    {
      format = formats->pos_color;

      sh = OVERLAY_shader_armature_wire();
      grp = DRW_shgroup_create(sh, armature_ps);
      cb->wire = BUF_LINE(grp, format);
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Shader Groups (DRW_shgroup)
 * \{ */

typedef struct BoneInstanceData {
  /* Keep sync with bone instance vertex format (OVERLAY_InstanceFormats) */
  union {
    float mat[4][4];
    struct {
      float _pad0[3], color_hint_a;
      float _pad1[3], color_hint_b;
      float _pad2[3], color_a;
      float _pad3[3], color_b;
    };
    struct {
      float _pad00[3], amin_a;
      float _pad01[3], amin_b;
      float _pad02[3], amax_a;
      float _pad03[3], amax_b;
    };
  };
} BoneInstanceData;

static void bone_instance_data_set_angle_minmax(BoneInstanceData *data,
                                                const float aminx,
                                                const float aminz,
                                                const float amaxx,
                                                const float amaxz)
{
  data->amin_a = aminx;
  data->amin_b = aminz;
  data->amax_a = amaxx;
  data->amax_b = amaxz;
}

static void bone_instance_data_set_color_hint(BoneInstanceData *data, const float hint_color[4])
{
  /* Encoded color into 2 floats to be able to use the obmat to color the custom bones. */
  data->color_hint_a = (hint_color[0] * 254.0f / 255.0f) + floorf(hint_color[1] * 255.0f);
  data->color_hint_b = (hint_color[2] * 254.0f / 255.0f) + floorf(hint_color[3] * 255.0f);
}

static void bone_instance_data_set_color(BoneInstanceData *data, const float bone_color[4])
{
  /* Encoded color into 2 floats to be able to use the obmat to color the custom bones. */
  data->color_a = (bone_color[0] * 254.0f / 255.0f) + floorf(bone_color[1] * 255.0f);
  data->color_b = (bone_color[2] * 254.0f / 255.0f) + floorf(bone_color[3] * 255.0f);
}

/* Octahedral */
static void drw_shgroup_bone_octahedral(ArmatureDrawContext *ctx,
                                        const float (*bone_mat)[4],
                                        const float bone_color[4],
                                        const float hint_color[4],
                                        const float outline_color[4])
{
  BoneInstanceData inst_data;
  mul_m4_m4m4(inst_data.mat, ctx->ob->obmat, bone_mat);
  if (ctx->solid) {
    bone_instance_data_set_color(&inst_data, bone_color);
    bone_instance_data_set_color_hint(&inst_data, hint_color);
    DRW_buffer_add_entry_struct(ctx->solid, &inst_data);
  }
  if (outline_color[3] > 0.0f) {
    bone_instance_data_set_color(&inst_data, outline_color);
    DRW_buffer_add_entry_struct(ctx->outline, &inst_data);
  }
}

/* Box / B-Bone */
static void drw_shgroup_bone_box(ArmatureDrawContext *ctx,
                                 const float (*bone_mat)[4],
                                 const float bone_color[4],
                                 const float hint_color[4],
                                 const float outline_color[4])
{
  BoneInstanceData inst_data;
  mul_m4_m4m4(inst_data.mat, ctx->ob->obmat, bone_mat);
  if (ctx->solid) {
    bone_instance_data_set_color(&inst_data, bone_color);
    bone_instance_data_set_color_hint(&inst_data, hint_color);
    DRW_buffer_add_entry_struct(ctx->solid, &inst_data);
  }
  if (outline_color[3] > 0.0f) {
    bone_instance_data_set_color(&inst_data, outline_color);
    DRW_buffer_add_entry_struct(ctx->outline, &inst_data);
  }
}

/* Wire */
static void drw_shgroup_bone_wire(ArmatureDrawContext *ctx,
                                  const float (*bone_mat)[4],
                                  const float color[4])
{
  float head[3], tail[3];
  mul_v3_m4v3(head, ctx->ob->obmat, bone_mat[3]);
  add_v3_v3v3(tail, bone_mat[3], bone_mat[1]);
  mul_m4_v3(ctx->ob->obmat, tail);

  DRW_buffer_add_entry(ctx->wire, head, color);
  DRW_buffer_add_entry(ctx->wire, tail, color);
}

/* Stick */
static void drw_shgroup_bone_stick(ArmatureDrawContext *ctx,
                                   const float (*bone_mat)[4],
                                   const float col_wire[4],
                                   const float col_bone[4],
                                   const float col_head[4],
                                   const float col_tail[4])
{
  float head[3], tail[3];
  mul_v3_m4v3(head, ctx->ob->obmat, bone_mat[3]);
  add_v3_v3v3(tail, bone_mat[3], bone_mat[1]);
  mul_m4_v3(ctx->ob->obmat, tail);

  DRW_buffer_add_entry(ctx->stick, head, tail, col_wire, col_bone, col_head, col_tail);
}

/* Envelope */
static void drw_shgroup_bone_envelope_distance(ArmatureDrawContext *ctx,
                                               const float (*bone_mat)[4],
                                               const float *radius_head,
                                               const float *radius_tail,
                                               const float *distance)
{
  if (ctx->envelope_distance) {
    float head_sph[4] = {0.0f, 0.0f, 0.0f, 1.0f}, tail_sph[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    float xaxis[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    /* Still less operation than m4 multiplication. */
    mul_m4_v4(bone_mat, head_sph);
    mul_m4_v4(bone_mat, tail_sph);
    mul_m4_v4(bone_mat, xaxis);
    mul_m4_v4(ctx->ob->obmat, head_sph);
    mul_m4_v4(ctx->ob->obmat, tail_sph);
    mul_m4_v4(ctx->ob->obmat, xaxis);
    sub_v3_v3(xaxis, head_sph);
    head_sph[3] = *radius_head;
    head_sph[3] += *distance;
    tail_sph[3] = *radius_tail;
    tail_sph[3] += *distance;
    DRW_buffer_add_entry(ctx->envelope_distance, head_sph, tail_sph, xaxis);
  }
}

static void drw_shgroup_bone_envelope(ArmatureDrawContext *ctx,
                                      const float (*bone_mat)[4],
                                      const float bone_col[4],
                                      const float hint_col[4],
                                      const float outline_col[4],
                                      const float *radius_head,
                                      const float *radius_tail)
{
  float head_sph[4] = {0.0f, 0.0f, 0.0f, 1.0f}, tail_sph[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  float xaxis[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  /* Still less operation than m4 multiplication. */
  mul_m4_v4(bone_mat, head_sph);
  mul_m4_v4(bone_mat, tail_sph);
  mul_m4_v4(bone_mat, xaxis);
  mul_m4_v4(ctx->ob->obmat, head_sph);
  mul_m4_v4(ctx->ob->obmat, tail_sph);
  mul_m4_v4(ctx->ob->obmat, xaxis);
  head_sph[3] = *radius_head;
  tail_sph[3] = *radius_tail;

  if (head_sph[3] < 0.0f || tail_sph[3] < 0.0f) {
    BoneInstanceData inst_data;
    if (head_sph[3] < 0.0f) {
      /* Draw Tail only */
      scale_m4_fl(inst_data.mat, tail_sph[3] / PT_DEFAULT_RAD);
      copy_v3_v3(inst_data.mat[3], tail_sph);
    }
    else {
      /* Draw Head only */
      scale_m4_fl(inst_data.mat, head_sph[3] / PT_DEFAULT_RAD);
      copy_v3_v3(inst_data.mat[3], head_sph);
    }

    if (ctx->point_solid) {
      bone_instance_data_set_color(&inst_data, bone_col);
      bone_instance_data_set_color_hint(&inst_data, hint_col);
      DRW_buffer_add_entry_struct(ctx->point_solid, &inst_data);
    }
    if (outline_col[3] > 0.0f) {
      bone_instance_data_set_color(&inst_data, outline_col);
      DRW_buffer_add_entry_struct(ctx->point_outline, &inst_data);
    }
  }
  else {
    /* Draw Body */
    float tmp_sph[4];
    float len = len_v3v3(tail_sph, head_sph);
    float fac_head = (len - head_sph[3]) / len;
    float fac_tail = (len - tail_sph[3]) / len;
    /* Small epsilon to avoid problem with float precision in shader. */
    if (len > (tail_sph[3] + head_sph[3]) + 1e-8f) {
      copy_v4_v4(tmp_sph, head_sph);
      interp_v4_v4v4(head_sph, tail_sph, head_sph, fac_head);
      interp_v4_v4v4(tail_sph, tmp_sph, tail_sph, fac_tail);
      if (ctx->envelope_solid) {
        DRW_buffer_add_entry(ctx->envelope_solid, head_sph, tail_sph, bone_col, hint_col, xaxis);
      }
      if (outline_col[3] > 0.0f) {
        DRW_buffer_add_entry(ctx->envelope_outline, head_sph, tail_sph, outline_col, xaxis);
      }
    }
    else {
      /* Distance between endpoints is too small for a capsule. Draw a Sphere instead. */
      float fac = max_ff(fac_head, 1.0f - fac_tail);
      interp_v4_v4v4(tmp_sph, tail_sph, head_sph, clamp_f(fac, 0.0f, 1.0f));

      BoneInstanceData inst_data;
      scale_m4_fl(inst_data.mat, tmp_sph[3] / PT_DEFAULT_RAD);
      copy_v3_v3(inst_data.mat[3], tmp_sph);
      if (ctx->point_solid) {
        bone_instance_data_set_color(&inst_data, bone_col);
        bone_instance_data_set_color_hint(&inst_data, hint_col);
        DRW_buffer_add_entry_struct(ctx->point_solid, &inst_data);
      }
      if (outline_col[3] > 0.0f) {
        bone_instance_data_set_color(&inst_data, outline_col);
        DRW_buffer_add_entry_struct(ctx->point_outline, &inst_data);
      }
    }
  }
}

/* Custom (geometry) */

extern void drw_batch_cache_validate(Object *custom);
extern void drw_batch_cache_generate_requested(Object *custom);

static void drw_shgroup_bone_custom_solid(ArmatureDrawContext *ctx,
                                          const float (*bone_mat)[4],
                                          const float bone_color[4],
                                          const float hint_color[4],
                                          const float outline_color[4],
                                          Object *custom)
{
  /* TODO(fclem) arg... less than ideal but we never iter on this object
   * to assure batch cache is valid. */
  drw_batch_cache_validate(custom);

  struct GPUBatch *surf = DRW_cache_object_surface_get(custom);
  struct GPUBatch *edges = DRW_cache_object_edge_detection_get(custom, NULL);
  struct GPUBatch *ledges = DRW_cache_object_loose_edges_get(custom);
  BoneInstanceData inst_data;

  if (surf || edges || ledges) {
    mul_m4_m4m4(inst_data.mat, ctx->ob->obmat, bone_mat);
  }

  if (surf && ctx->custom_solid) {
    bone_instance_data_set_color_hint(&inst_data, hint_color);
    bone_instance_data_set_color(&inst_data, bone_color);
    DRW_shgroup_call_obmat(ctx->custom_solid, surf, inst_data.mat);
  }

  if (edges && ctx->custom_outline) {
    bone_instance_data_set_color(&inst_data, outline_color);
    DRW_shgroup_call_obmat(ctx->custom_outline, edges, inst_data.mat);
  }

  if (ledges) {
    bone_instance_data_set_color_hint(&inst_data, outline_color);
    bone_instance_data_set_color(&inst_data, outline_color);
    DRW_shgroup_call_obmat(ctx->custom_wire, ledges, inst_data.mat);
  }

  /* TODO(fclem) needs to be moved elsewhere. */
  drw_batch_cache_generate_requested(custom);
}

static void drw_shgroup_bone_custom_wire(ArmatureDrawContext *ctx,
                                         const float (*bone_mat)[4],
                                         const float color[4],
                                         Object *custom)
{
  /* TODO(fclem) arg... less than ideal but we never iter on this object
   * to assure batch cache is valid. */
  drw_batch_cache_validate(custom);

  struct GPUBatch *geom = DRW_cache_object_all_edges_get(custom);

  if (geom) {
    BoneInstanceData inst_data;
    mul_m4_m4m4(inst_data.mat, ctx->ob->obmat, bone_mat);
    bone_instance_data_set_color_hint(&inst_data, color);
    bone_instance_data_set_color(&inst_data, color);
    DRW_shgroup_call_obmat(ctx->custom_wire, geom, inst_data.mat);
  }

  /* TODO(fclem) needs to be moved elsewhere. */
  drw_batch_cache_generate_requested(custom);
}

static void drw_shgroup_bone_custom_empty(ArmatureDrawContext *ctx,
                                          const float (*bone_mat)[4],
                                          const float color[4],
                                          Object *custom)
{
  float final_color[4] = {color[0], color[1], color[2], 1.0f};
  float mat[4][4];
  mul_m4_m4m4(mat, ctx->ob->obmat, bone_mat);

  switch (custom->empty_drawtype) {
    case OB_PLAINAXES:
    case OB_SINGLE_ARROW:
    case OB_CUBE:
    case OB_CIRCLE:
    case OB_EMPTY_SPHERE:
    case OB_EMPTY_CONE:
    case OB_ARROWS:
      OVERLAY_empty_shape(
          ctx->extras, mat, custom->empty_drawsize, custom->empty_drawtype, final_color);
      break;
    case OB_EMPTY_IMAGE:
      break;
  }
}

/* Head and tail sphere */
static void drw_shgroup_bone_point(ArmatureDrawContext *ctx,
                                   const float (*bone_mat)[4],
                                   const float bone_color[4],
                                   const float hint_color[4],
                                   const float outline_color[4])
{
  BoneInstanceData inst_data;
  mul_m4_m4m4(inst_data.mat, ctx->ob->obmat, bone_mat);
  if (ctx->point_solid) {
    bone_instance_data_set_color(&inst_data, bone_color);
    bone_instance_data_set_color_hint(&inst_data, hint_color);
    DRW_buffer_add_entry_struct(ctx->point_solid, &inst_data);
  }
  if (outline_color[3] > 0.0f) {
    bone_instance_data_set_color(&inst_data, outline_color);
    DRW_buffer_add_entry_struct(ctx->point_outline, &inst_data);
  }
}

/* Axes */
static void drw_shgroup_bone_axes(ArmatureDrawContext *ctx,
                                  const float (*bone_mat)[4],
                                  const float color[4])
{
  float mat[4][4];
  mul_m4_m4m4(mat, ctx->ob->obmat, bone_mat);
  /* Move to bone tail. */
  add_v3_v3(mat[3], mat[1]);
  OVERLAY_empty_shape(ctx->extras, mat, 0.25f, OB_ARROWS, color);
}

/* Relationship lines */
static void drw_shgroup_bone_relationship_lines_ex(ArmatureDrawContext *ctx,
                                                   const float start[3],
                                                   const float end[3],
                                                   const float color[4])
{
  float s[3], e[3];
  mul_v3_m4v3(s, ctx->ob->obmat, start);
  mul_v3_m4v3(e, ctx->ob->obmat, end);
  /* reverse order to have less stipple overlap */
  OVERLAY_extra_line_dashed(ctx->extras, s, e, color);
}

static void drw_shgroup_bone_relationship_lines(ArmatureDrawContext *ctx,
                                                const float start[3],
                                                const float end[3])
{
  drw_shgroup_bone_relationship_lines_ex(ctx, start, end, ctx->color.wire);
}

static void drw_shgroup_bone_ik_lines(ArmatureDrawContext *ctx,
                                      const float start[3],
                                      const float end[3])
{
  float fcolor[4] = {0.8f, 0.5f, 0.0f, 1.0f}; /* add theme! */
  drw_shgroup_bone_relationship_lines_ex(ctx, start, end, fcolor);
}

static void drw_shgroup_bone_ik_no_target_lines(ArmatureDrawContext *ctx,
                                                const float start[3],
                                                const float end[3])
{
  float fcolor[4] = {0.8f, 0.8f, 0.2f, 1.0f}; /* add theme! */
  drw_shgroup_bone_relationship_lines_ex(ctx, start, end, fcolor);
}

static void drw_shgroup_bone_ik_spline_lines(ArmatureDrawContext *ctx,
                                             const float start[3],
                                             const float end[3])
{
  float fcolor[4] = {0.8f, 0.8f, 0.2f, 1.0f}; /* add theme! */
  drw_shgroup_bone_relationship_lines_ex(ctx, start, end, fcolor);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing Theme Helpers
 *
 * Note, this section is duplicate of code in 'drawarmature.c'.
 *
 * \{ */

/* values of colCode for set_pchan_color */
enum {
  PCHAN_COLOR_NORMAL = 0, /* normal drawing */
  PCHAN_COLOR_SOLID,      /* specific case where "solid" color is needed */
  PCHAN_COLOR_CONSTS,     /* "constraint" colors (which may/may-not be suppressed) */

  PCHAN_COLOR_SPHEREBONE_BASE, /* for the 'stick' of sphere (envelope) bones */
  PCHAN_COLOR_SPHEREBONE_END,  /* for the ends of sphere (envelope) bones */
  PCHAN_COLOR_LINEBONE,        /* for the middle of line-bones */
};

/* This function sets the color-set for coloring a certain bone */
static void set_pchan_colorset(ArmatureDrawContext *ctx, Object *ob, bPoseChannel *pchan)
{
  bPose *pose = (ob) ? ob->pose : NULL;
  bArmature *arm = (ob) ? ob->data : NULL;
  bActionGroup *grp = NULL;
  short color_index = 0;

  /* sanity check */
  if (ELEM(NULL, ob, arm, pose, pchan)) {
    ctx->bcolor = NULL;
    return;
  }

  /* only try to set custom color if enabled for armature */
  if (arm->flag & ARM_COL_CUSTOM) {
    /* currently, a bone can only use a custom color set if it's group (if it has one),
     * has been set to use one
     */
    if (pchan->agrp_index) {
      grp = (bActionGroup *)BLI_findlink(&pose->agroups, (pchan->agrp_index - 1));
      if (grp) {
        color_index = grp->customCol;
      }
    }
  }

  /* bcolor is a pointer to the color set to use. If NULL, then the default
   * color set (based on the theme colors for 3d-view) is used.
   */
  if (color_index > 0) {
    bTheme *btheme = UI_GetTheme();
    ctx->bcolor = &btheme->tarm[(color_index - 1)];
  }
  else if (color_index == -1) {
    /* use the group's own custom color set (grp is always != NULL here) */
    ctx->bcolor = &grp->cs;
  }
  else {
    ctx->bcolor = NULL;
  }
}

/* This function is for brightening/darkening a given color (like UI_GetThemeColorShade3ubv()) */
static void cp_shade_color3ub(uchar cp[3], const int offset)
{
  int r, g, b;

  r = offset + (int)cp[0];
  CLAMP(r, 0, 255);
  g = offset + (int)cp[1];
  CLAMP(g, 0, 255);
  b = offset + (int)cp[2];
  CLAMP(b, 0, 255);

  cp[0] = r;
  cp[1] = g;
  cp[2] = b;
}

static void cp_shade_color3f(float cp[3], const float offset)
{
  add_v3_fl(cp, offset);
  CLAMP(cp[0], 0, 255);
  CLAMP(cp[1], 0, 255);
  CLAMP(cp[2], 0, 255);
}

/* This function sets the gl-color for coloring a certain bone (based on bcolor) */
static bool set_pchan_color(const ArmatureDrawContext *ctx,
                            short colCode,
                            const int boneflag,
                            const short constflag,
                            float r_color[4])
{
  float *fcolor = r_color;
  const ThemeWireColor *bcolor = ctx->bcolor;

  switch (colCode) {
    case PCHAN_COLOR_NORMAL: {
      if (bcolor) {
        uchar cp[4] = {255};

        if (boneflag & BONE_DRAW_ACTIVE) {
          copy_v3_v3_uchar(cp, bcolor->active);
          if (!(boneflag & BONE_SELECTED)) {
            cp_shade_color3ub(cp, -80);
          }
        }
        else if (boneflag & BONE_SELECTED) {
          copy_v3_v3_uchar(cp, bcolor->select);
        }
        else {
          /* a bit darker than solid */
          copy_v3_v3_uchar(cp, bcolor->solid);
          cp_shade_color3ub(cp, -50);
        }

        rgb_uchar_to_float(fcolor, cp);
      }
      else {
        if ((boneflag & BONE_DRAW_ACTIVE) && (boneflag & BONE_SELECTED)) {
          UI_GetThemeColor4fv(TH_BONE_POSE_ACTIVE, fcolor);
        }
        else if (boneflag & BONE_DRAW_ACTIVE) {
          UI_GetThemeColorBlendShade4fv(TH_WIRE, TH_BONE_POSE, 0.15f, 0, fcolor);
        }
        else if (boneflag & BONE_SELECTED) {
          UI_GetThemeColor4fv(TH_BONE_POSE, fcolor);
        }
        else {
          UI_GetThemeColor4fv(TH_WIRE, fcolor);
        }
      }

      return true;
    }
    case PCHAN_COLOR_SOLID: {
      UI_GetThemeColor4fv(TH_BONE_SOLID, fcolor);

      if (bcolor) {
        float solid_bcolor[3];
        rgb_uchar_to_float(solid_bcolor, (uchar *)bcolor->solid);
        interp_v3_v3v3(fcolor, fcolor, solid_bcolor, 1.0f);
      }

      return true;
    }
    case PCHAN_COLOR_CONSTS: {
      if ((bcolor == NULL) || (bcolor->flag & TH_WIRECOLOR_CONSTCOLS)) {
        uchar cp[4];
        if (constflag & PCHAN_HAS_TARGET) {
          rgba_uchar_args_set(cp, 255, 150, 0, 80);
        }
        else if (constflag & PCHAN_HAS_IK) {
          rgba_uchar_args_set(cp, 255, 255, 0, 80);
        }
        else if (constflag & PCHAN_HAS_SPLINEIK) {
          rgba_uchar_args_set(cp, 200, 255, 0, 80);
        }
        else if (constflag & PCHAN_HAS_CONST) {
          rgba_uchar_args_set(cp, 0, 255, 120, 80);
        }
        else {
          return false;
        }

        rgba_uchar_to_float(fcolor, cp);

        return true;
      }
      return false;
    }
    case PCHAN_COLOR_SPHEREBONE_BASE: {
      if (bcolor) {
        uchar cp[4] = {255};

        if (boneflag & BONE_DRAW_ACTIVE) {
          copy_v3_v3_uchar(cp, bcolor->active);
        }
        else if (boneflag & BONE_SELECTED) {
          copy_v3_v3_uchar(cp, bcolor->select);
        }
        else {
          copy_v3_v3_uchar(cp, bcolor->solid);
        }

        rgb_uchar_to_float(fcolor, cp);
      }
      else {
        if (boneflag & BONE_DRAW_ACTIVE) {
          UI_GetThemeColorShade4fv(TH_BONE_POSE, 40, fcolor);
        }
        else if (boneflag & BONE_SELECTED) {
          UI_GetThemeColor4fv(TH_BONE_POSE, fcolor);
        }
        else {
          UI_GetThemeColor4fv(TH_BONE_SOLID, fcolor);
        }
      }

      return true;
    }
    case PCHAN_COLOR_SPHEREBONE_END: {
      if (bcolor) {
        uchar cp[4] = {255};

        if (boneflag & BONE_DRAW_ACTIVE) {
          copy_v3_v3_uchar(cp, bcolor->active);
          cp_shade_color3ub(cp, 10);
        }
        else if (boneflag & BONE_SELECTED) {
          copy_v3_v3_uchar(cp, bcolor->select);
          cp_shade_color3ub(cp, -30);
        }
        else {
          copy_v3_v3_uchar(cp, bcolor->solid);
          cp_shade_color3ub(cp, -30);
        }

        rgb_uchar_to_float(fcolor, cp);
      }
      else {
        if (boneflag & BONE_DRAW_ACTIVE) {
          UI_GetThemeColorShade4fv(TH_BONE_POSE, 10, fcolor);
        }
        else if (boneflag & BONE_SELECTED) {
          UI_GetThemeColorShade4fv(TH_BONE_POSE, -30, fcolor);
        }
        else {
          UI_GetThemeColorShade4fv(TH_BONE_SOLID, -30, fcolor);
        }
      }
      break;
    }
    case PCHAN_COLOR_LINEBONE: {
      /* inner part in background color or constraint */
      if ((constflag) && ((bcolor == NULL) || (bcolor->flag & TH_WIRECOLOR_CONSTCOLS))) {
        uchar cp[4];
        if (constflag & PCHAN_HAS_TARGET) {
          rgba_uchar_args_set(cp, 255, 150, 0, 255);
        }
        else if (constflag & PCHAN_HAS_IK) {
          rgba_uchar_args_set(cp, 255, 255, 0, 255);
        }
        else if (constflag & PCHAN_HAS_SPLINEIK) {
          rgba_uchar_args_set(cp, 200, 255, 0, 255);
        }
        else if (constflag & PCHAN_HAS_CONST) {
          rgba_uchar_args_set(cp, 0, 255, 120, 255);
        }
        else if (constflag) {
          UI_GetThemeColor4ubv(TH_BONE_POSE, cp);
        } /* PCHAN_HAS_ACTION */

        rgb_uchar_to_float(fcolor, cp);
      }
      else {
        if (bcolor) {
          const uchar *cp = bcolor->solid;
          rgb_uchar_to_float(fcolor, (uchar *)cp);
          fcolor[3] = 204.f / 255.f;
        }
        else {
          UI_GetThemeColorShade4fv(TH_BACK, -30, fcolor);
        }
      }

      return true;
    }
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing Color Helpers
 * \{ */

static const float *get_bone_solid_color(const ArmatureDrawContext *ctx,
                                         const EditBone *UNUSED(eBone),
                                         const bPoseChannel *pchan,
                                         const bArmature *arm,
                                         const int boneflag,
                                         const short constflag)
{
  if (ctx->const_color) {
    return ctx->color.bone_solid;
  }

  if (arm->flag & ARM_POSEMODE) {
    static float disp_color[4];
    copy_v4_v4(disp_color, pchan->draw_data->solid_color);
    set_pchan_color(ctx, PCHAN_COLOR_SOLID, boneflag, constflag, disp_color);
    return disp_color;
  }

  return ctx->color.bone_solid;
}

static const float *get_bone_solid_with_consts_color(const ArmatureDrawContext *ctx,
                                                     const EditBone *eBone,
                                                     const bPoseChannel *pchan,
                                                     const bArmature *arm,
                                                     const int boneflag,
                                                     const short constflag)
{
  if (ctx->const_color) {
    return ctx->color.bone_solid;
  }

  const float *col = get_bone_solid_color(ctx, eBone, pchan, arm, boneflag, constflag);

  static float consts_color[4];
  if (set_pchan_color(ctx, PCHAN_COLOR_CONSTS, boneflag, constflag, consts_color)) {
    interp_v3_v3v3(consts_color, col, consts_color, 0.5f);
  }
  else {
    copy_v4_v4(consts_color, col);
  }
  return consts_color;
}

static float get_bone_wire_thickness(const ArmatureDrawContext *ctx, int boneflag)
{
  if (ctx->const_color) {
    return ctx->const_wire;
  }
  else if (boneflag & (BONE_DRAW_ACTIVE | BONE_SELECTED)) {
    return 2.0f;
  }
  else {
    return 1.0f;
  }
}

static const float *get_bone_wire_color(const ArmatureDrawContext *ctx,
                                        const EditBone *eBone,
                                        const bPoseChannel *pchan,
                                        const bArmature *arm,
                                        const int boneflag,
                                        const short constflag)
{
  static float disp_color[4];

  if (ctx->const_color) {
    copy_v3_v3(disp_color, ctx->const_color);
  }
  else if (eBone) {
    if (boneflag & BONE_SELECTED) {
      if (boneflag & BONE_DRAW_ACTIVE) {
        copy_v3_v3(disp_color, ctx->color.edge_select);
      }
      else {
        copy_v3_v3(disp_color, ctx->color.bone_select);
      }
    }
    else {
      if (boneflag & BONE_DRAW_ACTIVE) {
        copy_v3_v3(disp_color, ctx->color.bone_active_unselect);
      }
      else {
        copy_v3_v3(disp_color, ctx->color.wire_edit);
      }
    }
  }
  else if (arm->flag & ARM_POSEMODE) {
    copy_v4_v4(disp_color, pchan->draw_data->wire_color);
    set_pchan_color(ctx, PCHAN_COLOR_NORMAL, boneflag, constflag, disp_color);
  }
  else {
    copy_v3_v3(disp_color, ctx->color.vertex);
  }

  disp_color[3] = get_bone_wire_thickness(ctx, boneflag);

  return disp_color;
}

#define HINT_MUL 0.5f
#define HINT_SHADE 0.2f

static void bone_hint_color_shade(float hint_color[4], const float color[4])
{
  mul_v3_v3fl(hint_color, color, HINT_MUL);
  cp_shade_color3f(hint_color, -HINT_SHADE);
  hint_color[3] = 1.0f;
}

static const float *get_bone_hint_color(const ArmatureDrawContext *ctx,
                                        const EditBone *eBone,
                                        const bPoseChannel *pchan,
                                        const bArmature *arm,
                                        const int boneflag,
                                        const short constflag)
{
  static float hint_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  if (ctx->const_color) {
    bone_hint_color_shade(hint_color, ctx->color.bone_solid);
  }
  else {
    const float *wire_color = get_bone_wire_color(ctx, eBone, pchan, arm, boneflag, constflag);
    bone_hint_color_shade(hint_color, wire_color);
  }

  return hint_color;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helper Utils
 * \{ */

static void pchan_draw_data_init(bPoseChannel *pchan)
{
  if (pchan->draw_data != NULL) {
    if (pchan->draw_data->bbone_matrix_len != pchan->bone->segments) {
      MEM_SAFE_FREE(pchan->draw_data);
    }
  }

  if (pchan->draw_data == NULL) {
    pchan->draw_data = MEM_mallocN(
        sizeof(*pchan->draw_data) + sizeof(Mat4) * pchan->bone->segments, __func__);
    pchan->draw_data->bbone_matrix_len = pchan->bone->segments;
  }
}

static void draw_bone_update_disp_matrix_default(EditBone *eBone, bPoseChannel *pchan)
{
  float s[4][4], ebmat[4][4];
  float length;
  float(*bone_mat)[4];
  float(*disp_mat)[4];
  float(*disp_tail_mat)[4];

  /* TODO : This should be moved to depsgraph or armature refresh
   * and not be tight to the draw pass creation.
   * This would refresh armature without invalidating the draw cache */
  if (pchan) {
    length = pchan->bone->length;
    bone_mat = pchan->pose_mat;
    disp_mat = pchan->disp_mat;
    disp_tail_mat = pchan->disp_tail_mat;
  }
  else {
    eBone->length = len_v3v3(eBone->tail, eBone->head);
    ED_armature_ebone_to_mat4(eBone, ebmat);

    length = eBone->length;
    bone_mat = ebmat;
    disp_mat = eBone->disp_mat;
    disp_tail_mat = eBone->disp_tail_mat;
  }

  scale_m4_fl(s, length);
  mul_m4_m4m4(disp_mat, bone_mat, s);
  copy_m4_m4(disp_tail_mat, disp_mat);
  translate_m4(disp_tail_mat, 0.0f, 1.0f, 0.0f);
}

/* compute connected child pointer for B-Bone drawing */
static void edbo_compute_bbone_child(bArmature *arm)
{
  EditBone *eBone;

  for (eBone = arm->edbo->first; eBone; eBone = eBone->next) {
    eBone->bbone_child = NULL;
  }

  for (eBone = arm->edbo->first; eBone; eBone = eBone->next) {
    if (eBone->parent && (eBone->flag & BONE_CONNECTED)) {
      eBone->parent->bbone_child = eBone;
    }
  }
}

/* A version of BKE_pchan_bbone_spline_setup() for previewing editmode curve settings. */
static void ebone_spline_preview(EditBone *ebone, float result_array[MAX_BBONE_SUBDIV][4][4])
{
  BBoneSplineParameters param;
  EditBone *prev, *next;
  float imat[4][4], bonemat[4][4];
  float tmp[3];

  memset(&param, 0, sizeof(param));

  param.segments = ebone->segments;
  param.length = ebone->length;

  /* Get "next" and "prev" bones - these are used for handle calculations. */
  if (ebone->bbone_prev_type == BBONE_HANDLE_AUTO) {
    /* Use connected parent. */
    if (ebone->flag & BONE_CONNECTED) {
      prev = ebone->parent;
    }
    else {
      prev = NULL;
    }
  }
  else {
    prev = ebone->bbone_prev;
  }

  if (ebone->bbone_next_type == BBONE_HANDLE_AUTO) {
    /* Use connected child. */
    next = ebone->bbone_child;
  }
  else {
    next = ebone->bbone_next;
  }

  /* compute handles from connected bones */
  if (prev || next) {
    ED_armature_ebone_to_mat4(ebone, imat);
    invert_m4(imat);

    if (prev) {
      param.use_prev = true;

      if (ebone->bbone_prev_type == BBONE_HANDLE_RELATIVE) {
        zero_v3(param.prev_h);
      }
      else if (ebone->bbone_prev_type == BBONE_HANDLE_TANGENT) {
        sub_v3_v3v3(tmp, prev->tail, prev->head);
        sub_v3_v3v3(tmp, ebone->head, tmp);
        mul_v3_m4v3(param.prev_h, imat, tmp);
      }
      else {
        param.prev_bbone = (prev->segments > 1);

        mul_v3_m4v3(param.prev_h, imat, prev->head);
      }

      if (!param.prev_bbone) {
        ED_armature_ebone_to_mat4(prev, bonemat);
        mul_m4_m4m4(param.prev_mat, imat, bonemat);
      }
    }

    if (next) {
      param.use_next = true;

      if (ebone->bbone_next_type == BBONE_HANDLE_RELATIVE) {
        copy_v3_fl3(param.next_h, 0.0f, param.length, 0.0);
      }
      else if (ebone->bbone_next_type == BBONE_HANDLE_TANGENT) {
        sub_v3_v3v3(tmp, next->tail, next->head);
        add_v3_v3v3(tmp, ebone->tail, tmp);
        mul_v3_m4v3(param.next_h, imat, tmp);
      }
      else {
        param.next_bbone = (next->segments > 1);

        mul_v3_m4v3(param.next_h, imat, next->tail);
      }

      ED_armature_ebone_to_mat4(next, bonemat);
      mul_m4_m4m4(param.next_mat, imat, bonemat);
    }
  }

  param.ease1 = ebone->ease1;
  param.ease2 = ebone->ease2;
  param.roll1 = ebone->roll1;
  param.roll2 = ebone->roll2;

  if (prev && (ebone->flag & BONE_ADD_PARENT_END_ROLL)) {
    param.roll1 += prev->roll2;
  }

  param.scale_in_x = ebone->scale_in_x;
  param.scale_in_y = ebone->scale_in_y;

  param.scale_out_x = ebone->scale_out_x;
  param.scale_out_y = ebone->scale_out_y;

  param.curve_in_x = ebone->curve_in_x;
  param.curve_in_y = ebone->curve_in_y;

  param.curve_out_x = ebone->curve_out_x;
  param.curve_out_y = ebone->curve_out_y;

  ebone->segments = BKE_pchan_bbone_spline_compute(&param, false, (Mat4 *)result_array);
}

static void draw_bone_update_disp_matrix_bbone(EditBone *eBone, bPoseChannel *pchan)
{
  float s[4][4], ebmat[4][4];
  float length, xwidth, zwidth;
  float(*bone_mat)[4];
  short bbone_segments;

  /* TODO : This should be moved to depsgraph or armature refresh
   * and not be tight to the draw pass creation.
   * This would refresh armature without invalidating the draw cache */
  if (pchan) {
    length = pchan->bone->length;
    xwidth = pchan->bone->xwidth;
    zwidth = pchan->bone->zwidth;
    bone_mat = pchan->pose_mat;
    bbone_segments = pchan->bone->segments;
  }
  else {
    eBone->length = len_v3v3(eBone->tail, eBone->head);
    ED_armature_ebone_to_mat4(eBone, ebmat);

    length = eBone->length;
    xwidth = eBone->xwidth;
    zwidth = eBone->zwidth;
    bone_mat = ebmat;
    bbone_segments = eBone->segments;
  }

  size_to_mat4(s, (const float[3]){xwidth, length / bbone_segments, zwidth});

  /* Compute BBones segment matrices... */
  /* Note that we need this even for one-segment bones, because box drawing need specific weirdo
   * matrix for the box, that we cannot use to draw end points & co. */
  if (pchan) {
    Mat4 *bbones_mat = (Mat4 *)pchan->draw_data->bbone_matrix;
    if (bbone_segments > 1) {
      BKE_pchan_bbone_spline_setup(pchan, false, false, bbones_mat);

      for (int i = bbone_segments; i--; bbones_mat++) {
        mul_m4_m4m4(bbones_mat->mat, bbones_mat->mat, s);
        mul_m4_m4m4(bbones_mat->mat, bone_mat, bbones_mat->mat);
      }
    }
    else {
      mul_m4_m4m4(bbones_mat->mat, bone_mat, s);
    }
  }
  else {
    float(*bbones_mat)[4][4] = eBone->disp_bbone_mat;

    if (bbone_segments > 1) {
      ebone_spline_preview(eBone, bbones_mat);

      for (int i = bbone_segments; i--; bbones_mat++) {
        mul_m4_m4m4(*bbones_mat, *bbones_mat, s);
        mul_m4_m4m4(*bbones_mat, bone_mat, *bbones_mat);
      }
    }
    else {
      mul_m4_m4m4(*bbones_mat, bone_mat, s);
    }
  }

  /* Grrr... We need default display matrix to draw end points, axes, etc. :( */
  draw_bone_update_disp_matrix_default(eBone, pchan);
}

static void draw_bone_update_disp_matrix_custom(bPoseChannel *pchan)
{
  float s[4][4];
  float length;
  float(*bone_mat)[4];
  float(*disp_mat)[4];
  float(*disp_tail_mat)[4];

  /* See TODO above */
  length = PCHAN_CUSTOM_DRAW_SIZE(pchan);
  bone_mat = pchan->custom_tx ? pchan->custom_tx->pose_mat : pchan->pose_mat;
  disp_mat = pchan->disp_mat;
  disp_tail_mat = pchan->disp_tail_mat;

  scale_m4_fl(s, length);
  mul_m4_m4m4(disp_mat, bone_mat, s);
  copy_m4_m4(disp_tail_mat, disp_mat);
  translate_m4(disp_tail_mat, 0.0f, 1.0f, 0.0f);
}

static void draw_axes(ArmatureDrawContext *ctx, EditBone *eBone, bPoseChannel *pchan)
{
  float final_col[4];
  const float *col = (ctx->const_color) ?
                         ctx->const_color :
                         (BONE_FLAG(eBone, pchan) & BONE_SELECTED) ? ctx->color.text_hi :
                                                                     ctx->color.text;
  copy_v4_v4(final_col, col);
  /* Mix with axes color. */
  final_col[3] = (ctx->const_color) ? 1.0 : (BONE_FLAG(eBone, pchan) & BONE_SELECTED) ? 0.3 : 0.8;
  drw_shgroup_bone_axes(ctx, BONE_VAR(eBone, pchan, disp_mat), final_col);
}

static void draw_points(ArmatureDrawContext *ctx,
                        const EditBone *eBone,
                        const bPoseChannel *pchan,
                        const bArmature *arm,
                        const int boneflag,
                        const short constflag,
                        const int select_id)
{
  float col_solid_root[4], col_solid_tail[4], col_wire_root[4], col_wire_tail[4];
  float col_hint_root[4], col_hint_tail[4];

  copy_v4_v4(col_solid_root, ctx->color.bone_solid);
  copy_v4_v4(col_solid_tail, ctx->color.bone_solid);
  copy_v4_v4(col_wire_root, (ctx->const_color) ? ctx->const_color : ctx->color.vertex);
  copy_v4_v4(col_wire_tail, (ctx->const_color) ? ctx->const_color : ctx->color.vertex);

  const bool is_envelope_draw = (arm->drawtype == ARM_ENVELOPE);
  const float envelope_ignore = -1.0f;

  col_wire_tail[3] = col_wire_root[3] = get_bone_wire_thickness(ctx, boneflag);

  /* Edit bone points can be selected */
  if (eBone) {
    if (eBone->flag & BONE_ROOTSEL) {
      copy_v3_v3(col_wire_root, ctx->color.vertex_select);
    }
    if (eBone->flag & BONE_TIPSEL) {
      copy_v3_v3(col_wire_tail, ctx->color.vertex_select);
    }
  }
  else if (arm->flag & ARM_POSEMODE) {
    const float *solid_color = get_bone_solid_color(ctx, eBone, pchan, arm, boneflag, constflag);
    const float *wire_color = get_bone_wire_color(ctx, eBone, pchan, arm, boneflag, constflag);
    copy_v4_v4(col_wire_tail, wire_color);
    copy_v4_v4(col_wire_root, wire_color);
    copy_v4_v4(col_solid_tail, solid_color);
    copy_v4_v4(col_solid_root, solid_color);
  }

  bone_hint_color_shade(col_hint_root, (ctx->const_color) ? col_solid_root : col_wire_root);
  bone_hint_color_shade(col_hint_tail, (ctx->const_color) ? col_solid_tail : col_wire_tail);

  /* Draw root point if we are not connected to our parent */
  if (!(eBone ? (eBone->parent && (eBone->flag & BONE_CONNECTED)) :
                (pchan->bone->parent && (pchan->bone->flag & BONE_CONNECTED)))) {
    if (select_id != -1) {
      DRW_select_load_id(select_id | BONESEL_ROOT);
    }

    if (eBone) {
      if (is_envelope_draw) {
        drw_shgroup_bone_envelope(ctx,
                                  eBone->disp_mat,
                                  col_solid_root,
                                  col_hint_root,
                                  col_wire_root,
                                  &eBone->rad_head,
                                  &envelope_ignore);
      }
      else {
        drw_shgroup_bone_point(ctx, eBone->disp_mat, col_solid_root, col_hint_root, col_wire_root);
      }
    }
    else {
      Bone *bone = pchan->bone;
      if (is_envelope_draw) {
        drw_shgroup_bone_envelope(ctx,
                                  pchan->disp_mat,
                                  col_solid_root,
                                  col_hint_root,
                                  col_wire_root,
                                  &bone->rad_head,
                                  &envelope_ignore);
      }
      else {
        drw_shgroup_bone_point(ctx, pchan->disp_mat, col_solid_root, col_hint_root, col_wire_root);
      }
    }
  }

  /*  Draw tip point */
  if (select_id != -1) {
    DRW_select_load_id(select_id | BONESEL_TIP);
  }

  if (is_envelope_draw) {
    const float *rad_tail = eBone ? &eBone->rad_tail : &pchan->bone->rad_tail;
    drw_shgroup_bone_envelope(ctx,
                              BONE_VAR(eBone, pchan, disp_mat),
                              col_solid_tail,
                              col_hint_tail,
                              col_wire_tail,
                              &envelope_ignore,
                              rad_tail);
  }
  else {
    drw_shgroup_bone_point(
        ctx, BONE_VAR(eBone, pchan, disp_tail_mat), col_solid_tail, col_hint_tail, col_wire_tail);
  }

  if (select_id != -1) {
    DRW_select_load_id(-1);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw Bones
 * \{ */

static void draw_bone_custom_shape(ArmatureDrawContext *ctx,
                                   EditBone *eBone,
                                   bPoseChannel *pchan,
                                   bArmature *arm,
                                   const int boneflag,
                                   const short constflag,
                                   const int select_id)
{
  const float *col_solid = get_bone_solid_color(ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_wire = get_bone_wire_color(ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_hint = get_bone_hint_color(ctx, eBone, pchan, arm, boneflag, constflag);
  const float(*disp_mat)[4] = pchan->disp_mat;

  if (select_id != -1) {
    DRW_select_load_id(select_id | BONESEL_BONE);
  }

  if (pchan->custom->type == OB_EMPTY) {
    Object *ob = pchan->custom;
    if (ob->empty_drawtype != OB_EMPTY_IMAGE) {
      drw_shgroup_bone_custom_empty(ctx, disp_mat, col_wire, pchan->custom);
    }
  }
  if ((boneflag & BONE_DRAWWIRE) == 0) {
    drw_shgroup_bone_custom_solid(ctx, disp_mat, col_solid, col_hint, col_wire, pchan->custom);
  }
  else {
    drw_shgroup_bone_custom_wire(ctx, disp_mat, col_wire, pchan->custom);
  }

  if (select_id != -1) {
    DRW_select_load_id(-1);
  }
}

static void draw_bone_envelope(ArmatureDrawContext *ctx,
                               EditBone *eBone,
                               bPoseChannel *pchan,
                               bArmature *arm,
                               const int boneflag,
                               const short constflag,
                               const int select_id)
{
  const float *col_solid = get_bone_solid_with_consts_color(
      ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_wire = get_bone_wire_color(ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_hint = get_bone_hint_color(ctx, eBone, pchan, arm, boneflag, constflag);

  float *rad_head, *rad_tail, *distance;
  if (eBone) {
    rad_tail = &eBone->rad_tail;
    distance = &eBone->dist;
    rad_head = (eBone->parent && (boneflag & BONE_CONNECTED)) ? &eBone->parent->rad_tail :
                                                                &eBone->rad_head;
  }
  else {
    rad_tail = &pchan->bone->rad_tail;
    distance = &pchan->bone->dist;
    rad_head = (pchan->parent && (boneflag & BONE_CONNECTED)) ? &pchan->parent->bone->rad_tail :
                                                                &pchan->bone->rad_head;
  }

  if ((select_id == -1) && (boneflag & BONE_NO_DEFORM) == 0 &&
      ((boneflag & BONE_SELECTED) || (eBone && (boneflag & (BONE_ROOTSEL | BONE_TIPSEL))))) {
    drw_shgroup_bone_envelope_distance(
        ctx, BONE_VAR(eBone, pchan, disp_mat), rad_head, rad_tail, distance);
  }

  if (select_id != -1) {
    DRW_select_load_id(select_id | BONESEL_BONE);
  }

  drw_shgroup_bone_envelope(
      ctx, BONE_VAR(eBone, pchan, disp_mat), col_solid, col_hint, col_wire, rad_head, rad_tail);

  if (select_id != -1) {
    DRW_select_load_id(-1);
  }

  draw_points(ctx, eBone, pchan, arm, boneflag, constflag, select_id);
}

static void draw_bone_line(ArmatureDrawContext *ctx,
                           EditBone *eBone,
                           bPoseChannel *pchan,
                           bArmature *arm,
                           const int boneflag,
                           const short constflag,
                           const int select_id)
{
  const float *col_bone = get_bone_solid_with_consts_color(
      ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_wire = get_bone_wire_color(ctx, eBone, pchan, arm, boneflag, constflag);
  const float no_display[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const float *col_head = no_display;
  const float *col_tail = col_bone;

  if (ctx->const_color != NULL) {
    col_wire = no_display; /* actually shrink the display. */
    col_bone = col_head = col_tail = ctx->const_color;
  }
  else {
    if (eBone) {
      if (eBone->flag & BONE_TIPSEL) {
        col_tail = ctx->color.vertex_select;
      }
      if (boneflag & BONE_SELECTED) {
        col_bone = ctx->color.edge_select;
      }
      col_wire = ctx->color.wire;
    }

    /* Draw root point if we are not connected to our parent. */
    if (!(eBone ? (eBone->parent && (eBone->flag & BONE_CONNECTED)) :
                  (pchan->bone->parent && (pchan->bone->flag & BONE_CONNECTED)))) {

      if (eBone) {
        col_head = (eBone->flag & BONE_ROOTSEL) ? ctx->color.vertex_select : col_bone;
      }
      else {
        col_head = col_bone;
      }
    }
  }

  if (select_id == -1) {
    /* Not in selection mode, draw everything at once. */
    drw_shgroup_bone_stick(
        ctx, BONE_VAR(eBone, pchan, disp_mat), col_wire, col_bone, col_head, col_tail);
  }
  else {
    /* In selection mode, draw bone, root and tip separately. */
    DRW_select_load_id(select_id | BONESEL_BONE);
    drw_shgroup_bone_stick(
        ctx, BONE_VAR(eBone, pchan, disp_mat), col_wire, col_bone, no_display, no_display);

    if (col_head[3] > 0.0f) {
      DRW_select_load_id(select_id | BONESEL_ROOT);
      drw_shgroup_bone_stick(
          ctx, BONE_VAR(eBone, pchan, disp_mat), col_wire, no_display, col_head, no_display);
    }

    DRW_select_load_id(select_id | BONESEL_TIP);
    drw_shgroup_bone_stick(
        ctx, BONE_VAR(eBone, pchan, disp_mat), col_wire, no_display, no_display, col_tail);

    DRW_select_load_id(-1);
  }
}

static void draw_bone_wire(ArmatureDrawContext *ctx,
                           EditBone *eBone,
                           bPoseChannel *pchan,
                           bArmature *arm,
                           const int boneflag,
                           const short constflag,
                           const int select_id)
{
  const float *col_wire = get_bone_wire_color(ctx, eBone, pchan, arm, boneflag, constflag);

  if (select_id != -1) {
    DRW_select_load_id(select_id | BONESEL_BONE);
  }

  if (pchan) {
    Mat4 *bbones_mat = (Mat4 *)pchan->draw_data->bbone_matrix;
    BLI_assert(bbones_mat != NULL);

    for (int i = pchan->bone->segments; i--; bbones_mat++) {
      drw_shgroup_bone_wire(ctx, bbones_mat->mat, col_wire);
    }
  }
  else if (eBone) {
    for (int i = 0; i < eBone->segments; i++) {
      drw_shgroup_bone_wire(ctx, eBone->disp_bbone_mat[i], col_wire);
    }
  }

  if (select_id != -1) {
    DRW_select_load_id(-1);
  }

  if (eBone) {
    draw_points(ctx, eBone, pchan, arm, boneflag, constflag, select_id);
  }
}

static void draw_bone_box(ArmatureDrawContext *ctx,
                          EditBone *eBone,
                          bPoseChannel *pchan,
                          bArmature *arm,
                          const int boneflag,
                          const short constflag,
                          const int select_id)
{
  const float *col_solid = get_bone_solid_with_consts_color(
      ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_wire = get_bone_wire_color(ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_hint = get_bone_hint_color(ctx, eBone, pchan, arm, boneflag, constflag);

  if (select_id != -1) {
    DRW_select_load_id(select_id | BONESEL_BONE);
  }

  if (pchan) {
    Mat4 *bbones_mat = (Mat4 *)pchan->draw_data->bbone_matrix;
    BLI_assert(bbones_mat != NULL);

    for (int i = pchan->bone->segments; i--; bbones_mat++) {
      drw_shgroup_bone_box(ctx, bbones_mat->mat, col_solid, col_hint, col_wire);
    }
  }
  else if (eBone) {
    for (int i = 0; i < eBone->segments; i++) {
      drw_shgroup_bone_box(ctx, eBone->disp_bbone_mat[i], col_solid, col_hint, col_wire);
    }
  }

  if (select_id != -1) {
    DRW_select_load_id(-1);
  }

  if (eBone) {
    draw_points(ctx, eBone, pchan, arm, boneflag, constflag, select_id);
  }
}

static void draw_bone_octahedral(ArmatureDrawContext *ctx,
                                 EditBone *eBone,
                                 bPoseChannel *pchan,
                                 bArmature *arm,
                                 const int boneflag,
                                 const short constflag,
                                 const int select_id)
{
  const float *col_solid = get_bone_solid_with_consts_color(
      ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_wire = get_bone_wire_color(ctx, eBone, pchan, arm, boneflag, constflag);
  const float *col_hint = get_bone_hint_color(ctx, eBone, pchan, arm, boneflag, constflag);

  if (select_id != -1) {
    DRW_select_load_id(select_id | BONESEL_BONE);
  }

  drw_shgroup_bone_octahedral(
      ctx, BONE_VAR(eBone, pchan, disp_mat), col_solid, col_hint, col_wire);

  if (select_id != -1) {
    DRW_select_load_id(-1);
  }

  draw_points(ctx, eBone, pchan, arm, boneflag, constflag, select_id);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw Degrees of Freedom
 * \{ */

static void draw_bone_degrees_of_freedom(ArmatureDrawContext *ctx, bPoseChannel *pchan)
{
  BoneInstanceData inst_data;
  float tmp[4][4], posetrans[4][4];
  float xminmax[2], zminmax[2];
  float color[4];

  if (ctx->dof_sphere == NULL) {
    return;
  }

  /* *0.5f here comes from M_PI/360.0f when rotations were still in degrees */
  xminmax[0] = sinf(pchan->limitmin[0] * 0.5f);
  xminmax[1] = sinf(pchan->limitmax[0] * 0.5f);
  zminmax[0] = sinf(pchan->limitmin[2] * 0.5f);
  zminmax[1] = sinf(pchan->limitmax[2] * 0.5f);

  unit_m4(posetrans);
  translate_m4(posetrans, pchan->pose_mat[3][0], pchan->pose_mat[3][1], pchan->pose_mat[3][2]);
  /* in parent-bone pose space... */
  if (pchan->parent) {
    copy_m4_m4(tmp, pchan->parent->pose_mat);
    zero_v3(tmp[3]);
    mul_m4_m4m4(posetrans, posetrans, tmp);
  }
  /* ... but own restspace */
  mul_m4_m4m3(posetrans, posetrans, pchan->bone->bone_mat);

  float scale = pchan->bone->length * pchan->size[1];
  scale_m4_fl(tmp, scale);
  tmp[1][1] = -tmp[1][1];
  mul_m4_m4m4(posetrans, posetrans, tmp);

  /* into world space. */
  mul_m4_m4m4(inst_data.mat, ctx->ob->obmat, posetrans);

  if ((pchan->ikflag & BONE_IK_XLIMIT) && (pchan->ikflag & BONE_IK_ZLIMIT)) {
    bone_instance_data_set_angle_minmax(
        &inst_data, xminmax[0], zminmax[0], xminmax[1], zminmax[1]);

    copy_v4_fl4(color, 0.25f, 0.25f, 0.25f, 0.25f);
    DRW_buffer_add_entry(ctx->dof_sphere, color, &inst_data);

    copy_v4_fl4(color, 0.0f, 0.0f, 0.0f, 1.0f);
    DRW_buffer_add_entry(ctx->dof_lines, color, &inst_data);
  }
  if (pchan->ikflag & BONE_IK_XLIMIT) {
    bone_instance_data_set_angle_minmax(&inst_data, xminmax[0], 0.0f, xminmax[1], 0.0f);
    copy_v4_fl4(color, 1.0f, 0.0f, 0.0f, 1.0f);
    DRW_buffer_add_entry(ctx->dof_lines, color, &inst_data);
  }
  if (pchan->ikflag & BONE_IK_ZLIMIT) {
    bone_instance_data_set_angle_minmax(&inst_data, 0.0f, zminmax[0], 0.0f, zminmax[1]);
    copy_v4_fl4(color, 0.0f, 0.0f, 1.0f, 1.0f);
    DRW_buffer_add_entry(ctx->dof_lines, color, &inst_data);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw Relationships
 * \{ */

static void pchan_draw_ik_lines(ArmatureDrawContext *ctx,
                                bPoseChannel *pchan,
                                const bool only_temp,
                                const int constflag)
{
  bConstraint *con;
  bPoseChannel *parchan;
  float *line_start = NULL, *line_end = NULL;

  for (con = pchan->constraints.first; con; con = con->next) {
    if (con->enforce == 0.0f) {
      continue;
    }

    switch (con->type) {
      case CONSTRAINT_TYPE_KINEMATIC: {
        bKinematicConstraint *data = (bKinematicConstraint *)con->data;
        int segcount = 0;

        /* if only_temp, only draw if it is a temporary ik-chain */
        if (only_temp && !(data->flag & CONSTRAINT_IK_TEMP)) {
          continue;
        }

        /* exclude tip from chain? */
        parchan = ((data->flag & CONSTRAINT_IK_TIP) == 0) ? pchan->parent : pchan;
        line_start = parchan->pose_tail;

        /* Find the chain's root */
        while (parchan->parent) {
          segcount++;
          if (segcount == data->rootbone || segcount > 255) {
            break; /* 255 is weak */
          }
          parchan = parchan->parent;
        }

        if (parchan) {
          line_end = parchan->pose_head;

          if (constflag & PCHAN_HAS_TARGET) {
            drw_shgroup_bone_ik_lines(ctx, line_start, line_end);
          }
          else {
            drw_shgroup_bone_ik_no_target_lines(ctx, line_start, line_end);
          }
        }
        break;
      }
      case CONSTRAINT_TYPE_SPLINEIK: {
        bSplineIKConstraint *data = (bSplineIKConstraint *)con->data;
        int segcount = 0;

        /* don't draw if only_temp, as Spline IK chains cannot be temporary */
        if (only_temp) {
          continue;
        }

        parchan = pchan;
        line_start = parchan->pose_tail;

        /* Find the chain's root */
        while (parchan->parent) {
          segcount++;
          /* FIXME: revise the breaking conditions */
          if (segcount == data->chainlen || segcount > 255) {
            break; /* 255 is weak */
          }
          parchan = parchan->parent;
        }
        /* Only draw line in case our chain is more than one bone long! */
        if (parchan != pchan) { /* XXX revise the breaking conditions to only stop at the tail? */
          line_end = parchan->pose_head;
          drw_shgroup_bone_ik_spline_lines(ctx, line_start, line_end);
        }
        break;
      }
    }
  }
}

static void draw_bone_relations(ArmatureDrawContext *ctx,
                                EditBone *ebone,
                                bPoseChannel *pchan,
                                bArmature *arm,
                                const int boneflag,
                                const short constflag)
{
  if (ebone && ebone->parent) {
    if (ctx->do_relations) {
      /* Always draw for unconnected bones, regardless of selection,
       * since riggers will want to know about the links between bones
       */
      if ((boneflag & BONE_CONNECTED) == 0) {
        drw_shgroup_bone_relationship_lines(ctx, ebone->head, ebone->parent->tail);
      }
    }
  }
  else if (pchan && pchan->parent) {
    if (ctx->do_relations) {
      /* Only draw if bone or its parent is selected - reduces viewport complexity with complex
       * rigs */
      if ((boneflag & BONE_SELECTED) ||
          (pchan->parent->bone && (pchan->parent->bone->flag & BONE_SELECTED))) {
        if ((boneflag & BONE_CONNECTED) == 0) {
          drw_shgroup_bone_relationship_lines(ctx, pchan->pose_head, pchan->parent->pose_tail);
        }
      }
    }

    /* Draw a line to IK root bone if bone is selected. */
    if (arm->flag & ARM_POSEMODE) {
      if (constflag & (PCHAN_HAS_IK | PCHAN_HAS_SPLINEIK)) {
        if (boneflag & BONE_SELECTED) {
          pchan_draw_ik_lines(ctx, pchan, !ctx->do_relations, constflag);
        }
      }
    }
  }
}

static void draw_bone_name(ArmatureDrawContext *ctx,
                           EditBone *eBone,
                           bPoseChannel *pchan,
                           bArmature *arm,
                           const int boneflag)
{
  struct DRWTextStore *dt = DRW_text_cache_ensure();
  uchar color[4];
  float vec[3];

  bool highlight = (pchan && (arm->flag & ARM_POSEMODE) && (boneflag & BONE_SELECTED)) ||
                   (eBone && (eBone->flag & BONE_SELECTED));

  UI_GetThemeColor4ubv(highlight ? TH_TEXT_HI : TH_TEXT, color);

  float *head = pchan ? pchan->pose_head : eBone->head;
  float *tail = pchan ? pchan->pose_tail : eBone->tail;
  mid_v3_v3v3(vec, head, tail);
  mul_m4_v3(ctx->ob->obmat, vec);

  DRW_text_cache_add(dt,
                     vec,
                     (pchan) ? pchan->name : eBone->name,
                     (pchan) ? strlen(pchan->name) : strlen(eBone->name),
                     10,
                     0,
                     DRW_TEXT_CACHE_GLOBALSPACE | DRW_TEXT_CACHE_STRING_PTR,
                     color);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Draw Loops
 * \{ */

static void draw_armature_edit(ArmatureDrawContext *ctx)
{
  Object *ob = ctx->ob;
  EditBone *eBone;
  int index;
  const bool is_select = DRW_state_is_select();
  const bool show_text = DRW_state_show_text();

  const Object *ob_orig = DEG_get_original_object(ob);
  /* FIXME(campbell): We should be able to use the CoW object,
   * however the active bone isn't updated. Long term solution is an 'EditArmature' struct.
   * for now we can draw from the original armature. See: T66773. */
  // bArmature *arm = ob->data;
  bArmature *arm = ob_orig->data;

  edbo_compute_bbone_child(arm);

  for (eBone = arm->edbo->first, index = ob_orig->runtime.select_id; eBone;
       eBone = eBone->next, index += 0x10000) {
    if (eBone->layer & arm->layer) {
      if ((eBone->flag & BONE_HIDDEN_A) == 0) {
        const int select_id = is_select ? index : (uint)-1;
        const short constflag = 0;

        /* catch exception for bone with hidden parent */
        int boneflag = eBone->flag;
        if ((eBone->parent) && !EBONE_VISIBLE(arm, eBone->parent)) {
          boneflag &= ~BONE_CONNECTED;
        }

        /* set temporary flag for drawing bone as active, but only if selected */
        if (eBone == arm->act_edbone) {
          boneflag |= BONE_DRAW_ACTIVE;
        }

        draw_bone_relations(ctx, eBone, NULL, arm, boneflag, constflag);

        if (arm->drawtype == ARM_ENVELOPE) {
          draw_bone_update_disp_matrix_default(eBone, NULL);
          draw_bone_envelope(ctx, eBone, NULL, arm, boneflag, constflag, select_id);
        }
        else if (arm->drawtype == ARM_LINE) {
          draw_bone_update_disp_matrix_default(eBone, NULL);
          draw_bone_line(ctx, eBone, NULL, arm, boneflag, constflag, select_id);
        }
        else if (arm->drawtype == ARM_WIRE) {
          draw_bone_update_disp_matrix_bbone(eBone, NULL);
          draw_bone_wire(ctx, eBone, NULL, arm, boneflag, constflag, select_id);
        }
        else if (arm->drawtype == ARM_B_BONE) {
          draw_bone_update_disp_matrix_bbone(eBone, NULL);
          draw_bone_box(ctx, eBone, NULL, arm, boneflag, constflag, select_id);
        }
        else {
          draw_bone_update_disp_matrix_default(eBone, NULL);
          draw_bone_octahedral(ctx, eBone, NULL, arm, boneflag, constflag, select_id);
        }

        if (show_text && (arm->flag & ARM_DRAWNAMES)) {
          draw_bone_name(ctx, eBone, NULL, arm, boneflag);
        }

        if (arm->flag & ARM_DRAWAXES) {
          draw_axes(ctx, eBone, NULL);
        }
      }
    }
  }
}

static void draw_armature_pose(ArmatureDrawContext *ctx)
{
  Object *ob = ctx->ob;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  const Scene *scene = draw_ctx->scene;
  bArmature *arm = ob->data;
  bPoseChannel *pchan;
  int index = -1;
  const bool show_text = DRW_state_show_text();

  /* We can't safely draw non-updated pose, might contain NULL bone pointers... */
  if (ob->pose->flag & POSE_RECALC) {
    return;
  }

  bool is_pose_select = false;
  /* Object can be edited in the scene. */
  if ((ob->base_flag & (BASE_FROM_SET | BASE_FROM_DUPLI)) == 0) {
    if ((draw_ctx->object_mode & OB_MODE_POSE) || (ob == draw_ctx->object_pose)) {
      arm->flag |= ARM_POSEMODE;
    }
    is_pose_select =
        /* If we're in pose-mode or object-mode with the ability to enter pose mode. */
        (
            /* Draw as if in pose mode (when selection is possible). */
            (arm->flag & ARM_POSEMODE) ||
            /* When we're in object mode, which may select bones. */
            ((ob->mode & OB_MODE_POSE) &&
             (
                 /* Switch from object mode when object lock is disabled. */
                 ((draw_ctx->object_mode == OB_MODE_OBJECT) &&
                  (scene->toolsettings->object_flag & SCE_OBJECT_MODE_LOCK) == 0) ||
                 /* Allow selection when in weight-paint mode
                  * (selection code ensures this wont become active). */
                 ((draw_ctx->object_mode == OB_MODE_WEIGHT_PAINT) &&
                  (draw_ctx->object_pose != NULL))))) &&
        DRW_state_is_select();

    if (is_pose_select) {
      const Object *ob_orig = DEG_get_original_object(ob);
      index = ob_orig->runtime.select_id;
    }
  }

  for (pchan = ob->pose->chanbase.first; pchan; pchan = pchan->next, index += 0x10000) {
    Bone *bone = pchan->bone;
    const bool bone_visible = (bone->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG)) == 0;

    if (bone_visible) {
      if (bone->layer & arm->layer) {
        const bool draw_dofs = !is_pose_select && ctx->show_relations &&
                               (arm->flag & ARM_POSEMODE) && (bone->flag & BONE_SELECTED) &&
                               ((ob->base_flag & BASE_FROM_DUPLI) == 0) &&
                               (pchan->ikflag & (BONE_IK_XLIMIT | BONE_IK_ZLIMIT));
        const int select_id = is_pose_select ? index : (uint)-1;
        const short constflag = pchan->constflag;

        pchan_draw_data_init(pchan);

        if (!ctx->const_color) {
          set_pchan_colorset(ctx, ob, pchan);
        }

        int boneflag = bone->flag;
        /* catch exception for bone with hidden parent */
        boneflag = bone->flag;
        if ((bone->parent) && (bone->parent->flag & (BONE_HIDDEN_P | BONE_HIDDEN_PG))) {
          boneflag &= ~BONE_CONNECTED;
        }

        /* set temporary flag for drawing bone as active, but only if selected */
        if (bone == arm->act_bone) {
          boneflag |= BONE_DRAW_ACTIVE;
        }

        draw_bone_relations(ctx, NULL, pchan, arm, boneflag, constflag);

        if ((pchan->custom) && !(arm->flag & ARM_NO_CUSTOM)) {
          draw_bone_update_disp_matrix_custom(pchan);
          draw_bone_custom_shape(ctx, NULL, pchan, arm, boneflag, constflag, select_id);
        }
        else if (arm->drawtype == ARM_ENVELOPE) {
          draw_bone_update_disp_matrix_default(NULL, pchan);
          draw_bone_envelope(ctx, NULL, pchan, arm, boneflag, constflag, select_id);
        }
        else if (arm->drawtype == ARM_LINE) {
          draw_bone_update_disp_matrix_default(NULL, pchan);
          draw_bone_line(ctx, NULL, pchan, arm, boneflag, constflag, select_id);
        }
        else if (arm->drawtype == ARM_WIRE) {
          draw_bone_update_disp_matrix_bbone(NULL, pchan);
          draw_bone_wire(ctx, NULL, pchan, arm, boneflag, constflag, select_id);
        }
        else if (arm->drawtype == ARM_B_BONE) {
          draw_bone_update_disp_matrix_bbone(NULL, pchan);
          draw_bone_box(ctx, NULL, pchan, arm, boneflag, constflag, select_id);
        }
        else {
          draw_bone_update_disp_matrix_default(NULL, pchan);
          draw_bone_octahedral(ctx, NULL, pchan, arm, boneflag, constflag, select_id);
        }

        if (draw_dofs) {
          draw_bone_degrees_of_freedom(ctx, pchan);
        }

        if (show_text && (arm->flag & ARM_DRAWNAMES)) {
          draw_bone_name(ctx, NULL, pchan, arm, boneflag);
        }

        if (arm->flag & ARM_DRAWAXES) {
          draw_axes(ctx, NULL, pchan);
        }
      }
    }
  }

  arm->flag &= ~ARM_POSEMODE;
}

static void armature_context_setup(ArmatureDrawContext *ctx,
                                   OVERLAY_PrivateData *pd,
                                   Object *ob,
                                   const bool do_envelope_dist,
                                   float *const_color)
{
  const bool is_xray = (ob->dtx & OB_DRAWXRAY) != 0;
  const bool is_filled = !pd->armature.transparent || do_envelope_dist;
  bArmature *arm = ob->data;
  OVERLAY_ArmatureCallBuffers *cb = &pd->armature_call_buffers[is_xray];

  switch (arm->drawtype) {
    case ARM_ENVELOPE:
      ctx->envelope_outline = cb->envelope_outline;
      ctx->envelope_solid = (is_filled) ? cb->envelope_solid : NULL;
      ctx->envelope_distance = (do_envelope_dist) ? cb->envelope_distance : NULL;
      break;
    case ARM_LINE:
      ctx->stick = cb->stick;
      break;
    case ARM_WIRE:
      ctx->wire = cb->wire;
      break;
    case ARM_B_BONE:
      ctx->outline = cb->box_outline;
      ctx->solid = (is_filled) ? cb->box_solid : NULL;
      break;
    case ARM_OCTA:
      ctx->outline = cb->octa_outline;
      ctx->solid = (is_filled) ? cb->octa_solid : NULL;
      break;
  }
  ctx->ob = ob;
  ctx->extras = &pd->extra_call_buffers[is_xray];
  ctx->dof_lines = cb->dof_lines;
  ctx->dof_sphere = cb->dof_sphere;
  ctx->point_solid = (is_filled) ? cb->point_solid : NULL;
  ctx->point_outline = cb->point_outline;
  ctx->custom_solid = (is_filled) ? cb->custom_solid : NULL;
  ctx->custom_outline = cb->custom_outline;
  ctx->custom_wire = cb->custom_solid; /* Use same shader. */
  ctx->transparent = pd->armature.transparent;
  ctx->show_relations = pd->armature.show_relations;
  ctx->const_color = const_color;
  ctx->const_wire = (((ob->base_flag & BASE_SELECTED) || (arm->drawtype == ARM_WIRE)) ?
                         1.5f :
                         ((ctx->transparent) ? 1.0f : 0.0f));

  /** See: 'set_pchan_color'*/
#define NO_ALPHA(c) (((c)[3] = 1.0f), (c))

  UI_GetThemeColor3fv(TH_SELECT, NO_ALPHA(ctx->color.select));
  UI_GetThemeColorShade3fv(TH_EDGE_SELECT, 60, NO_ALPHA(ctx->color.edge_select));
  UI_GetThemeColorShade3fv(TH_EDGE_SELECT, -20, NO_ALPHA(ctx->color.bone_select));
  UI_GetThemeColor3fv(TH_WIRE, NO_ALPHA(ctx->color.wire));
  UI_GetThemeColor3fv(TH_WIRE_EDIT, NO_ALPHA(ctx->color.wire_edit));
  UI_GetThemeColor3fv(TH_BONE_SOLID, NO_ALPHA(ctx->color.bone_solid));
  UI_GetThemeColorBlendShade3fv(
      TH_WIRE_EDIT, TH_EDGE_SELECT, 0.15f, 0, NO_ALPHA(ctx->color.bone_active_unselect));
  UI_GetThemeColor3fv(TH_BONE_POSE, NO_ALPHA(ctx->color.bone_pose));
  UI_GetThemeColor3fv(TH_BONE_POSE_ACTIVE, NO_ALPHA(ctx->color.bone_pose_active));
  UI_GetThemeColorBlendShade3fv(
      TH_WIRE, TH_BONE_POSE, 0.15f, 0, NO_ALPHA(ctx->color.bone_pose_active_unselect));
  UI_GetThemeColor3fv(TH_TEXT_HI, NO_ALPHA(ctx->color.text_hi));
  UI_GetThemeColor3fv(TH_TEXT, NO_ALPHA(ctx->color.text));
  UI_GetThemeColor3fv(TH_VERTEX_SELECT, NO_ALPHA(ctx->color.vertex_select));
  UI_GetThemeColor3fv(TH_VERTEX, NO_ALPHA(ctx->color.vertex));

#undef NO_ALPHA
}

void OVERLAY_edit_armature_cache_populate(OVERLAY_Data *vedata, Object *ob)
{
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  ArmatureDrawContext arm_ctx;
  armature_context_setup(&arm_ctx, pd, ob, true, NULL);
  draw_armature_edit(&arm_ctx);
}

void OVERLAY_pose_armature_cache_populate(OVERLAY_Data *vedata, Object *ob)
{
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  ArmatureDrawContext arm_ctx;
  armature_context_setup(&arm_ctx, pd, ob, true, NULL);
  draw_armature_pose(&arm_ctx);
}

void OVERLAY_armature_cache_populate(OVERLAY_Data *vedata, Object *ob)
{
  const DRWContextState *draw_ctx = DRW_context_state_get();
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  ArmatureDrawContext arm_ctx;
  float *color;
  DRW_object_wire_theme_get(ob, draw_ctx->view_layer, &color);
  armature_context_setup(&arm_ctx, pd, ob, false, color);
  draw_armature_pose(&arm_ctx);
}

void OVERLAY_armature_draw(OVERLAY_Data *vedata)
{
  OVERLAY_PassList *psl = vedata->psl;

  DRW_draw_pass(psl->armature_transp_ps);
  DRW_draw_pass(psl->armature_ps[0]);
  DRW_draw_pass(psl->armature_ps[1]);
}