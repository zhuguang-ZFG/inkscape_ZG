# GRBL Streaming Control Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a testable GRBL streaming state, reply parser, and presentation layer, then wire it into the existing GRBL control panel so send progress, paused/cancelled/error/alarm states, and safe button behavior are explicit.

**Architecture:** Add focused `grbl-panel-streaming-*` helpers under `src/ui/dialog/` following the existing `grbl-panel-*` pattern. Keep parsing, state transitions, and GTK presentation separate so most behavior is covered by unit tests before panel integration. Integrate narrowly with `grbl-panel-sender.*` and `grbl-control-panel.*` without rewriting serial transport.

**Tech Stack:** C++17, GTKmm, Glibmm, GTest, existing Inkscape CMake `add_unit_test` harness.

---

## File Structure

- Create `src/ui/dialog/grbl-panel-streaming-reply.h`: GRBL reply event enum, parsed reply struct, and parser function declaration.
- Create `src/ui/dialog/grbl-panel-streaming-reply.cpp`: Pure string parser for `ok`, `error:n`, `ALARM:n`, runtime status reports, informational replies, and unknown text.
- Create `testfiles/src/grbl-panel-streaming-reply-test.cpp`: Parser unit tests.
- Create `src/ui/dialog/grbl-panel-streaming-state.h`: Streaming state data, event methods, and query helpers.
- Create `src/ui/dialog/grbl-panel-streaming-state.cpp`: Deterministic state transitions for start, line written, acknowledgement, pause, resume, cancel, error, alarm, finish, and disconnect.
- Create `testfiles/src/grbl-panel-streaming-state-test.cpp`: State transition tests.
- Create `src/ui/dialog/grbl-panel-streaming-presentation.h`: Presentation plan structs for labels, progress fractions, CSS state, and button sensitivity.
- Create `src/ui/dialog/grbl-panel-streaming-presentation.cpp`: Mapping from streaming state to UI strings and sensitivities.
- Create `testfiles/src/grbl-panel-streaming-presentation-test.cpp`: Presentation tests.
- Modify `testfiles/CMakeLists.txt`: Register the three new unit tests.
- Modify `src/CMakeLists.txt`: Add the new source files to `inkscape_base`.
- Modify `src/ui/dialog/grbl-panel-sender.h`: Add optional streaming callbacks to `GrblPanelSenderContext`.
- Modify `src/ui/dialog/grbl-panel-sender.cpp`: Fire streaming callbacks from the existing editor G-code send loop and direct-send progress callbacks; do not replace `send_line_wait_ok` in this slice.
- Modify `src/ui/dialog/grbl-control-panel.h`: Add streaming status widgets and a `GrblPanelStreamingState` member.
- Modify `src/ui/dialog/grbl-control-panel.cpp`: Initialize/reset streaming state, refresh compact status area, and connect callbacks from `make_sender_context()`.

## Verification Commands

Use the existing build tree if present. If no build tree exists, configure/build must be handled separately by the implementer.

- Preferred targeted tests:
  - `cmake --build build --target grbl-panel-streaming-reply-test`
  - `cmake --build build --target grbl-panel-streaming-state-test`
  - `cmake --build build --target grbl-panel-streaming-presentation-test`
  - `build\testfiles\grbl-panel-streaming-reply-test.exe`
  - `build\testfiles\grbl-panel-streaming-state-test.exe`
  - `build\testfiles\grbl-panel-streaming-presentation-test.exe`
- If this repository uses `inkscape\build-zg` on the local machine, use the same targets under that build directory instead:
  - `cmake --build inkscape\build-zg --target grbl-panel-streaming-reply-test`
  - `cmake --build inkscape\build-zg --target grbl-panel-streaming-state-test`
  - `cmake --build inkscape\build-zg --target grbl-panel-streaming-presentation-test`
- Final smoke build:
  - `cmake --build build --target inkscape`
  - or local equivalent `cmake --build inkscape\build-zg --target inkscape`

### Task 1: GRBL Reply Parser

**Files:**
- Create: `src/ui/dialog/grbl-panel-streaming-reply.h`
- Create: `src/ui/dialog/grbl-panel-streaming-reply.cpp`
- Create: `testfiles/src/grbl-panel-streaming-reply-test.cpp`
- Modify: `testfiles/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write failing parser tests**

Create `testfiles/src/grbl-panel-streaming-reply-test.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-streaming-reply.h"

using Inkscape::UI::Dialog::GrblStreamingReplyKind;
using Inkscape::UI::Dialog::parse_grbl_streaming_reply;

TEST(GrblPanelStreamingReplyTest, ClassifiesOk)
{
    auto const reply = parse_grbl_streaming_reply("ok");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::ok);
    EXPECT_EQ(reply.code, 0);
    EXPECT_EQ(reply.text, "ok");
}

TEST(GrblPanelStreamingReplyTest, ClassifiesErrorWithCode)
{
    auto const reply = parse_grbl_streaming_reply("error:33");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::error);
    EXPECT_EQ(reply.code, 33);
    EXPECT_EQ(reply.text, "error:33");
}

TEST(GrblPanelStreamingReplyTest, ClassifiesAlarmWithCode)
{
    auto const reply = parse_grbl_streaming_reply("ALARM:2");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::alarm);
    EXPECT_EQ(reply.code, 2);
    EXPECT_EQ(reply.text, "ALARM:2");
}

