# Shipped-SDK clarity experiment

This isolated Windows target compiles the production `LumaTextRenderer`, native
rendering-parameter policy and typography code against Pulse's pinned SDK.
It does not build, merge, install, package or release Pulse.

The only wrapper change is the opt-in environment variable
`PULSE_LUMATEXT_FILTER=direct`, read once at renderer initialization. Unset,
`mitchell` and unknown values preserve Mitchell. The stored filter is applied to
both the profile and draw config; two renderer instances keep caches separate.
There is no automatic DPI threshold or production-default change.

`cmake -S tools/clarity_test -B out/clarity-build -G "Visual Studio 17 2022" -A x64`

`cmake --build out/clarity-build --config Release`

`out/clarity-build/Release/pulse_clarity.exe out/clarity`

The 12 PNGs have four columns: native as shipped (Chinese UI typography), native
using precisely the Luma font file/face with baseline alignment, actual wrapper
Mitchell, actual wrapper Direct. Each has 12 rows: weights 400/600, phases 0/.5,
and Latin/Chinese/mixed strings. Physical sizes are 12, 13, 14, 16.25, 19.5 and 26px,
representing small UI text and 13 DIP filenames at 100/125/150/200% scale.
All targets and Luma frames are 96 DPI as in Pulse; physical font scaling happens
once. Both themes are captured. This matrix uses YaHei, face index 0 and no axes;
it does not establish results for Segoe UI Variable. Weight 600 uses Luma's
actual bold-face mapping; exact-font native requests that same bold face.

Every actual native run in the matched column must have the expected file,
face index 0, no simulations and no missing glyph. Native as-shipped faces are
logged independently. Native params come from the actual
`typography::CreateRenderingParams` and retain CI's monitor gamma/gray contrast,
NATURAL_SYMMETRIC, FLAT and grid-fit disabled. CI's virtual monitor cannot stand
in for a user's display tuning. Native uses ordinary snapped D2D layout drawing,
as Pulse does; the matched column aligns nominal baselines, not raster curves.

The harness fails if Luma falls back, a cold image differs from a warm-cache
replay, an eligible short-text cache probe does not hit, a native face mismatches, output is empty/chromatic/
clipped, or the two filter captures are identical. JSON records per-row settings,
font paths, baselines, actual rendering route and edge-spread/coverage metrics.
Edge counts measure spread, not subjective readability or macOS fidelity.

Scope limits: no app-wide screenshot automation, user monitor calibration,
DirectComposition zoom animation, Segoe optical axes, hosted-edit selection,
or native-only colored/highlighted filename routes. There is no release action.

The standalone link includes two fail-closed guards for unrelated full-app
`Compositor` methods referenced by unused functions in `typography.cpp`.
They throw/terminate if accidentally called. None replaces native typography
policy or the actual Luma renderer under test. `/WX` rejects compiler warnings.

Long diagnostic strings may exceed the production 64 KiB per-surface cache cap.
All matrix images must still repeat identically; a separate short-text probe
requires both actual caches to hit and their cached pixels to match cold draws.
