// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared editor-generated G-code state helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_EDITOR_GENERATION_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_EDITOR_GENERATION_STATE_H

#include <glibmm/ustring.h>

namespace Inkscape::UI::Dialog {

struct EditorGcodeGenerationState
{
    bool valid = false;
    bool clip_to_machine_bed = false;
    bool swap_xy = false;
    bool invert_x = false;
    bool invert_y = false;
    bool flip_y_canvas = false;
    bool align_content_min_to_origin = false;
    double machine_bed_width_mm = 0.0;
    double machine_bed_depth_mm = 0.0;
};

struct EditorGcodeGenerationInputs
{
    bool clip_to_machine_bed = false;
    bool swap_xy = false;
    bool invert_x = false;
    bool invert_y = false;
    bool flip_y_canvas = false;
    bool align_content_min_to_origin = false;
    double machine_bed_width_mm = 0.0;
    double machine_bed_depth_mm = 0.0;
};

EditorGcodeGenerationState make_editor_gcode_generation_state(bool clip_to_machine_bed, bool swap_xy, bool invert_x,
                                                              bool invert_y, bool flip_y_canvas,
                                                              bool align_content_min_to_origin, double bed_width_mm,
                                                              double bed_depth_mm);
EditorGcodeGenerationInputs make_editor_gcode_generation_inputs(bool clip_to_machine_bed, bool swap_xy, bool invert_x,
                                                                bool invert_y, bool flip_y_canvas,
                                                                bool align_content_min_to_origin, double bed_width_mm,
                                                                double bed_depth_mm);
bool editor_gcode_generation_state_matches(EditorGcodeGenerationState const &state,
                                           EditorGcodeGenerationInputs const &inputs);
bool editor_gcode_generation_context_matches(EditorGcodeGenerationState const &state,
                                             EditorGcodeGenerationInputs const &inputs,
                                             bool document_matches);
Glib::ustring build_editor_gcode_stale_mapping_message();

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_EDITOR_GENERATION_STATE_H