TEST(GrblPanelStreamingReplyTest, ClassifiesRuntimeStatus)
{
    auto const reply = parse_grbl_streaming_reply("<Run|MPos:1.000,2.000,0.000|FS:1200,0>");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::runtime_status);
    EXPECT_EQ(reply.runtime_state, "Run");
}

TEST(GrblPanelStreamingReplyTest, ClassifiesInfoAndUnknown)
{
    EXPECT_EQ(parse_grbl_streaming_reply("Grbl 1.1h ['$' for help]").kind,
              GrblStreamingReplyKind::info);
    EXPECT_EQ(parse_grbl_streaming_reply("[MSG:Reset to continue]").kind,
              GrblStreamingReplyKind::info);
    EXPECT_EQ(parse_grbl_streaming_reply("unexpected text").kind,
              GrblStreamingReplyKind::unknown);
}

TEST(GrblPanelStreamingReplyTest, TrimsWhitespace)
{
    auto const reply = parse_grbl_streaming_reply("  ok\r");
    EXPECT_EQ(reply.kind, GrblStreamingReplyKind::ok);
    EXPECT_EQ(reply.text, "ok");
}
```

- [ ] **Step 2: Register failing parser test**

Add this block near the existing GRBL panel tests in `testfiles/CMakeLists.txt`:

```cmake
add_unit_test(grbl-panel-streaming-reply-test TEST_SOURCE "grbl-panel-streaming-reply-test.cpp"
                                             SOURCES "ui/dialog/grbl-panel-streaming-reply.cpp")
```

Add these source files to the `target_sources(inkscape_base PRIVATE ...)` list in `src/CMakeLists.txt` near other `ui/dialog/grbl-panel-*` entries:

```cmake
ui/dialog/grbl-panel-streaming-reply.cpp
ui/dialog/grbl-panel-streaming-state.cpp
ui/dialog/grbl-panel-streaming-presentation.cpp
```

At this step only `grbl-panel-streaming-reply.cpp` exists; CMake will fail until Tasks 2 and 3 add the other files. If the source list is easier to keep compiling task-by-task, add only `grbl-panel-streaming-reply.cpp` now and add the other two files in their tasks.

- [ ] **Step 3: Run parser test to verify failure**

Run:

```powershell
cmake --build build --target grbl-panel-streaming-reply-test
```

Expected: build fails because `src/ui/dialog/grbl-panel-streaming-reply.h` does not exist yet.

- [ ] **Step 4: Implement parser header**

Create `src/ui/dialog/grbl-panel-streaming-reply.h`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * GRBL streaming reply parsing helpers.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_REPLY_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_REPLY_H

#include <string>

namespace Inkscape::UI::Dialog {

enum class GrblStreamingReplyKind {
    ok,
    error,
    alarm,
    runtime_status,
    info,
    unknown,
};

struct GrblStreamingReply {
    GrblStreamingReplyKind kind = GrblStreamingReplyKind::unknown;
    int code = 0;
    std::string runtime_state;
    std::string text;
};

GrblStreamingReply parse_grbl_streaming_reply(std::string const &line);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_REPLY_H
```

- [ ] **Step 5: Implement parser source**

Create `src/ui/dialog/grbl-panel-streaming-reply.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-streaming-reply.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace Inkscape::UI::Dialog {
namespace {

std::string trim_ascii(std::string text)
{
    auto const is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [&](char ch) {
                   return !is_space(static_cast<unsigned char>(ch));
               }));
    text.erase(std::find_if(text.rbegin(), text.rend(), [&](char ch) {
                   return !is_space(static_cast<unsigned char>(ch));
               }).base(),
               text.end());
    return text;
}

bool starts_with_ci(std::string const &text, char const *prefix)
{
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= text.size()) {
            return false;
        }
        auto const lhs = static_cast<unsigned char>(text[i]);
        auto const rhs = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

int parse_code_after_colon(std::string const &text)
{
    auto const colon = text.find(':');
    if (colon == std::string::npos || colon + 1 >= text.size()) {
        return 0;
    }
    return std::atoi(text.c_str() + colon + 1);
}

} // namespace

GrblStreamingReply parse_grbl_streaming_reply(std::string const &line)
{
    GrblStreamingReply reply;
    reply.text = trim_ascii(line);

    if (reply.text == "ok") {
        reply.kind = GrblStreamingReplyKind::ok;
        return reply;
    }

    if (starts_with_ci(reply.text, "error:")) {
        reply.kind = GrblStreamingReplyKind::error;
        reply.code = parse_code_after_colon(reply.text);
        return reply;
    }

    if (starts_with_ci(reply.text, "ALARM:")) {
        reply.kind = GrblStreamingReplyKind::alarm;
        reply.code = parse_code_after_colon(reply.text);
        return reply;
    }

    if (reply.text.size() >= 3 && reply.text.front() == '<') {
        auto const bar = reply.text.find('|');
        auto const end = reply.text.find('>');
        auto const state_end = bar == std::string::npos ? end : bar;
        if (state_end != std::string::npos && state_end > 1) {
            reply.kind = GrblStreamingReplyKind::runtime_status;
            reply.runtime_state = reply.text.substr(1, state_end - 1);
            return reply;
        }
    }

    if (starts_with_ci(reply.text, "Grbl ") || starts_with_ci(reply.text, "[MSG:") ||
        starts_with_ci(reply.text, "[GC:") || starts_with_ci(reply.text, "[VER:") ||
        starts_with_ci(reply.text, "[OPT:") || starts_with_ci(reply.text, "$")) {
        reply.kind = GrblStreamingReplyKind::info;
        return reply;
    }

    reply.kind = GrblStreamingReplyKind::unknown;
    return reply;
}

} // namespace Inkscape::UI::Dialog
```

