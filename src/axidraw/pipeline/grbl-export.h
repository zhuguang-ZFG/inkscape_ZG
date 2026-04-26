// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Export visible shape geometry as GRBL G-code over an open serial port, or build the same
 * program into a string for inspection before sending (generate → review → send workflow).
 *
 * **Firmware (device behaviour):** Align pen speeds, Z/M3/M5 semantics, and streaming `ok` protocol
 * with the Grbl_Esp32 tree shipped alongside `inkscape-axidraw` (e.g. `Grbl_Esp32/src/Config.h`,
 * `Machines/custom_3axis_hr4988.h`, `Custom/paixi_writer_tool_change.cpp`, `Spindles/NullSpindle.cpp`).
 * Maintainer-recorded local clone path: `src/axidraw/FIRMWARE.md`.
 * Defaults here match typical pen-lift Z=0 / Z=5 style comments in that firmware unless the user
 * overrides preferences.
 *
 * **Host UX reference (not wire protocol):** Quasi-engraving style “vector → preview → machine”
 * flows in proprietary hosts are reflected at a high level by decompiled Java under
 * `kxnx/.../decompiled_java/com/kvenjoy/drawsoft/lib/` (e.g. `remote/pck/PrintPck.java` for a
 * length-prefixed single G-code payload when UUID is empty, `grbl/GrblParameters.java` for GRBL-side
 * timing fields). Inkscape uses a **local serial** path and plain line-based G-code only; do not
 * assume packet parity with that Java.
 *
 * **Pen control mode** (`/options/grbl/pen-control`, default `z`):
 * - `z` — use preferences `pen-up-cmd` / `pen-down-cmd` (defaults `G1 Z0 F3000` / `G1 Z5 F3000`).
 * - `m3m5` — fixed `M5` (up) and `M3 S1000` (down), matching `PAIXI_PEN_M3_M5_CONTROL` in
 *   `Spindles/NullSpindle.cpp` (M3 → pen-down Z, M5 → pen-up Z) without sending raw Z lines.
 */
#ifndef INK_AXIDRAW_GRBL_EXPORT_H
#define INK_AXIDRAW_GRBL_EXPORT_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <glibmm/ustring.h>
#include <string>

#include <2geom/pathvector.h>

class SPDocument;
class SPDesktop;

namespace Inkscape {
class Preferences;
class Selection;
} // namespace Inkscape

namespace Inkscape::Axidraw {

enum class SparseSamplingStrategy {
    Legacy,
    Directional,
};

class SerialPort;

struct GrblExportParams {
    double flatness = 0.08;
    double feed_draw_mm_min = 1200.0;
    double feed_travel_mm_min = 6000.0;
    Glib::ustring pen_up_cmd = "G1 Z0 F3000";
    Glib::ustring pen_down_cmd = "G1 Z5 F3000";
    /// Optional dwell after pen-up / pen-down commands to let the mechanism settle.
    double pen_up_delay_ms = 0.0;
    double pen_down_delay_ms = 0.0;
    /// Extend the start of open strokes so pen-down transitions happen outside the intended geometry.
    bool enable_path_lead_in = false;
    double lead_in_distance_mm = 0.0;
    /// Extend the end of open strokes so pen-up transitions happen outside the intended geometry.
    bool enable_path_lead_out = false;
    double lead_out_distance_mm = 0.0;
    /// Optional higher pen-up move used before long travel moves.
    bool enable_long_pen_up = false;
    double long_pen_up_mm = 10.0;
    double long_move_distance_mm = 20.0;
    /// Join consecutive strokes with a drawn bridge when the gap between them is very small.
    bool enable_near_connect = false;
    double near_connect_distance_mm = 0.3;
    /// Keep one stroke out of each group of @a sparse_keep_every to thin dense hatch / line fields.
    bool enable_sparse_stroke_sampling = false;
    int sparse_keep_every = 1;
    SparseSamplingStrategy sparse_sampling_strategy = SparseSamplingStrategy::Legacy;
    /// Greedy nearest-neighbor ordering of whole polylines to shorten G0 travel (after collect, before send).
    bool optimize_stroke_order = true;
    /// When stroke-order optimization runs, allow reversing each polyline so travel starts from the closer endpoint.
    bool optimize_stroke_direction = true;

