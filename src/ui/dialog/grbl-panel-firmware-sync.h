// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Internal GRBL panel firmware sync helpers extracted from the panel UI unit.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_H

#include <atomic>

namespace Inkscape::UI::Dialog {

class GrblControlPanel;

class GrblPanelFirmwareSync {
public:
    static void run(GrblControlPanel &panel, std::atomic<bool> const &stop);
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_FIRMWARE_SYNC_H