- [ ] **Step 6: Run parser test to verify pass**

Run:

```powershell
cmake --build build --target grbl-panel-streaming-reply-test
build\testfiles\grbl-panel-streaming-reply-test.exe
```

Expected: test binary builds and all parser tests pass.

- [ ] **Step 7: Commit parser**

Run:

```powershell
git add src/ui/dialog/grbl-panel-streaming-reply.* testfiles/src/grbl-panel-streaming-reply-test.cpp testfiles/CMakeLists.txt src/CMakeLists.txt
git commit -m "Add GRBL streaming reply parser"
```

### Task 2: Streaming State Model

**Files:**
- Create: `src/ui/dialog/grbl-panel-streaming-state.h`
- Create: `src/ui/dialog/grbl-panel-streaming-state.cpp`
- Create: `testfiles/src/grbl-panel-streaming-state-test.cpp`
- Modify: `testfiles/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write failing state tests**

Create `testfiles/src/grbl-panel-streaming-state-test.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-streaming-reply.h"
#include "src/ui/dialog/grbl-panel-streaming-state.h"

using Inkscape::UI::Dialog::GrblPanelStreamingState;
using Inkscape::UI::Dialog::GrblStreamingPhase;
using Inkscape::UI::Dialog::parse_grbl_streaming_reply;

TEST(GrblPanelStreamingStateTest, StartInitializesTotals)
{
    GrblPanelStreamingState state;
    state.start(10);

    EXPECT_EQ(state.phase(), GrblStreamingPhase::running);
    EXPECT_EQ(state.total_lines(), 10u);
    EXPECT_EQ(state.lines_written(), 0u);
    EXPECT_EQ(state.lines_acknowledged(), 0u);
    EXPECT_EQ(state.in_flight_lines(), 0u);
}

TEST(GrblPanelStreamingStateTest, OkAdvancesAcknowledgements)
{
    GrblPanelStreamingState state;
    state.start(3);
    state.line_written();
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("ok"));

    EXPECT_EQ(state.lines_written(), 2u);
    EXPECT_EQ(state.lines_acknowledged(), 1u);
    EXPECT_EQ(state.in_flight_lines(), 1u);
    EXPECT_FALSE(state.is_terminal());
}

TEST(GrblPanelStreamingStateTest, CompleteWhenAllWrittenLinesAreAcknowledged)
{
    GrblPanelStreamingState state;
    state.start(2);
    state.line_written();
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("ok"));
    state.apply_reply(parse_grbl_streaming_reply("ok"));
    state.finish_if_complete();

    EXPECT_EQ(state.phase(), GrblStreamingPhase::complete);
    EXPECT_TRUE(state.is_terminal());
}

TEST(GrblPanelStreamingStateTest, ErrorBlocksFurtherSending)
{
    GrblPanelStreamingState state;
    state.start(2);
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("error:33"));

    EXPECT_EQ(state.phase(), GrblStreamingPhase::error);
    EXPECT_TRUE(state.is_terminal());
    EXPECT_FALSE(state.can_feed_more_lines());
    EXPECT_EQ(state.error_text(), "error:33");
}

TEST(GrblPanelStreamingStateTest, ExplicitFailureBlocksFurtherSending)
{
    GrblPanelStreamingState state;
    state.start(2);
    state.fail("serial write failed");

    EXPECT_EQ(state.phase(), GrblStreamingPhase::error);
    EXPECT_TRUE(state.is_terminal());
    EXPECT_FALSE(state.can_feed_more_lines());
    EXPECT_EQ(state.error_text(), "serial write failed");
}

TEST(GrblPanelStreamingStateTest, AlarmBlocksResume)
{
    GrblPanelStreamingState state;
    state.start(2);
    state.request_pause();
    state.apply_reply(parse_grbl_streaming_reply("ALARM:2"));

    EXPECT_EQ(state.phase(), GrblStreamingPhase::alarm);
    EXPECT_TRUE(state.is_terminal());
    EXPECT_FALSE(state.can_resume());
    EXPECT_EQ(state.alarm_text(), "ALARM:2");
}

TEST(GrblPanelStreamingStateTest, PauseAndResumeAreExplicit)
{
    GrblPanelStreamingState state;
    state.start(4);
    state.line_written();
    state.request_pause();

    EXPECT_EQ(state.phase(), GrblStreamingPhase::paused);
    EXPECT_FALSE(state.can_feed_more_lines());
    EXPECT_TRUE(state.can_resume());
    EXPECT_EQ(state.in_flight_lines(), 1u);

    state.resume();
    EXPECT_EQ(state.phase(), GrblStreamingPhase::running);
    EXPECT_TRUE(state.can_feed_more_lines());
}

TEST(GrblPanelStreamingStateTest, CancelIsIdempotent)
{
    GrblPanelStreamingState state;
    state.start(4);
    EXPECT_TRUE(state.request_cancel());
    EXPECT_FALSE(state.request_cancel());
    EXPECT_EQ(state.phase(), GrblStreamingPhase::cancelling);
    EXPECT_FALSE(state.can_feed_more_lines());
}

