// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Shared export session helpers for the GRBL control panel.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_EXPORT_SESSION_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_EXPORT_SESSION_H

#include <atomic>

#include "axidraw/pipeline/grbl-export.h"

namespace Inkscape {
class Selection;
}

class SPDesktop;

namespace Inkscape::UI::Dialog {

struct GrblPanelExportSources
{
    SPDesktop *desktop = nullptr;
    Inkscape::Selection *selection = nullptr;
    bool use_current_layer_without_selection = false;
    std::atomic<bool> const *cancel = nullptr;
};

struct GrblPanelExportSession
{
    Inkscape::Axidraw::GrblExportParams params;
    Inkscape::Axidraw::GrblExportContext context;
};

Inkscape::Axidraw::GrblExportContext make_grbl_panel_export_context(GrblPanelExportSources const &sources);
GrblPanelExportSession make_grbl_panel_export_session(Inkscape::Axidraw::GrblExportParams const &params,
                                                      GrblPanelExportSources const &sources);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_EXPORT_SESSION_H
