// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * File operations (independent of GUI)
 *
 * Copyright (C) 2018, 2019 Tavmjong Bah
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 *
 */

#include "io/file.h"

#include <iostream>
#include <memory>
#include <unistd.h>
#include <array>
#include <vector>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include <glibmm/spawn.h>
#include <giomm/file.h>

#include "document.h"
#include "document-undo.h"
#include "extension/system.h"     // Extension::open()
#include "extension/extension.h"
#include "extension/db.h"
#include "extension/output.h"
#include "extension/input.h"
#include "io/resource.h"
#include "object/sp-root.h"
#include "path-prefix.h"
#include "xml/repr.h"

namespace {

bool is_dxf_path(std::string const &path)
{
    auto const lowered = Glib::ustring(path).lowercase();
    return lowered.size() >= 4 && lowered.substr(lowered.size() - 4) == ".dxf";
}

std::string locate_dxf_input_script()
{
    using Inkscape::IO::Resource::EXTENSIONS;
    using Inkscape::IO::Resource::SYSTEM;

    std::vector<std::string> candidates;

    if (auto const from_resource = Inkscape::IO::Resource::get_filename(EXTENSIONS, "dxf_input.py", false, true); !from_resource.empty()) {
        candidates.push_back(from_resource);
    }

    if (auto const from_system_path = Inkscape::IO::Resource::get_path_string(SYSTEM, EXTENSIONS, "dxf_input.py"); !from_system_path.empty()) {
        candidates.push_back(from_system_path);
    }

    if (auto const *program_dir = get_program_dir(); program_dir && *program_dir) {
        candidates.emplace_back(Glib::build_filename(program_dir, "..", "..", "share", "extensions", "dxf_input.py"));
        candidates.emplace_back(Glib::build_filename(program_dir, "..", "share", "inkscape", "extensions", "dxf_input.py"));
    }

    for (auto const &candidate : candidates) {
        if (Glib::file_test(candidate, Glib::FileTest::IS_REGULAR)) {
            return Glib::canonicalize_filename(candidate);
        }
    }

    return {};
}

std::string find_python_for_extensions()
{
#ifdef _WIN32
    std::array<char const *, 4> candidates = {"python", "python3", "py", "C:\\msys64\\ucrt64\\bin\\python.exe"};
#else
    std::array<char const *, 2> candidates = {"python3", "python"};
#endif
    for (auto const *candidate : candidates) {
        if (auto const found = Glib::find_program_in_path(candidate); !found.empty()) {
            return found;
        }
    }
    return {};
}

std::unique_ptr<SPDocument> open_dxf_via_script_fallback(std::string const &path)
{
    auto const script = locate_dxf_input_script();
    auto const python = find_python_for_extensions();
    if (script.empty() || python.empty()) {
        return nullptr;
    }

    std::vector<std::string> argv = {
        python,
        script,
        path,
        "--encoding", "latin_1",
        "--scalemethod", "manual",
        "--scale", "1.0",
        "--xmin", "0.0",
        "--ymin", "0.0",
        "--textscale", "1.0",
        "--font", "Arial"
    };

    try {
        std::string svg_out;
        std::string stderr_out;
        int exit_status = 0;
        Glib::spawn_sync(
            Glib::path_get_dirname(script),
            argv,
            std::vector<std::string>{},
            Glib::SpawnFlags::SEARCH_PATH,
            sigc::slot<void()>(),
            &svg_out,
            &stderr_out,
            &exit_status);

        if (exit_status != 0 || svg_out.empty()) {
            if (!stderr_out.empty()) {
                std::cerr << "ink_file_open: DXF fallback import failed: " << stderr_out << std::endl;
            }
            return nullptr;
        }

        auto doc = ink_file_open(std::span<char const>(svg_out.data(), svg_out.size()));
        if (doc) {
            doc->changeFilenameAndHrefs(path.c_str());
        }
        return doc;
    } catch (Glib::SpawnError const &e) {
        std::cerr << "ink_file_open: failed to run DXF fallback importer: " << e.what() << std::endl;
    } catch (std::exception const &e) {
        std::cerr << "ink_file_open: DXF fallback importer raised: " << e.what() << std::endl;
    }

    return nullptr;
}

} // namespace