TEST(GrblPanelStreamingStateTest, DisconnectResetsToIdle)
{
    GrblPanelStreamingState state;
    state.start(4);
    state.line_written();
    state.disconnect();

    EXPECT_EQ(state.phase(), GrblStreamingPhase::idle);
    EXPECT_EQ(state.total_lines(), 0u);
    EXPECT_EQ(state.in_flight_lines(), 0u);
}
```

- [ ] **Step 2: Register failing state test**

Add to `testfiles/CMakeLists.txt`:

```cmake
add_unit_test(grbl-panel-streaming-state-test TEST_SOURCE "grbl-panel-streaming-state-test.cpp"
                                             SOURCES "ui/dialog/grbl-panel-streaming-state.cpp"
                                                     "ui/dialog/grbl-panel-streaming-reply.cpp")
```

Add `ui/dialog/grbl-panel-streaming-state.cpp` to `src/CMakeLists.txt` if not already added in Task 1.

- [ ] **Step 3: Run state test to verify failure**

Run:

```powershell
cmake --build build --target grbl-panel-streaming-state-test
```

Expected: build fails because state header/source do not exist yet.

- [ ] **Step 4: Implement state header**

Create `src/ui/dialog/grbl-panel-streaming-state.h`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Testable GRBL streaming state transitions.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_STATE_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_STATE_H

#include <cstddef>
#include <string>

#include "grbl-panel-streaming-reply.h"

namespace Inkscape::UI::Dialog {

enum class GrblStreamingPhase {
    idle,
    running,
    paused,
    cancelling,
    complete,
    error,
    alarm,
};

class GrblPanelStreamingState {
public:
    void start(std::size_t total_lines);
    void line_written();
    void apply_reply(GrblStreamingReply const &reply);
    void fail(std::string error_text);
    void request_pause();
    bool resume();
    bool request_cancel();
    void finish_if_complete();
    void disconnect();

    GrblStreamingPhase phase() const { return _phase; }
    std::size_t total_lines() const { return _total_lines; }
    std::size_t lines_written() const { return _lines_written; }
    std::size_t lines_acknowledged() const { return _lines_acknowledged; }
    std::size_t in_flight_lines() const;
    std::string const &error_text() const { return _error_text; }
    std::string const &alarm_text() const { return _alarm_text; }
    std::string const &runtime_state() const { return _runtime_state; }

    bool can_feed_more_lines() const;
    bool can_resume() const;
    bool is_terminal() const;

private:
    GrblStreamingPhase _phase = GrblStreamingPhase::idle;
    std::size_t _total_lines = 0;
    std::size_t _lines_written = 0;
    std::size_t _lines_acknowledged = 0;
    std::string _error_text;
    std::string _alarm_text;
    std::string _runtime_state;
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_STATE_H
```

- [ ] **Step 5: Implement state source**

Create `src/ui/dialog/grbl-panel-streaming-state.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-streaming-state.h"

#include <algorithm>
#include <utility>

namespace Inkscape::UI::Dialog {

void GrblPanelStreamingState::start(std::size_t const total_lines)
{
    _phase = GrblStreamingPhase::running;
    _total_lines = total_lines;
    _lines_written = 0;
    _lines_acknowledged = 0;
    _error_text.clear();
    _alarm_text.clear();
    _runtime_state.clear();
}

void GrblPanelStreamingState::line_written()
{
    if (!can_feed_more_lines()) {
        return;
    }
    ++_lines_written;
}

void GrblPanelStreamingState::apply_reply(GrblStreamingReply const &reply)
{
    switch (reply.kind) {
        case GrblStreamingReplyKind::ok:
            if (_lines_acknowledged < _lines_written) {
                ++_lines_acknowledged;
            }
            break;
        case GrblStreamingReplyKind::error:
            _phase = GrblStreamingPhase::error;
            _error_text = reply.text;
            break;
        case GrblStreamingReplyKind::alarm:
            _phase = GrblStreamingPhase::alarm;
            _alarm_text = reply.text;
            break;
        case GrblStreamingReplyKind::runtime_status:
            _runtime_state = reply.runtime_state;
            break;
        case GrblStreamingReplyKind::info:
        case GrblStreamingReplyKind::unknown:
            break;
    }
}

void GrblPanelStreamingState::fail(std::string error_text)
{
    _phase = GrblStreamingPhase::error;
    _error_text = std::move(error_text);
}

void GrblPanelStreamingState::request_pause()
{
    if (_phase == GrblStreamingPhase::running) {
        _phase = GrblStreamingPhase::paused;
    }
}

bool GrblPanelStreamingState::resume()
{
    if (!can_resume()) {
        return false;
    }
    _phase = GrblStreamingPhase::running;
    return true;
}

bool GrblPanelStreamingState::request_cancel()
{
    if (_phase != GrblStreamingPhase::running && _phase != GrblStreamingPhase::paused) {
        return false;
    }
    _phase = GrblStreamingPhase::cancelling;
    return true;
}

void GrblPanelStreamingState::finish_if_complete()
{
    if ((_phase == GrblStreamingPhase::running || _phase == GrblStreamingPhase::cancelling) &&
        _total_lines > 0 && _lines_written >= _total_lines && _lines_acknowledged >= _lines_written) {
        _phase = GrblStreamingPhase::complete;
    }
}

void GrblPanelStreamingState::disconnect()
{
    _phase = GrblStreamingPhase::idle;
    _total_lines = 0;
    _lines_written = 0;
    _lines_acknowledged = 0;
    _error_text.clear();
    _alarm_text.clear();
    _runtime_state.clear();
}

std::size_t GrblPanelStreamingState::in_flight_lines() const
{
    return _lines_written > _lines_acknowledged ? _lines_written - _lines_acknowledged : 0;
}

bool GrblPanelStreamingState::can_feed_more_lines() const
{
    return _phase == GrblStreamingPhase::running &&
           (_total_lines == 0 || _lines_written < _total_lines);
}

bool GrblPanelStreamingState::can_resume() const
{
    return _phase == GrblStreamingPhase::paused;
}

bool GrblPanelStreamingState::is_terminal() const
{
    return _phase == GrblStreamingPhase::complete ||
           _phase == GrblStreamingPhase::error ||
           _phase == GrblStreamingPhase::alarm;
}

} // namespace Inkscape::UI::Dialog
```

