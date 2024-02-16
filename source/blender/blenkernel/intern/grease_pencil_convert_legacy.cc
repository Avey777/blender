/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.h"
#include "BKE_object.hh"

#include "BLI_color.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_gpencil_legacy_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_meshdata_types.h"

namespace blender::bke::greasepencil::convert {

/**
 * Find vertex groups that have assigned vertices in this drawing.
 * Returns:
 * - ListBase with used vertex group names (bDeformGroup)
 * - Array of indices in the new vertex group list for remapping
 */
static void find_used_vertex_groups(const bGPDframe &gpf,
                                    const ListBase &all_names,
                                    ListBase &r_vertex_group_names,
                                    Array<int> &r_indices)
{
  const int num_vertex_groups = BLI_listbase_count(&all_names);
  Array<int> is_group_used(num_vertex_groups, false);
  LISTBASE_FOREACH (bGPDstroke *, gps, &gpf.strokes) {
    if (!gps->dvert) {
      continue;
    }
    Span<MDeformVert> dverts = {gps->dvert, gps->totpoints};
    for (const MDeformVert &dvert : dverts) {
      for (const MDeformWeight &weight : Span<MDeformWeight>{dvert.dw, dvert.totweight}) {
        is_group_used[weight.def_nr] = true;
      }
    }
  }
  BLI_listbase_clear(&r_vertex_group_names);
  r_indices.reinitialize(num_vertex_groups);
  int new_group_i = 0;
  int old_group_i;
  LISTBASE_FOREACH_INDEX (const bDeformGroup *, def_group, &all_names, old_group_i) {
    if (!is_group_used[old_group_i]) {
      r_indices[old_group_i] = -1;
      continue;
    }
    r_indices[old_group_i] = new_group_i++;

    bDeformGroup *def_group_copy = static_cast<bDeformGroup *>(MEM_dupallocN(def_group));
    BLI_addtail(&r_vertex_group_names, def_group_copy);
  }
}

void legacy_gpencil_frame_to_grease_pencil_drawing(const bGPDframe &gpf,
                                                   const ListBase &vertex_group_names,
                                                   GreasePencilDrawing &r_drawing)
{
  /* Construct an empty CurvesGeometry in-place. */
  new (&r_drawing.geometry) CurvesGeometry();
  r_drawing.base.type = GP_DRAWING;
  r_drawing.runtime = MEM_new<bke::greasepencil::DrawingRuntime>(__func__);

  /* Get the number of points, number of strokes and the offsets for each stroke. */
  Vector<int> offsets;
  offsets.append(0);
  int num_strokes = 0;
  int num_points = 0;
  LISTBASE_FOREACH (bGPDstroke *, gps, &gpf.strokes) {
    num_points += gps->totpoints;
    offsets.append(num_points);
    num_strokes++;
  }

  /* Resize the CurvesGeometry. */
  Drawing &drawing = r_drawing.wrap();
  CurvesGeometry &curves = drawing.strokes_for_write();
  curves.resize(num_points, num_strokes);
  if (num_strokes > 0) {
    curves.offsets_for_write().copy_from(offsets);
  }
  OffsetIndices<int> points_by_curve = curves.points_by_curve();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  /* All strokes are poly curves. */
  curves.fill_curve_types(CURVE_TYPE_POLY);

  /* Find used vertex groups in this drawing. */
  ListBase stroke_vertex_group_names;
  Array<int> stroke_def_nr_map;
  find_used_vertex_groups(gpf, vertex_group_names, stroke_vertex_group_names, stroke_def_nr_map);
  BLI_assert(BLI_listbase_is_empty(&curves.vertex_group_names));
  curves.vertex_group_names = stroke_vertex_group_names;
  const bool use_dverts = !BLI_listbase_is_empty(&curves.vertex_group_names);

  /* Copy vertex weights and map the vertex group indices. */
  auto copy_dvert = [&](const MDeformVert &src_dvert, MDeformVert &dst_dvert) {
    dst_dvert = src_dvert;
    dst_dvert.dw = static_cast<MDeformWeight *>(MEM_dupallocN(src_dvert.dw));
    const MutableSpan<MDeformWeight> vertex_weights = {dst_dvert.dw, dst_dvert.totweight};
    for (MDeformWeight &weight : vertex_weights) {
      /* Map def_nr to the reduced vertex group list. */
      weight.def_nr = stroke_def_nr_map[weight.def_nr];
    }
  };

  /* Point Attributes. */
  MutableSpan<float3> positions = curves.positions_for_write();
  MutableSpan<float> radii = drawing.radii_for_write();
  MutableSpan<float> opacities = drawing.opacities_for_write();
  SpanAttributeWriter<float> delta_times = attributes.lookup_or_add_for_write_span<float>(
      "delta_time", AttrDomain::Point);
  SpanAttributeWriter<float> rotations = attributes.lookup_or_add_for_write_span<float>(
      "rotation", AttrDomain::Point);
  SpanAttributeWriter<ColorGeometry4f> vertex_colors =
      attributes.lookup_or_add_for_write_span<ColorGeometry4f>("vertex_color", AttrDomain::Point);
  SpanAttributeWriter<bool> selection = attributes.lookup_or_add_for_write_span<bool>(
      ".selection", AttrDomain::Point);
  MutableSpan<MDeformVert> dverts = use_dverts ? curves.wrap().deform_verts_for_write() :
                                                 MutableSpan<MDeformVert>();

  /* Curve Attributes. */
  SpanAttributeWriter<bool> stroke_cyclic = attributes.lookup_or_add_for_write_span<bool>(
      "cyclic", AttrDomain::Curve);
  /* TODO: This should be a `double` attribute. */
  SpanAttributeWriter<float> stroke_init_times = attributes.lookup_or_add_for_write_span<float>(
      "init_time", AttrDomain::Curve);
  SpanAttributeWriter<int8_t> stroke_start_caps = attributes.lookup_or_add_for_write_span<int8_t>(
      "start_cap", AttrDomain::Curve);
  SpanAttributeWriter<int8_t> stroke_end_caps = attributes.lookup_or_add_for_write_span<int8_t>(
      "end_cap", AttrDomain::Curve);
  SpanAttributeWriter<float> stroke_hardnesses = attributes.lookup_or_add_for_write_span<float>(
      "hardness", AttrDomain::Curve);
  SpanAttributeWriter<float> stroke_point_aspect_ratios =
      attributes.lookup_or_add_for_write_span<float>("aspect_ratio", AttrDomain::Curve);
  SpanAttributeWriter<float2> stroke_fill_translations =
      attributes.lookup_or_add_for_write_span<float2>("fill_translation", AttrDomain::Curve);
  SpanAttributeWriter<float> stroke_fill_rotations =
      attributes.lookup_or_add_for_write_span<float>("fill_rotation", AttrDomain::Curve);
  SpanAttributeWriter<float2> stroke_fill_scales = attributes.lookup_or_add_for_write_span<float2>(
      "fill_scale", AttrDomain::Curve);
  SpanAttributeWriter<ColorGeometry4f> stroke_fill_colors =
      attributes.lookup_or_add_for_write_span<ColorGeometry4f>("fill_color", AttrDomain::Curve);
  SpanAttributeWriter<int> stroke_materials = attributes.lookup_or_add_for_write_span<int>(
      "material_index", AttrDomain::Curve);

  int stroke_i = 0;
  LISTBASE_FOREACH_INDEX (bGPDstroke *, gps, &gpf.strokes, stroke_i) {
    /* TODO: check if `gps->editcurve` is not nullptr and parse bezier curve instead. */

    stroke_cyclic.span[stroke_i] = (gps->flag & GP_STROKE_CYCLIC) != 0;
    /* TODO: This should be a `double` attribute. */
    stroke_init_times.span[stroke_i] = float(gps->inittime);
    stroke_start_caps.span[stroke_i] = int8_t(gps->caps[0]);
    stroke_end_caps.span[stroke_i] = int8_t(gps->caps[1]);
    stroke_hardnesses.span[stroke_i] = gps->hardness;
    stroke_point_aspect_ratios.span[stroke_i] = gps->aspect_ratio[0] /
                                                max_ff(gps->aspect_ratio[1], 1e-8);
    stroke_fill_translations.span[stroke_i] = float2(gps->uv_translation);
    stroke_fill_rotations.span[stroke_i] = gps->uv_rotation;
    stroke_fill_scales.span[stroke_i] = float2(gps->uv_scale);
    stroke_fill_colors.span[stroke_i] = ColorGeometry4f(gps->vert_color_fill);
    stroke_materials.span[stroke_i] = gps->mat_nr;

    IndexRange points = points_by_curve[stroke_i];
    if (points.is_empty()) {
      continue;
    }

    /* Previously, Grease Pencil used a radius convention where 1 `px` = 0.001 units. This `px`
     * was the brush size which would be stored in the stroke thickness and then scaled by the
     * point pressure factor. Finally, the render engine would divide this thickness value by
     * 2000 (we're going from a thickness to a radius, hence the factor of two) to convert back
     * into blender units. Store the radius now directly in blender units. This makes it
     * consistent with how hair curves handle the radius. */
    const float stroke_thickness = float(gps->thickness) / 2000.0f;
    Span<bGPDspoint> src_points{gps->points, gps->totpoints};
    MutableSpan<float3> dst_positions = positions.slice(points);
    MutableSpan<float> dst_radii = radii.slice(points);
    MutableSpan<float> dst_opacities = opacities.slice(points);
    MutableSpan<float> dst_deltatimes = delta_times.span.slice(points);
    MutableSpan<float> dst_rotations = rotations.span.slice(points);
    MutableSpan<ColorGeometry4f> dst_vertex_colors = vertex_colors.span.slice(points);
    MutableSpan<bool> dst_selection = selection.span.slice(points);
    MutableSpan<MDeformVert> dst_dverts = use_dverts ? dverts.slice(points) :
                                                       MutableSpan<MDeformVert>();

    threading::parallel_for(src_points.index_range(), 4096, [&](const IndexRange range) {
      for (const int point_i : range) {
        const bGPDspoint &pt = src_points[point_i];
        dst_positions[point_i] = float3(pt.x, pt.y, pt.z);
        dst_radii[point_i] = stroke_thickness * pt.pressure;
        dst_opacities[point_i] = pt.strength;
        dst_rotations[point_i] = pt.uv_rot;
        dst_vertex_colors[point_i] = ColorGeometry4f(pt.vert_color);
        dst_selection[point_i] = (pt.flag & GP_SPOINT_SELECT) != 0;
        if (use_dverts && gps->dvert) {
          copy_dvert(gps->dvert[point_i], dst_dverts[point_i]);
        }
      }
    });

    dst_deltatimes.first() = 0;
    threading::parallel_for(
        src_points.index_range().drop_front(1), 4096, [&](const IndexRange range) {
          for (const int point_i : range) {
            const bGPDspoint &pt = src_points[point_i];
            const bGPDspoint &pt_prev = src_points[point_i - 1];
            dst_deltatimes[point_i] = pt.time - pt_prev.time;
          }
        });
  }

  delta_times.finish();
  rotations.finish();
  vertex_colors.finish();
  selection.finish();

  stroke_cyclic.finish();
  stroke_init_times.finish();
  stroke_start_caps.finish();
  stroke_end_caps.finish();
  stroke_hardnesses.finish();
  stroke_point_aspect_ratios.finish();
  stroke_fill_translations.finish();
  stroke_fill_rotations.finish();
  stroke_fill_scales.finish();
  stroke_fill_colors.finish();
  stroke_materials.finish();
}

void legacy_gpencil_to_grease_pencil(Main &bmain, GreasePencil &grease_pencil, bGPdata &gpd)
{
  using namespace blender::bke::greasepencil;

  int num_drawings = 0;
  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd.layers) {
    num_drawings += BLI_listbase_count(&gpl->frames);
  }

