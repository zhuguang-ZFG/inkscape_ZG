# GRBL UI Link Boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce `GrblControlPanel` device coupling by moving serial/TCP link ownership and line I/O dispatch behind a small GRBL link boundary.

**Architecture:** Add a focused `GrblLink` unit under `src/axidraw/device/` that owns either a `SerialPort` or `TcpPort` and exposes the operations the UI already uses: `is_open`, `write_bytes`, `read_line`, `send_line_wait_ok`, `purge_io`, and `close`. Keep direct document streaming serial-only in this phase by exposing a narrow `serial_port()` accessor for the existing `export_paths_to_grbl(SerialPort&, ...)` path. This is an extraction, not a behavior change.

**Tech Stack:** C++20, existing `SerialPort`, `TcpPort`, `grbl_send_line`, CMake unit tests, GoogleTest.

**Constraints:**
- Do not change GRBL protocol semantics in this phase.
- Do not enable TCP direct document streaming in this phase.
- Do not commit unless the user explicitly requests a git commit.
- Preserve the existing `_port_mutex` synchronization behavior in `GrblControlPanel`.

---

## File Structure

- Create `src/axidraw/device/grbl-link.h`: public link boundary used by UI code.
- Create `src/axidraw/device/grbl-link.cpp`: production implementation that dispatches to `SerialPort`, `TcpPort`, and `grbl_send_line`.
- Modify `src/axidraw/CMakeLists.txt`: add the new source file to the axidraw source list.
- Modify `src/ui/dialog/grbl-control-panel.h`: replace `_port` / `_tcp_port` with `_link`, remove private `link_*` helpers that move into `GrblLink`.
- Modify `src/ui/dialog/grbl-control-panel.cpp`: use `_link` for connection state, status polling, G-code editor sending, firmware sync, disconnect, and legacy serial direct streaming.
- Create or extend `testfiles/src/grbl-link-test.cpp`: unit test the link state and dispatch behavior using fake endpoints if the production class is templated for tests; otherwise test the non-hardware helper logic and rely on build-level coverage for concrete ports.
- Modify `testfiles/CMakeLists.txt`: register `grbl-link-test` and link `ws2_32` on Windows if it includes `tcp-port.cpp`.

## Task 1: Add The Link Boundary

**Files:**
- Create: `src/axidraw/device/grbl-link.h`
- Create: `src/axidraw/device/grbl-link.cpp`
- Modify: `src/axidraw/CMakeLists.txt`

- [ ] **Step 1: Write the new header**

Create `src/axidraw/device/grbl-link.h` with this shape:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INK_AXIDRAW_GRBL_LINK_H
#define INK_AXIDRAW_GRBL_LINK_H

#include <cstddef>
#include <memory>
#include <string>

namespace Inkscape::Axidraw {

class SerialPort;
class TcpPort;

class GrblLink {
public:
    enum class Kind { none, serial, tcp };

    GrblLink();
    ~GrblLink();

    GrblLink(GrblLink const &) = delete;
    GrblLink &operator=(GrblLink const &) = delete;
    GrblLink(GrblLink &&) noexcept;
    GrblLink &operator=(GrblLink &&) noexcept;

    void set_serial(std::unique_ptr<SerialPort> port);
    void set_tcp(std::unique_ptr<TcpPort> port);

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] SerialPort *serial_port() const noexcept;

    bool write_bytes(void const *data, std::size_t len);
    bool read_line(std::string &out, int timeout_ms);
    bool send_line_wait_ok(std::string const &line, std::string &err_out);
    void purge_io();
    void close();

private:
    std::unique_ptr<SerialPort> _serial;
    std::unique_ptr<TcpPort> _tcp;
};

} // namespace Inkscape::Axidraw

#endif // INK_AXIDRAW_GRBL_LINK_H
```

- [ ] **Step 2: Implement exact behavior currently embedded in the panel**

Create `src/axidraw/device/grbl-link.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-link.h"

#include "grbl-client.h"
#include "serial-port.h"
#include "tcp-port.h"