- [ ] **Step 6: Run state test to verify pass**

Run:

```powershell
cmake --build build --target grbl-panel-streaming-state-test
build\testfiles\grbl-panel-streaming-state-test.exe
```

Expected: all state tests pass.

- [ ] **Step 7: Commit state model**

Run:

```powershell
git add src/ui/dialog/grbl-panel-streaming-state.* testfiles/src/grbl-panel-streaming-state-test.cpp testfiles/CMakeLists.txt src/CMakeLists.txt
git commit -m "Add GRBL streaming state model"
```

### Task 3: Streaming Presentation

**Files:**
- Create: `src/ui/dialog/grbl-panel-streaming-presentation.h`
- Create: `src/ui/dialog/grbl-panel-streaming-presentation.cpp`
- Create: `testfiles/src/grbl-panel-streaming-presentation-test.cpp`
- Modify: `testfiles/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write failing presentation tests**

Create `testfiles/src/grbl-panel-streaming-presentation-test.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-streaming-presentation.h"
#include "src/ui/dialog/grbl-panel-streaming-state.h"

using Inkscape::UI::Dialog::GrblPanelStreamingState;
using Inkscape::UI::Dialog::GrblStreamingPhase;
using Inkscape::UI::Dialog::make_grbl_streaming_presentation;
using Inkscape::UI::Dialog::parse_grbl_streaming_reply;

TEST(GrblPanelStreamingPresentationTest, IdleHasNeutralCopy)
{
    GrblPanelStreamingState state;
    auto const presentation = make_grbl_streaming_presentation(state);

    EXPECT_FALSE(presentation.visible);
    EXPECT_EQ(presentation.progress_fraction, 0.0);
    EXPECT_FALSE(presentation.pause_sensitive);
    EXPECT_FALSE(presentation.resume_sensitive);
    EXPECT_FALSE(presentation.stop_sensitive);
}

TEST(GrblPanelStreamingPresentationTest, RunningShowsProgressAndInFlight)
{
    GrblPanelStreamingState state;
    state.start(10);
    state.line_written();
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("ok"));

    auto const presentation = make_grbl_streaming_presentation(state);
    EXPECT_TRUE(presentation.visible);
    EXPECT_NE(presentation.progress_label.raw().find("1 / 10"), std::string::npos);
    EXPECT_NE(presentation.in_flight_label.raw().find("1"), std::string::npos);
    EXPECT_NEAR(presentation.progress_fraction, 0.1, 0.0001);
    EXPECT_TRUE(presentation.pause_sensitive);
    EXPECT_FALSE(presentation.resume_sensitive);
    EXPECT_TRUE(presentation.stop_sensitive);
}

TEST(GrblPanelStreamingPresentationTest, PausedEnablesResume)
{
    GrblPanelStreamingState state;
    state.start(10);
    state.request_pause();

    auto const presentation = make_grbl_streaming_presentation(state);
    EXPECT_NE(presentation.phase_label.raw().find("Paused"), std::string::npos);
    EXPECT_FALSE(presentation.pause_sensitive);
    EXPECT_TRUE(presentation.resume_sensitive);
    EXPECT_TRUE(presentation.stop_sensitive);
    EXPECT_EQ(presentation.css_class, "warning");
}

TEST(GrblPanelStreamingPresentationTest, ErrorAndAlarmAreBlocking)
{
    GrblPanelStreamingState error_state;
    error_state.start(10);
    error_state.apply_reply(parse_grbl_streaming_reply("error:33"));
    auto const error_presentation = make_grbl_streaming_presentation(error_state);
    EXPECT_NE(error_presentation.blocking_label.raw().find("error:33"), std::string::npos);
    EXPECT_EQ(error_presentation.css_class, "error");
    EXPECT_FALSE(error_presentation.resume_sensitive);

    GrblPanelStreamingState alarm_state;
    alarm_state.start(10);
    alarm_state.apply_reply(parse_grbl_streaming_reply("ALARM:2"));
    auto const alarm_presentation = make_grbl_streaming_presentation(alarm_state);
    EXPECT_NE(alarm_presentation.blocking_label.raw().find("ALARM:2"), std::string::npos);
    EXPECT_EQ(alarm_presentation.css_class, "error");
    EXPECT_FALSE(alarm_presentation.resume_sensitive);
}