  grease_pencil.drawing_array_num = num_drawings;
  grease_pencil.drawing_array = reinterpret_cast<GreasePencilDrawingBase **>(
      MEM_cnew_array<GreasePencilDrawing *>(num_drawings, __func__));

  int i = 0, layer_idx = 0;
  LISTBASE_FOREACH_INDEX (bGPDlayer *, gpl, &gpd.layers, layer_idx) {
    /* Create a new layer. */
    Layer &new_layer = grease_pencil.add_layer(
        StringRefNull(gpl->info, BLI_strnlen(gpl->info, 128)));

    /* Flags. */
    new_layer.set_visible((gpl->flag & GP_LAYER_HIDE) == 0);
    new_layer.set_locked((gpl->flag & GP_LAYER_LOCKED) != 0);
    new_layer.set_selected((gpl->flag & GP_LAYER_SELECT) != 0);
    SET_FLAG_FROM_TEST(
        new_layer.base.flag, (gpl->flag & GP_LAYER_FRAMELOCK), GP_LAYER_TREE_NODE_MUTE);
    SET_FLAG_FROM_TEST(
        new_layer.base.flag, (gpl->flag & GP_LAYER_USE_LIGHTS), GP_LAYER_TREE_NODE_USE_LIGHTS);
    SET_FLAG_FROM_TEST(new_layer.base.flag,
                       (gpl->onion_flag & GP_LAYER_ONIONSKIN),
                       GP_LAYER_TREE_NODE_USE_ONION_SKINNING);

    new_layer.blend_mode = int8_t(gpl->blend_mode);

    new_layer.parent = gpl->parent;
    new_layer.set_parent_bone_name(gpl->parsubstr);

    copy_v3_v3(new_layer.translation, gpl->location);
    copy_v3_v3(new_layer.rotation, gpl->rotation);
    copy_v3_v3(new_layer.scale, gpl->scale);

    /* Convert the layer masks. */
    LISTBASE_FOREACH (bGPDlayer_Mask *, mask, &gpl->mask_layers) {
      LayerMask *new_mask = MEM_new<LayerMask>(mask->name);
      new_mask->flag = mask->flag;
      BLI_addtail(&new_layer.masks, new_mask);
    }
    new_layer.opacity = gpl->opacity;

    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      grease_pencil.drawing_array[i] = reinterpret_cast<GreasePencilDrawingBase *>(
          MEM_new<GreasePencilDrawing>(__func__));
      GreasePencilDrawing &drawing = *reinterpret_cast<GreasePencilDrawing *>(
          grease_pencil.drawing_array[i]);

      /* Convert the frame to a drawing. */
      legacy_gpencil_frame_to_grease_pencil_drawing(*gpf, gpd.vertex_group_names, drawing);

      /* Add the frame to the layer. */
      if (GreasePencilFrame *new_frame = new_layer.add_frame(gpf->framenum, i)) {
        new_frame->type = gpf->key_type;
        SET_FLAG_FROM_TEST(new_frame->flag, (gpf->flag & GP_FRAME_SELECT), GP_FRAME_SELECTED);
      }
      i++;
    }

