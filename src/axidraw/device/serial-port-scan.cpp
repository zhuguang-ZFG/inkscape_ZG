// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Enumerate serial ports for GRBL device pickers.
 */

#include "serial-port-scan.h"

#include <cstring>
#include <filesystem>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#endif

namespace fs = std::filesystem;

namespace Inkscape::Axidraw {

#ifdef _WIN32
static void append_registry_serial_ports(std::set<std::string> &out)
{
    HKEY key = nullptr;
    // HARDWARE\DEVICEMAP\SERIALCOMM maps device objects to "COMn" names.
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, R"(HARDWARE\DEVICEMAP\SERIALCOMM)", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return;
    }

    for (DWORD index = 0;; ++index) {
        char value_name[256];
        DWORD name_len = sizeof(value_name);
        BYTE data[256];
        DWORD data_len = sizeof(data);
        DWORD value_type = 0;
        LONG const rc = RegEnumValueA(key, index, value_name, &name_len, nullptr, &value_type, data, &data_len);
        if (rc == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (rc != ERROR_SUCCESS || value_type != REG_SZ || data_len == 0) {
            continue;
        }
        // REG_SZ may or may not include trailing null in data_len.
        auto const *text = reinterpret_cast<char *>(data);
        auto const *end = static_cast<char const *>(std::memchr(text, '\0', data_len));
        std::size_t const n = end ? static_cast<std::size_t>(end - text) : data_len;
        if (n > 0) {
            out.emplace(text, n);
        }
    }
    RegCloseKey(key);
}
#endif

std::vector<std::string> enumerate_serial_ports()
{
    std::set<std::string> ordered;

#ifdef _WIN32
    append_registry_serial_ports(ordered);
#else
    try {
        if (!fs::exists("/dev") || !fs::is_directory("/dev")) {
            return {};
        }
        static char const *const prefixes[] = {"ttyUSB", "ttyACM", "ttyAMA", "cu.", "rfcomm"};
        for (auto const &entry : fs::directory_iterator("/dev")) {
            if (!entry.is_character_file() && !entry.is_symlink()) {
                continue;
            }
            std::string const name = entry.path().filename().string();
            for (auto const *pre : prefixes) {
                if (name.find(pre) == 0) {
                    ordered.insert(entry.path().string());
                    break;
                }
            }
        }
    } catch (...) {
        return {};
    }
#endif

    return {ordered.begin(), ordered.end()};
}

} // namespace Inkscape::Axidraw

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