TEST(GrblPanelStreamingPresentationTest, CompleteShowsDone)
{
    GrblPanelStreamingState state;
    state.start(1);
    state.line_written();
    state.apply_reply(parse_grbl_streaming_reply("ok"));
    state.finish_if_complete();

    auto const presentation = make_grbl_streaming_presentation(state);
    EXPECT_TRUE(presentation.visible);
    EXPECT_NE(presentation.phase_label.raw().find("Complete"), std::string::npos);
    EXPECT_NEAR(presentation.progress_fraction, 1.0, 0.0001);
    EXPECT_EQ(presentation.css_class, "success");
}
```

- [ ] **Step 2: Register failing presentation test**

Add to `testfiles/CMakeLists.txt`:

```cmake
add_unit_test(grbl-panel-streaming-presentation-test TEST_SOURCE "grbl-panel-streaming-presentation-test.cpp"
                                                    SOURCES "ui/dialog/grbl-panel-streaming-presentation.cpp"
                                                            "ui/dialog/grbl-panel-streaming-state.cpp"
                                                            "ui/dialog/grbl-panel-streaming-reply.cpp"
                                         EXTRA_LIBS GLibmm::GLibmm)
```

Add `ui/dialog/grbl-panel-streaming-presentation.cpp` to `src/CMakeLists.txt` if not already added.

- [ ] **Step 3: Run presentation test to verify failure**

Run:

```powershell
cmake --build build --target grbl-panel-streaming-presentation-test
```

Expected: build fails because presentation header/source do not exist yet.

- [ ] **Step 4: Implement presentation header**

Create `src/ui/dialog/grbl-panel-streaming-presentation.h`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * UI presentation helpers for GRBL streaming state.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_PRESENTATION_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_PRESENTATION_H

#include <string>

#include <glibmm/ustring.h>

#include "grbl-panel-streaming-state.h"

namespace Inkscape::UI::Dialog {

struct GrblStreamingPresentation {
    bool visible = false;
    double progress_fraction = 0.0;
    Glib::ustring progress_label;
    Glib::ustring in_flight_label;
    Glib::ustring phase_label;
    Glib::ustring blocking_label;
    std::string css_class;
    bool pause_sensitive = false;
    bool resume_sensitive = false;
    bool stop_sensitive = false;
};

GrblStreamingPresentation make_grbl_streaming_presentation(GrblPanelStreamingState const &state);

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_STREAMING_PRESENTATION_H
```

- [ ] **Step 5: Implement presentation source**

Create `src/ui/dialog/grbl-panel-streaming-presentation.cpp`:

```cpp
// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-streaming-presentation.h"

#include <algorithm>

#include <glibmm/i18n.h>

namespace Inkscape::UI::Dialog {
namespace {

Glib::ustring phase_label_for(GrblStreamingPhase const phase)
{
    switch (phase) {
        case GrblStreamingPhase::running:
            return _("Running");
        case GrblStreamingPhase::paused:
            return _("Paused");
        case GrblStreamingPhase::cancelling:
            return _("Cancelling");
        case GrblStreamingPhase::complete:
            return _("Complete");
        case GrblStreamingPhase::error:
            return _("Error");
        case GrblStreamingPhase::alarm:
            return _("Alarm");
        case GrblStreamingPhase::idle:
        default:
            return _("Idle");
    }
}

std::string css_class_for(GrblStreamingPhase const phase)
{
    switch (phase) {
        case GrblStreamingPhase::paused:
        case GrblStreamingPhase::cancelling:
            return "warning";
        case GrblStreamingPhase::error:
        case GrblStreamingPhase::alarm:
            return "error";
        case GrblStreamingPhase::complete:
            return "success";
        case GrblStreamingPhase::running:
            return "running";
        case GrblStreamingPhase::idle:
        default:
            return "idle";
    }
}

} // namespace

GrblStreamingPresentation make_grbl_streaming_presentation(GrblPanelStreamingState const &state)
{
    GrblStreamingPresentation p;
    p.visible = state.phase() != GrblStreamingPhase::idle;
    p.phase_label = phase_label_for(state.phase());
    p.css_class = css_class_for(state.phase());

    auto const total = state.total_lines();
    auto const acknowledged = state.lines_acknowledged();
    if (total > 0) {
        p.progress_fraction = std::min(1.0, static_cast<double>(acknowledged) / static_cast<double>(total));
        p.progress_label = Glib::ustring::compose(_("Sent %1 / %2 lines"),
                                                  static_cast<guint64>(acknowledged),
                                                  static_cast<guint64>(total));
    } else {
        p.progress_fraction = 0.0;
        p.progress_label = _("No active stream");
    }

    p.in_flight_label = Glib::ustring::compose(_("Awaiting %1 acknowledgements"),
                                               static_cast<guint64>(state.in_flight_lines()));

    if (state.phase() == GrblStreamingPhase::error) {
        p.blocking_label = Glib::ustring::compose(_("Streaming stopped on %1"), state.error_text());
    } else if (state.phase() == GrblStreamingPhase::alarm) {
        p.blocking_label = Glib::ustring::compose(_("Controller alarm: %1"), state.alarm_text());
    } else if (!state.runtime_state().empty()) {
        p.blocking_label = Glib::ustring::compose(_("Controller state: %1"), state.runtime_state());
    }

    p.pause_sensitive = state.phase() == GrblStreamingPhase::running;
    p.resume_sensitive = state.can_resume();
    p.stop_sensitive = state.phase() == GrblStreamingPhase::running ||
                       state.phase() == GrblStreamingPhase::paused;
    return p;
}

} // namespace Inkscape::UI::Dialog
```