    /// Convert closed contours into hatch scanlines before emit (engraving-style "contour to line fill").
    bool contour_to_hatch = false;
    /// Hatch scanline spacing in machine millimetres (used when @a contour_to_hatch is true).
    double hatch_spacing_mm = 1.0;

    /// After converting to millimetres: mirror Y using <tt>page_height_mm − y</tt> (SVG Y-down → common machine Y-up).
    bool flip_y_canvas = false;
    /// Swap X/Y after document-to-machine conversion, useful when the machine axes are mounted rotated 90°.
    bool swap_xy = false;
    /// Invert the exported X coordinate after document-to-machine conversion.
    bool invert_x = false;
    /// Invert the exported Y coordinate after document-to-machine conversion.
    bool invert_y = false;
    /// Subtract the minimum X/Y of all points so the lower-left of the plotted bounds becomes (0, 0) in machine mm.
    bool align_content_min_to_origin = false;
    /// Clip segments to the axis-aligned rectangle [0, machine_bed_width_mm] × [0, machine_bed_depth_mm].
    bool clip_to_machine_bed = false;
    double machine_bed_width_mm = 300.0;
    double machine_bed_depth_mm = 200.0;

    /// After the first non-empty layer, insert a pause (see inkscape-axidraw `auto_pause_between_layers`).
    bool auto_pause_between_layers = false;
    /// On a layer pause: pen up, optional return to work XY zero, optional confirm, then rapid back (see
    /// `run_pen_change_flow` in inkscape-axidraw `axidraw.py`).
    bool manual_pen_change = false;
    bool pen_change_to_home = true;
    bool pen_change_prompt = true;
    /// When a layer label contains `Tn`, emit `Tn M6` between layers if the tool changes.
    bool enable_layer_tool_change_m6 = false;
    /// Before `Tn M6`, optionally move to a dedicated pen-change point.
    bool tool_change_use_point = false;
    double tool_change_x_mm = 0.0;
    double tool_change_y_mm = 0.0;
    /// If set (>0) and manual pen change is off, insert <tt>G4 P…</tt> dwell (seconds) between layers instead.
    double auto_layer_pause_dwell_sec = 0.0;
    /// Optional custom commands inserted after G21/G90 and before the first stroke.
    Glib::ustring start_gcode;
    /// Optional custom commands inserted after the final pen-up move.
    Glib::ustring end_gcode;
};

/**
 * Non-null @a selection with at least one item: export only those trees.
 * When @a selection is null or empty: if @a use_current_layer_without_selection and @a desktop
 * are set, export the current layer subtree only; otherwise the whole document.
 * @a cancel, when set, may be flagged from the UI thread to stop between G-code lines.
 */
struct GrblExportContext {
    SPDesktop *desktop = nullptr;
    Inkscape::Selection *selection = nullptr;
    bool use_current_layer_without_selection = false;
    std::atomic<bool> const *cancel = nullptr;
    /// When manual pen change triggers between layers: return false to abort (user declined). Called on the UI thread.
    std::function<bool(double resume_x_mm, double resume_y_mm)> on_manual_pen_change_between_layers;
    /// When building a text preview, skip interactive prompts and emit comment lines instead.
    bool inhibit_interactive_pen_changes = false;
    /**
     * Optional: invoked after each drawable stroke is fully emitted (same thread as serial I/O;
     * `grbl_send_line` pumps the default GLib main context between lines, so GTK may run).
     * @a stroke_done is 1-based count of completed strokes; @a stroke_total counts only strokes with at least two points.
     */
    std::function<void(std::size_t stroke_done, std::size_t stroke_total)> on_plot_stroke_progress;
    /**
     * Optional: invoked after each G-code line is accepted by the controller over serial (not used
     * when building into a string). @a lines_sent is 1-based; @a lines_estimated is a pre-flight
     * count matching the same emit path (headers, pen moves, draws, layer pauses).
     */
    std::function<void(std::size_t lines_sent, std::size_t lines_estimated)> on_plot_gcode_line_progress;
    /// Set by `export_paths_to_grbl` before streaming; incremented from `emit_line_impl` on serial only.
    mutable std::size_t plot_gcode_lines_sent = 0;
    mutable std::size_t plot_gcode_lines_estimate = 0;
};

/** Populated by export_paths_to_grbl / build_grbl_plot_gcode_string when @a stats_out is non-null (machine mm). */
struct GrblPlotStats {
    std::size_t stroke_count = 0;
    std::size_t layer_count = 0;
    std::size_t tool_change_count = 0;
    bool has_bounds_mm = false;
    double min_x_mm = 0;
    double min_y_mm = 0;
    double max_x_mm = 0;
    double max_y_mm = 0;
    bool has_length_stats = false;
    double draw_length_mm = 0;
    double travel_length_mm = 0;
    bool has_travel_optimization_stats = false;
    double travel_length_before_optimization_mm = 0;
    double travel_length_saved_by_optimization_mm = 0;
    double estimated_duration_sec = 0;
};

/**
 * Send G21/G90, then pen-up rapids and pen-down draws for visible SPShape paths.
 * @return false and sets @a err_out on I/O or protocol failure.
 */
bool export_paths_to_grbl(SerialPort &port, SPDocument *doc, GrblExportParams const &params,
                          GrblExportContext const &ctx, std::string &err_out,
                          std::size_t *stroke_count_out = nullptr, GrblPlotStats *stats_out = nullptr);

/** Fill @a params from `/options/grbl/...` keys (feeds, pen mode, flatness, stroke order, bed clip, etc.). */
void grbl_export_params_from_preferences(Inkscape::Preferences *prefs, GrblExportParams &params);

/** Prepare geometry and fill @a stats_out without generating or streaming G-code. */
bool analyze_grbl_plot(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                       GrblPlotStats &stats_out, std::string &err_out);

/**
 * Build the same G-code program that export_paths_to_grbl would stream (including G21/G90),
 * without opening serial. Intended for the GRBL control panel “review then send” path.
 * @param max_output_bytes caps @a gcode_out growth (default 32 MiB).
 * @param stats_out optional bounding box and stroke count for UI feedback (same geometry pass as @a gcode_out).
 */
bool build_grbl_plot_gcode_string(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                                  std::string &gcode_out, std::string &err_out,
                                  std::size_t *stroke_count_out = nullptr,
                                  std::size_t max_output_bytes = 32u * 1024u * 1024u,
                                  GrblPlotStats *stats_out = nullptr);

/**
 * Polylines in document space (same sampling and stroke order as export, before mm conversion, Y mirror,
 * origin shift, and machine-bed clipping). Intended for a non-destructive canvas overlay.
 * @param max_strokes caps the number of polylines converted to paths (large drawings).
 * @param strokes_included_out / strokes_total_out optional counts when preview is truncated.
 */
bool build_grbl_plot_preview_pathvector(SPDocument *doc, GrblExportParams const &params, GrblExportContext const &ctx,
                                        Geom::PathVector &paths_out, std::string &err_out,
                                        std::size_t max_strokes = 12000,
                                        std::size_t *strokes_included_out = nullptr,
                                        std::size_t *strokes_total_out = nullptr);

/**
 * Same polylines as streamed G-code (after mm, optional page Y mirror, origin shift, and bed clipping), mapped back
 * into document units for canvas overlay. When @a approximate_due_to_clip_out is non-null, it is set if bed clipping
 * ran (removed geometry cannot be reconstructed on the canvas).
 */
bool build_grbl_plot_machine_preview_pathvector_in_doc_space(SPDocument *doc, GrblExportParams const &params,
                                                             GrblExportContext const &ctx, Geom::PathVector &paths_out,
                                                             std::string &err_out,
                                                             bool *approximate_due_to_clip_out = nullptr,
                                                             std::size_t max_strokes = 12000,
                                                             std::size_t *strokes_included_out = nullptr,
                                                             std::size_t *strokes_total_out = nullptr);

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_GRBL_EXPORT_H