/**
 * Create a blank document, remove any template data.
 * Input: Empty string or template file name.
 */
std::unique_ptr<SPDocument> ink_file_new(std::string const &Template)
{
    auto doc = SPDocument::createNewDoc(Template.empty() ? nullptr : Template.c_str(), true);

    if (!doc) {
        std::cerr << "ink_file_new: Did not create new document!" << std::endl;
        return nullptr;
    }

    // Remove all the template info from xml tree
    Inkscape::XML::Node *myRoot = doc->getReprRoot();
    for (auto const name: {"inkscape:templateinfo",
                           "inkscape:_templateinfo"}) // backwards-compatibility
    {
        if (auto node = std::unique_ptr<Inkscape::XML::Node>{sp_repr_lookup_name(myRoot, name)}) {
            Inkscape::DocumentUndo::ScopedInsensitive no_undo(doc.get());
            sp_repr_unparent(node.get());
        }
    }

    return doc;
}

/**
 * Open a document from memory.
 */
std::unique_ptr<SPDocument> ink_file_open(std::span<char const> buffer)
{
    auto doc = SPDocument::createNewDocFromMem(buffer);

    if (!doc) {
        std::cerr << "ink_file_open: cannot open file in memory (pipe?)" << std::endl;
        return nullptr;
    }
    return doc;
}

/**
 * Open a document.
 */
std::pair<std::unique_ptr<SPDocument>, bool> ink_file_open(Glib::RefPtr<Gio::File> const &file)
{
    std::unique_ptr<SPDocument> doc;
    std::string path = file->get_path();

    if (is_dxf_path(path)) {
        doc = open_dxf_via_script_fallback(path);
        if (doc) return {std::move(doc), false};
    }

    // TODO: It's useless to catch these exceptions here (and below) unless we do something with them.
    //       If we can't properly handle them (e.g. by showing a user-visible message) don't catch them!
    try {
        doc = Inkscape::Extension::open(nullptr, path.c_str());
    } catch (Inkscape::Extension::Input::no_extension_found const &) {
    } catch (Inkscape::Extension::Input::open_failed const &) {
    } catch (Inkscape::Extension::Input::open_cancelled const &) {
        return {nullptr, true};
    }

    // Try to open explicitly as SVG.
    // TODO: Why is this necessary? Shouldn't this be handled by the first call already?
    if (!doc) {
        try {
            doc = Inkscape::Extension::open(Inkscape::Extension::db.get(SP_MODULE_KEY_INPUT_SVG), path.c_str());
        } catch (Inkscape::Extension::Input::no_extension_found const &) {
        } catch (Inkscape::Extension::Input::open_failed const &) {
        } catch (Inkscape::Extension::Input::open_cancelled const &) {
            return {nullptr, true};
        }
    }

    if (!doc) {
        std::cerr << "ink_file_open: '" << path << "' cannot be opened!" << std::endl;
        return {nullptr, false};
    }

    return {std::move(doc), false};
}

namespace Inkscape::IO {

/**
 * Create a temporary filename, which is closed and deleted when deconstructed.
 */
TempFilename::TempFilename(const std::string &pattern)
{
    try {
        _tempfd = Glib::file_open_tmp(_filename, pattern.c_str());
    } catch (...) {
        /// \todo Popup dialog here
    }
}
  
TempFilename::~TempFilename()
{
    close(_tempfd);
    unlink(_filename.c_str());
}

/**
 * Takes an absolute file path and returns a second file at the same
 * directory location, if and only if the filename exists and is a file.
 *
 * Returns the empty string if the new file is not found.
 */
std::string find_original_file(Glib::StdStringView const filepath, Glib::StdStringView const name)
{
    auto path = Glib::path_get_dirname(filepath);
    auto filename = Glib::build_filename(path, name);
    if (Glib::file_test(filename, Glib::FileTest::IS_REGULAR)) {
        return filename;
    }
    return ""; 
}

} // namespace Inkscape::IO

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
