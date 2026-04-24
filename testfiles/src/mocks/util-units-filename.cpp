// SPDX-License-Identifier: GPL-2.0-or-later

#include "util/units.h"

#include <cstdlib>
#include <string>

namespace Inkscape::Util {

std::string UnitTable::getUnitsFilename()
{
    if (auto const *datadir = std::getenv("INKSCAPE_DATADIR"); datadir && *datadir) {
        return std::string(datadir) + "/ui/units.xml";
    }
    return INKSCAPE_SHARE_DIR "/ui/units.xml";
}
 
} // namespace Inkscape::Util
