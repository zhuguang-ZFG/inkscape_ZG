// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Enumerate likely serial device names for GRBL (OS-specific heuristics).
 */
#ifndef INK_AXIDRAW_SERIAL_PORT_SCAN_H
#define INK_AXIDRAW_SERIAL_PORT_SCAN_H

#include <string>
#include <vector>

namespace Inkscape::Axidraw {

/** Return sorted unique port paths (e.g. COM3, /dev/ttyUSB0). May be empty. */
std::vector<std::string> enumerate_serial_ports();

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_SERIAL_PORT_SCAN_H