- [ ] **Step 6: Run presentation test to verify pass**

Run:

```powershell
cmake --build build --target grbl-panel-streaming-presentation-test
build\testfiles\grbl-panel-streaming-presentation-test.exe
```

Expected: all presentation tests pass.

- [ ] **Step 7: Commit presentation**

Run:

```powershell
git add src/ui/dialog/grbl-panel-streaming-presentation.* testfiles/src/grbl-panel-streaming-presentation-test.cpp testfiles/CMakeLists.txt src/CMakeLists.txt
git commit -m "Add GRBL streaming presentation model"
```

### Task 4: Sender Callback Integration

**Files:**
- Modify: `src/ui/dialog/grbl-panel-sender.h`
- Modify: `src/ui/dialog/grbl-panel-sender.cpp`

- [ ] **Step 1: Add callback fields to sender context**

Modify `GrblPanelSenderContext` in `src/ui/dialog/grbl-panel-sender.h`:

```cpp
std::function<void(std::size_t)> streaming_started;
std::function<void()> streaming_line_written;
std::function<void(std::string const &)> streaming_reply_received;
std::function<void(std::string const &)> streaming_failed;
std::function<void()> streaming_finished;
```

Keep them optional. All sender code must check each callback before calling it.

- [ ] **Step 2: Fire callbacks from editor send path**

In `GrblPanelSender::run_editor_gcode_send_worker`, after `prepare_stream_link(link);` and before creating `GcodeSendProgressTracker`, call:

```cpp
if (context.streaming_started) {
    context.streaming_started(total_exec);
}
```

Immediately after a successful `send_line_wait_ok(line, err)` call and before `++sent`, call:

```cpp
if (context.streaming_line_written) {
    context.streaming_line_written();
}
if (context.streaming_reply_received) {
    context.streaming_reply_received("ok");
}
```

Before every return from the `with_plot_waits` lambda after stream completion or failure, call:

```cpp
if (context.streaming_finished) {
    context.streaming_finished();
}
```

If `send_line_wait_ok` fails and `err` is non-empty, call:

```cpp
if (context.streaming_failed) {
    context.streaming_failed(err);
}
```

This is intentionally conservative: the current transport helper does not expose raw replies, so this slice records known successful acknowledgements and explicit failure text without rewriting transport.

- [ ] **Step 3: Fire callbacks from direct send path**

In `run_direct_send_worker`, after `prepare_stream_link(context.link);`, call:

```cpp
if (context.streaming_started) {
    context.streaming_started(0);
}
```

In `attach_direct_send_progress_callbacks`, do not change the function signature yet. Direct document send already emits line and stroke progress through export callbacks, but not raw `ok` events. For this slice, finish by calling `streaming_finished` after `export_paths_to_grbl` returns or fails.

When `export_paths_to_grbl` returns false with `err`, call:

```cpp
if (context.streaming_failed) {
    context.streaming_failed(err);
}
```

- [ ] **Step 4: Build sender**

Run:

```powershell
cmake --build build --target inkscape_base
```

Expected: `grbl-panel-sender.cpp` compiles. If the local build does not expose `inkscape_base`, build `inkscape` instead.

- [ ] **Step 5: Commit sender callbacks**

Run:

```powershell
git add src/ui/dialog/grbl-panel-sender.*
git commit -m "Expose GRBL streaming callbacks from sender"
```

### Task 5: Control Panel Status UI

**Files:**
- Modify: `src/ui/dialog/grbl-control-panel.h`
- Modify: `src/ui/dialog/grbl-control-panel.cpp`

- [ ] **Step 1: Add state and widgets to panel header**

In `src/ui/dialog/grbl-control-panel.h`, include the new headers:

```cpp
#include "ui/dialog/grbl-panel-streaming-state.h"
```

Add private methods near other UI refresh helpers:

```cpp
void refresh_streaming_status_ui();
void reset_streaming_status_ui();
```

Add private members near `_machine_status` and `_status`:

```cpp
GrblPanelStreamingState _streaming_state;
Gtk::Label _streaming_phase;
Gtk::Label _streaming_progress;
Gtk::Label _streaming_in_flight;
Gtk::Label _streaming_blocking;
```

- [ ] **Step 2: Include presentation/parser in panel source**

In `src/ui/dialog/grbl-control-panel.cpp`, add:

```cpp
#include "ui/dialog/grbl-panel-streaming-presentation.h"
#include "ui/dialog/grbl-panel-streaming-reply.h"
```

- [ ] **Step 3: Implement refresh helpers**

Add near `post_machine_status` or other small UI helpers:

```cpp
void GrblControlPanel::refresh_streaming_status_ui()
{
    auto const presentation = make_grbl_streaming_presentation(_streaming_state);
    _streaming_phase.set_text(presentation.phase_label);
    _streaming_progress.set_text(presentation.progress_label);
    _streaming_in_flight.set_text(presentation.in_flight_label);
    _streaming_blocking.set_text(presentation.blocking_label);

    bool const visible = presentation.visible;
    _streaming_phase.set_visible(visible);
    _streaming_progress.set_visible(visible);
    _streaming_in_flight.set_visible(visible);
    _streaming_blocking.set_visible(visible && !presentation.blocking_label.empty());
}

void GrblControlPanel::reset_streaming_status_ui()
{
    _streaming_state.disconnect();
    refresh_streaming_status_ui();
}
```

