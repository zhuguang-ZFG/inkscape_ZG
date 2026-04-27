# GRBL Feedback Split Design

**Goal:** In the GRBL control panel, split plot feedback refreshes into separate `summary` and `overlay` channels while keeping a single semantic trigger entry point, so we can reduce UI stutter and further lower refresh-state drift risk.

**Scope:** This design only covers plot feedback refresh behavior in the Inkscape GRBL control panel. It does not change firmware sync, G-code sending, worker threading, or GTK threading rules.

## Current Problem

The current refresh flow is already better than before because semantic trigger entry points were unified, but the actual timed refresh still tends to treat plot feedback as one bundled operation:

- summary text refresh
- preview overlay rebuild
- canvas redraw

That bundling creates two issues:

1. Some triggers only need textual summary updates, but we still rebuild overlays or keep the design open to that mistake.
2. The code still makes it too easy for future changes to accidentally over-refresh or under-refresh one part of plot feedback.

## Constraints

- Keep the current `idle + timeout` refresh model.
- Do not move plot analysis or overlay generation to background threads in this round.
- Do not alter send, cancel, firmware sync, or transport state behavior.
- Keep the change incremental and test-backed.

## Approaches Considered

### 1. Performance-only split

Split summary and overlay execution internally, but keep trigger meaning loose.

Pros:
- Smallest code change
- Best immediate chance to reduce unnecessary overlay rebuilds

Cons:
- Trigger semantics remain easy to misuse later
- Only partially addresses state correctness

### 2. Semantics-only tightening

Define stricter trigger-to-refresh mapping, but still execute feedback as one bundled operation.

Pros:
- Better correctness story
- Easier future maintenance

Cons:
- Limited direct benefit to stutter
- Still keeps heavy work coupled

### 3. Recommended: small two-channel plan layer

Keep one semantic trigger entry point, but map each trigger to a refresh plan with separate `summary` and `overlay` flags.

Pros:
- Improves correctness and performance together
- Keeps architecture incremental
- Easy to unit test

Cons:
- Slightly broader than a one-file tweak
- Still not a full async performance solution

## Recommended Design

Introduce a small shared plan module that translates semantic GRBL plot-feedback triggers into two refresh channels:

- `refresh_summaries`
- `refresh_overlay`

The control panel keeps using one high-level trigger entry point. Internally, the scheduling and timer path will carry the two-channel plan instead of a single bundled "refresh plot feedback" intention.

## Trigger Semantics

The following triggers should continue to refresh both channels:

- document content changed
- selection changed
- preview visibility changed
- layout changed
- work origin changed

Mapping-related changes become stricter:

- mapping changes that alter geometry, projection, clipping, inversion, alignment, or bed size should refresh both `summary` and `overlay`
- mapping changes that only affect textual summaries should refresh `summary` only

This round does not attempt to classify every mapping control in a deep domain-specific way. It only introduces a structure that makes that classification explicit and testable, then applies a safe first cut.

## Code Structure

Add or adapt a small pure helper module near the existing GRBL feedback state helpers.

Responsibilities:

- trigger plan helper:
  translates semantic trigger reason into a two-channel refresh request

- feedback schedule helper:
  preserves the current debounce behavior while carrying summary/overlay intent

- control panel:
  remains the orchestrator, but stops open-coding which feedback pieces should run for each trigger

## Data Flow

1. UI or document event calls a semantic trigger helper.
2. Trigger helper returns a plan describing whether summary and overlay need refresh.
3. Existing debounce path stores pending refresh intent.
4. Timer execution runs only the requested pieces:
   - summary only
   - overlay only
   - both
5. Overlay-related execution may request canvas redraw; summary-only execution should not rebuild overlay objects.

## Error Handling

- Missing document/desktop should still safely fall back to existing empty-summary behavior.
- Overlay build failures should remain isolated to overlay status and should not block summary refresh.
- Summary refresh failures should remain isolated to summary labels and should not force overlay rebuild.

## Testing Strategy

Add unit tests for the new plan layer:

- trigger reason -> expected summary/overlay flags
- debounce behavior preserves pending overlay when multiple triggers coalesce
- summary-only trigger does not accidentally request overlay rebuild
- legacy dual-refresh triggers still request both

Re-run existing related tests:

- `grbl-panel-feedback-state-test`
- `grbl-panel-feedback-trigger-state-test`
- `grbl-panel-mapping-state-test`

Then run a full build with the existing wrapper script.

## Non-Goals

- background computation
- cancellation of in-flight overlay builds
- redesign of `GrblControlPanel` ownership model
- changing firmware or sender behavior

## Expected Outcome

After this round:

- refresh intent becomes more explicit and easier to maintain
- some over-refresh paths can stop rebuilding overlay unnecessarily
- future canvas-related fixes have a cleaner place to encode semantics without scattering logic across the control panel
