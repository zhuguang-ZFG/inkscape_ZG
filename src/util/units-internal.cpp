// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Inkscape Units internal linking
 *
 * Copyright (C) 2026 AUTHORS
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "util/units.h"

#include <vector>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "io/resource.h"
#include "path-prefix.h"

namespace Inkscape::Util {

std::string UnitTable::getUnitsFilename()
{
    using namespace Inkscape::IO::Resource;

    if (auto filename = get_filename(UIS, "units.xml", false, true); !filename.empty()) {
        return filename;
    }

    std::vector<std::string> candidates;
    candidates.emplace_back(Glib::build_filename(get_inkscape_datadir(), "inkscape", "share", "ui", "units.xml"));
    candidates.emplace_back(Glib::build_filename(get_inkscape_datadir(), "share", "ui", "units.xml"));

    if (auto const *program_dir = get_program_dir(); program_dir && *program_dir) {
        candidates.emplace_back(Glib::build_filename(program_dir, "..", "..", "share", "ui", "units.xml"));
        candidates.emplace_back(Glib::build_filename(program_dir, "..", "share", "inkscape", "ui", "units.xml"));
    }

    for (auto const &candidate : candidates) {
        if (Glib::file_test(candidate, Glib::FileTest::IS_REGULAR)) {
            return Glib::canonicalize_filename(candidate);
        }
    }

    return {};
}

} // namespace Inkscape::Util

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