namespace Inkscape::Axidraw {

GrblLink::GrblLink() = default;
GrblLink::~GrblLink() = default;
GrblLink::GrblLink(GrblLink &&) noexcept = default;
GrblLink &GrblLink::operator=(GrblLink &&) noexcept = default;

void GrblLink::set_serial(std::unique_ptr<SerialPort> port)
{
    close();
    _serial = std::move(port);
}

void GrblLink::set_tcp(std::unique_ptr<TcpPort> port)
{
    close();
    _tcp = std::move(port);
}

GrblLink::Kind GrblLink::kind() const noexcept
{
    if (_serial) {
        return Kind::serial;
    }
    if (_tcp) {
        return Kind::tcp;
    }
    return Kind::none;
}

bool GrblLink::is_open() const
{
    return (_serial && _serial->is_open()) || (_tcp && _tcp->is_open());
}

SerialPort *GrblLink::serial_port() const noexcept
{
    return _serial.get();
}

bool GrblLink::write_bytes(void const *data, std::size_t len)
{
    if (_serial && _serial->is_open()) {
        return _serial->write_bytes(data, len);
    }
    if (_tcp && _tcp->is_open()) {
        return _tcp->write_bytes(data, len);
    }
    return false;
}

bool GrblLink::read_line(std::string &out, int timeout_ms)
{
    if (_serial && _serial->is_open()) {
        return _serial->read_line(out, timeout_ms);
    }
    if (_tcp && _tcp->is_open()) {
        return _tcp->read_line(out, timeout_ms);
    }
    return false;
}

bool GrblLink::send_line_wait_ok(std::string const &line, std::string &err_out)
{
    if (_serial && _serial->is_open()) {
        return grbl_send_line(*_serial, line, err_out);
    }
    if (_tcp && _tcp->is_open()) {
        return grbl_send_line(*_tcp, line, err_out);
    }
    err_out = "not connected";
    return false;
}

void GrblLink::purge_io()
{
    if (_serial && _serial->is_open()) {
        _serial->purge_io();
    } else if (_tcp && _tcp->is_open()) {
        _tcp->purge_io();
    }
}

void GrblLink::close()
{
    if (_serial) {
        _serial->close();
        _serial.reset();
    }
    if (_tcp) {
        _tcp->close();
        _tcp.reset();
    }
}

} // namespace Inkscape::Axidraw
```

- [ ] **Step 3: Add the source to the build**

In `src/axidraw/CMakeLists.txt`, add:

```cmake
device/grbl-link.cpp
```

next to `device/grbl-client.cpp`, `device/serial-port.cpp`, and `device/tcp-port.cpp`.

- [ ] **Step 4: Build to catch integration errors**

Run:

```powershell
$env:MINGW_CHOST='ucrt64'; $env:MINGW_PREFIX='C:/msys64/ucrt64'; $env:MSYSTEM='UCRT64'; $env:MINGW_PACKAGE_PREFIX='mingw-w64-ucrt-x86_64'; $env:Path='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + $env:Path; cmake --build build-zg --target test_grbl-export
```

Expected: target builds successfully. This does not prove panel behavior yet; it only verifies the new device source compiles in the current target graph.

## Task 2: Move Panel Link Ownership To `GrblLink`

**Files:**
- Modify: `src/ui/dialog/grbl-control-panel.h`
- Modify: `src/ui/dialog/grbl-control-panel.cpp`

- [ ] **Step 1: Update the panel header**

In `grbl-control-panel.h`:

Add a forward declaration:

```cpp
class GrblLink;
```

under `namespace Inkscape::Axidraw`.

Replace:

```cpp
std::unique_ptr<Inkscape::Axidraw::SerialPort> _port;
std::unique_ptr<Inkscape::Axidraw::TcpPort> _tcp_port;
```

with:

```cpp
std::unique_ptr<Inkscape::Axidraw::GrblLink> _link;
```

Remove these private methods from the panel declaration:

```cpp
bool link_is_open() const;
bool link_write_bytes(void const *data, size_t len);
bool link_read_line(std::string &out, int timeout_ms);
bool link_write_line(std::string const &line, std::string &err_out);
void link_purge_io();
void link_close();
```

- [ ] **Step 2: Construct the link in the panel constructor**

In `grbl-control-panel.cpp`, include:

```cpp
#include "axidraw/device/grbl-link.h"
```

In the constructor initializer or constructor body, initialize:

```cpp
_link = std::make_unique<Inkscape::Axidraw::GrblLink>();
```

- [ ] **Step 3: Replace helper calls with `_link` calls**

Replace panel helper calls as follows:

```cpp
link_is_open()        -> (_link && _link->is_open())
link_write_bytes(...) -> _link->write_bytes(...)
link_read_line(...)   -> _link->read_line(...)
link_write_line(...)  -> _link->send_line_wait_ok(...)
link_purge_io()       -> _link->purge_io()
link_close()          -> _link->close()
```

Preserve every existing `_port_mutex` lock.

- [ ] **Step 4: Replace connection setup**

Where connection code currently assigns `_port` after opening a serial port, wrap it:

```cpp
auto port = std::make_unique<Inkscape::Axidraw::SerialPort>();
if (!port->open(device.raw(), baud)) {
    // keep existing failure handling
}
_link->set_serial(std::move(port));
```

Where TCP code currently assigns `_tcp_port`, wrap it:

```cpp
auto port = std::make_unique<Inkscape::Axidraw::TcpPort>();
if (!port->connect(host.raw(), port_number)) {
    // keep existing failure handling
}
_link->set_tcp(std::move(port));
```

- [ ] **Step 5: Preserve direct document streaming as serial-only**

In `on_send_document_direct`, replace:

```cpp
if (!_port || !_port->is_open()) {
```

with:

```cpp
auto *serial = _link ? _link->serial_port() : nullptr;
if (!serial || !serial->is_open()) {
```

When calling export, replace:

```cpp
export_paths_to_grbl(*_port, doc, params, ctx, err, &strokes, &stats)
```

with:

```cpp
export_paths_to_grbl(*serial, doc, params, ctx, err, &strokes, &stats)
```

Keep the existing user message that TCP direct document streaming is not supported in this phase.

- [ ] **Step 6: Remove the old panel helper implementations**

Delete `GrblControlPanel::link_is_open`, `link_write_bytes`, `link_read_line`, `link_write_line`, `link_purge_io`, and `link_close` from `grbl-control-panel.cpp` after all call sites are migrated.

## Task 3: Verify Behavior And Guard Against Regression

**Files:**
- Modify: `testfiles/CMakeLists.txt` only if a new focused test target is feasible.
- Test: `testfiles/src/grbl-client-test.cpp`, `testfiles/src/grbl-export-test.cpp`

- [ ] **Step 1: Compile the panel integration path**

Run:

```powershell
$env:MINGW_CHOST='ucrt64'; $env:MINGW_PREFIX='C:/msys64/ucrt64'; $env:MSYSTEM='UCRT64'; $env:MINGW_PACKAGE_PREFIX='mingw-w64-ucrt-x86_64'; $env:Path='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + $env:Path; cmake --build build-zg --target inkscape
```

Expected: the full app target builds. This is required because `grbl-control-panel.cpp` may not compile in smaller test targets.

- [ ] **Step 2: Re-run existing GRBL tests**

Run:

```powershell
$env:Path='C:/msys64/ucrt64/bin;C:/msys64/usr/bin;' + $env:Path; .\build-zg\bin\grbl-client-test.exe; .\build-zg\bin\test_grbl-export.exe
```

Expected:
- `grbl-client-test.exe` exits 0.
- `test_grbl-export.exe` exits 0, with `HeadlessRealWorldSvgSmoke` skipped unless `INKSCAPE_GRBL_HEADLESS_SVG` is set.

- [ ] **Step 3: Read lints for touched files**

Check lints for:

```text
src/axidraw/device/grbl-link.h
src/axidraw/device/grbl-link.cpp
src/ui/dialog/grbl-control-panel.h
src/ui/dialog/grbl-control-panel.cpp
src/axidraw/CMakeLists.txt
```

Expected: no new diagnostics caused by this change.

- [ ] **Step 4: Confirm diff scope**

Run:

```powershell
git diff --stat -- src/axidraw/device/grbl-link.h src/axidraw/device/grbl-link.cpp src/axidraw/CMakeLists.txt src/ui/dialog/grbl-control-panel.h src/ui/dialog/grbl-control-panel.cpp
```

Expected: only link extraction and call-site replacement. No protocol changes, no G-code generation changes, no UI layout churn.

## Self-Review

- Spec coverage: The plan implements the approved first-stage UI decoupling by extracting serial/TCP ownership and dispatch from `GrblControlPanel`.
- Scope control: It explicitly does not implement TCP direct document streaming or protocol parser changes.
- Testability: The primary verification is compile-level for UI integration plus existing GRBL protocol/export tests. If a fake-port unit test is practical during execution, add it without changing the public scope.
- No placeholders: All touched files, commands, and expected outcomes are specified.