- [ ] **Step 4: Build compact status area**

In `build_log_section()`, before packing `_status`, add a small vertical box:

```cpp
auto *stream_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
_streaming_phase.add_css_class("grbl-panel-note");
_streaming_progress.add_css_class("grbl-panel-note");
_streaming_in_flight.add_css_class("grbl-panel-note");
_streaming_blocking.add_css_class("grbl-panel-note");
_streaming_blocking.add_css_class("error");
Inkscape::UI::pack_start(*stream_box, _streaming_phase, false, false, 0);
Inkscape::UI::pack_start(*stream_box, _streaming_progress, false, false, 0);
Inkscape::UI::pack_start(*stream_box, _streaming_in_flight, false, false, 0);
Inkscape::UI::pack_start(*stream_box, _streaming_blocking, false, false, 0);
Inkscape::UI::pack_start(*box_log, *stream_box, false, false, 0);
```

Then call `reset_streaming_status_ui()` from the constructor after widget setup, or set all four labels invisible in `build_log_section()`.

- [ ] **Step 5: Wire sender callbacks**

In `GrblControlPanel::make_sender_context()`, populate the new callbacks:

```cpp
.streaming_started = [this](std::size_t total) {
    Glib::signal_idle().connect_once([this, total] {
        _streaming_state.start(total);
        refresh_streaming_status_ui();
    });
},
.streaming_line_written = [this] {
    Glib::signal_idle().connect_once([this] {
        _streaming_state.line_written();
        refresh_streaming_status_ui();
    });
},
.streaming_reply_received = [this](std::string const &line) {
    Glib::signal_idle().connect_once([this, line] {
        _streaming_state.apply_reply(parse_grbl_streaming_reply(line));
        _streaming_state.finish_if_complete();
        refresh_streaming_status_ui();
    });
},
.streaming_failed = [this](std::string const &err) {
    Glib::signal_idle().connect_once([this, err] {
        _streaming_state.fail(err);
        refresh_streaming_status_ui();
    });
},
.streaming_finished = [this] {
    Glib::signal_idle().connect_once([this] {
        _streaming_state.finish_if_complete();
        refresh_streaming_status_ui();
    });
},
```

If aggregate initialization order becomes hard to read, switch `make_sender_context()` to construct a local `GrblPanelSenderContext context;` and assign fields one by one.

- [ ] **Step 6: Reset on disconnect and finish**

Call `reset_streaming_status_ui()` in `disconnect_controller()` after `post_machine_status({});`.

In `finish_gcode_stream_worker()` or the existing finish path that clears `_gcode_sending`, call `refresh_streaming_status_ui()` after runtime state refresh so complete/error/alarm text stays visible until the next send or disconnect.

- [ ] **Step 7: Build panel**

Run:

```powershell
cmake --build build --target inkscape
```

Expected: panel and new helpers compile.

- [ ] **Step 8: Commit panel UI integration**

Run:

```powershell
git add src/ui/dialog/grbl-control-panel.* src/ui/dialog/grbl-panel-sender.*
git commit -m "Show GRBL streaming status in control panel"
```

### Task 6: Final Verification and Documentation Check

**Files:**
- Modify docs only if implementation behavior diverges from `docs/superpowers/specs/2026-05-09-grbl-streaming-control-parity-design.md`

- [ ] **Step 1: Run targeted tests**

Run:

```powershell
cmake --build build --target grbl-panel-streaming-reply-test
cmake --build build --target grbl-panel-streaming-state-test
cmake --build build --target grbl-panel-streaming-presentation-test
build\testfiles\grbl-panel-streaming-reply-test.exe
build\testfiles\grbl-panel-streaming-state-test.exe
build\testfiles\grbl-panel-streaming-presentation-test.exe
```

Expected: all three test binaries pass.

- [ ] **Step 2: Run existing nearby tests**

Run:

```powershell
cmake --build build --target grbl-runtime-state-test
cmake --build build --target grbl-panel-presentation-test
build\testfiles\grbl-runtime-state-test.exe
build\testfiles\grbl-panel-presentation-test.exe
```

Expected: existing runtime and presentation tests still pass.

- [ ] **Step 3: Run build smoke**

Run:

```powershell
cmake --build build --target inkscape
```

Expected: Inkscape builds successfully.

- [ ] **Step 4: Review git diff**

Run:

```powershell
git diff --stat HEAD
git diff -- src/ui/dialog/grbl-panel-streaming-reply.* src/ui/dialog/grbl-panel-streaming-state.* src/ui/dialog/grbl-panel-streaming-presentation.* src/ui/dialog/grbl-panel-sender.* src/ui/dialog/grbl-control-panel.* testfiles/CMakeLists.txt src/CMakeLists.txt
```

Expected: changes are limited to the planned streaming helpers, tests, CMake registration, sender callbacks, and compact panel UI wiring.

- [ ] **Step 5: Commit final verification note if docs changed**

If docs changed:

```powershell
git add docs/superpowers/specs/2026-05-09-grbl-streaming-control-parity-design.md
git commit -m "Update GRBL streaming control design notes"
```

If docs did not change, do not create an empty commit.