    if ((gpl->flag & GP_LAYER_ACTIVE) != 0) {
      grease_pencil.set_active_layer(&new_layer);
    }

    /* TODO: Update drawing user counts. */
  }

  /* Copy vertex group names and settings. */
  BKE_defgroup_copy_list(&grease_pencil.vertex_group_names, &gpd.vertex_group_names);
  grease_pencil.vertex_group_active_index = gpd.vertex_group_active_index;

  /* Convert the onion skinning settings. */
  grease_pencil.onion_skinning_settings.opacity = gpd.onion_factor;
  grease_pencil.onion_skinning_settings.mode = gpd.onion_mode;
  if (gpd.onion_keytype == -1) {
    grease_pencil.onion_skinning_settings.filter = GREASE_PENCIL_ONION_SKINNING_FILTER_ALL;
  }
  else {
    grease_pencil.onion_skinning_settings.filter = (1 << gpd.onion_keytype);
  }
  grease_pencil.onion_skinning_settings.num_frames_before = gpd.gstep;
  grease_pencil.onion_skinning_settings.num_frames_after = gpd.gstep_next;
  copy_v3_v3(grease_pencil.onion_skinning_settings.color_before, gpd.gcolor_prev);
  copy_v3_v3(grease_pencil.onion_skinning_settings.color_after, gpd.gcolor_next);

  BKE_id_materials_copy(&bmain, &gpd.id, &grease_pencil.id);
}

void legacy_gpencil_object(Main &bmain, Object &object)
{
  bGPdata *gpd = static_cast<bGPdata *>(object.data);

  GreasePencil *new_grease_pencil = static_cast<GreasePencil *>(
      BKE_id_new(&bmain, ID_GP, gpd->id.name + 2));
  object.data = new_grease_pencil;
  object.type = OB_GREASE_PENCIL;

  /* NOTE: Could also use #BKE_id_free_us, to also free the legacy GP if not used anymore? */
  id_us_min(&gpd->id);
  /* No need to increase usercount of `new_grease_pencil`, since ID creation already set it
   * to 1. */

  legacy_gpencil_to_grease_pencil(bmain, *new_grease_pencil, *gpd);

  BKE_object_free_derived_caches(&object);
  BKE_object_free_modifiers(&object, 0);
}

}  // namespace blender::bke::greasepencil::convert
