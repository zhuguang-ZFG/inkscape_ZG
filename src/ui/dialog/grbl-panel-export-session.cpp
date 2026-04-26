// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-export-session.h"

namespace Inkscape::UI::Dialog {

Inkscape::Axidraw::GrblExportContext make_grbl_panel_export_context(GrblPanelExportSources const &sources)
{
    Inkscape::Axidraw::GrblExportContext context;
    context.desktop = sources.desktop;
    context.selection = sources.selection;
    context.use_current_layer_without_selection = sources.use_current_layer_without_selection;
    context.cancel = sources.cancel;
    return context;
}

GrblPanelExportSession make_grbl_panel_export_session(Inkscape::Axidraw::GrblExportParams const &params,
                                                      GrblPanelExportSources const &sources)
{
    GrblPanelExportSession session;
    session.params = params;
    session.context = make_grbl_panel_export_context(sources);
    return session;
}

} // namespace Inkscape::UI::Dialog
