// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-editor-generation-state.h"

#include "ui/dialog/grbl-editor-gcode.h"

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {

EditorGcodeGenerationState make_editor_gcode_generation_state(bool const clip_to_machine_bed, bool const swap_xy,
                                                              bool const invert_x, bool const invert_y,
                                                              bool const flip_y_canvas,
                                                              bool const align_content_min_to_origin,
                                                              double const bed_width_mm,
                                                              double const bed_depth_mm)
{
    EditorGcodeGenerationState state;
    state.valid = true;
    state.clip_to_machine_bed = clip_to_machine_bed;
    state.swap_xy = swap_xy;
    state.invert_x = invert_x;
    state.invert_y = invert_y;
    state.flip_y_canvas = flip_y_canvas;
    state.align_content_min_to_origin = align_content_min_to_origin;
    state.machine_bed_width_mm = bed_width_mm;
    state.machine_bed_depth_mm = bed_depth_mm;
    return state;
}

EditorGcodeGenerationInputs make_editor_gcode_generation_inputs(bool const clip_to_machine_bed, bool const swap_xy,
                                                                bool const invert_x, bool const invert_y,
                                                                bool const flip_y_canvas,
                                                                bool const align_content_min_to_origin,
                                                                double const bed_width_mm,
                                                                double const bed_depth_mm)
{
    EditorGcodeGenerationInputs inputs;
    inputs.clip_to_machine_bed = clip_to_machine_bed;
    inputs.swap_xy = swap_xy;
    inputs.invert_x = invert_x;
    inputs.invert_y = invert_y;
    inputs.flip_y_canvas = flip_y_canvas;
    inputs.align_content_min_to_origin = align_content_min_to_origin;
    inputs.machine_bed_width_mm = bed_width_mm;
    inputs.machine_bed_depth_mm = bed_depth_mm;
    return inputs;
}

bool editor_gcode_generation_state_matches(EditorGcodeGenerationState const &state,
                                           EditorGcodeGenerationInputs const &inputs)
{
    if (!state.valid) {
        return false;
    }

    return state.clip_to_machine_bed == inputs.clip_to_machine_bed &&
           state.swap_xy == inputs.swap_xy &&
           state.invert_x == inputs.invert_x &&
           state.invert_y == inputs.invert_y &&
           state.flip_y_canvas == inputs.flip_y_canvas &&
           state.align_content_min_to_origin == inputs.align_content_min_to_origin &&
           nearly_equal_mm(state.machine_bed_width_mm, inputs.machine_bed_width_mm) &&
           nearly_equal_mm(state.machine_bed_depth_mm, inputs.machine_bed_depth_mm);
}

bool editor_gcode_generation_context_matches(EditorGcodeGenerationState const &state,
                                             EditorGcodeGenerationInputs const &inputs,
                                             bool const document_matches)
{
    return document_matches && editor_gcode_generation_state_matches(state, inputs);
}

Glib::ustring build_editor_gcode_stale_mapping_message()
{
    return _("编辑器中的 G-code 是按之前的床面/映射设置生成的，和当前设置不一致。\n\n"
             "请先重新点击“从图稿填充”，再发送到机器。");
}

} // namespace Inkscape::UI::Dialog
