# feature/auto-resize-v4l2 — per-tile output offsets + runtime rotate/flip + gap support

## Summary
This branch adds per-tile, pixel-accurate output offsets for the 150 tiles rendered by the shader, runtime controls to rotate and flip the input image, plus configurable vertical gap handling between tile rows to model physical display pitches. Offsets are loaded from three module files (modul1.txt, modul2.txt, modul3.txt) and can be reloaded at runtime. The change preserves the existing source sampling logic while moving the displayed tiles on the output; it also hardens shader uniform handling and improves texture reallocation on V4L2 format changes.

## Why
- Corrects misaligned tiles by applying per-tile pixel offsets without modifying input sampling.
- Models physical vertical gaps between displays (display pitch) by configuring zero spacing between specific tile rows.
- Makes it easy to adjust layout at runtime via simple text files and reload key.
- Keeps shader and upload logic performant and robust for NV12/NV21 capture formats.

## Key features
- Per-tile output offsets:
  - 150 ivec2 offsets (`offsetxy1[150]`) passed to the fragment shader via `glUniform2iv`.
  - Interpretation: `offset.x` positive → shift tile right; negative → left. `offset.y` positive → shift tile down; negative → up.
  - Offsets move the output tile rectangles only; the input texture sampling region remains unchanged.

- Runtime reload:
  - `modul1.txt` / `modul2.txt` / `modul3.txt` (50 pairs each) are read by the program.
  - Press `k` to reload files at runtime and upload offsets to the shader.

- Input transformations:
  - `rot` uniform (0/1/2/3 → 0°/90°/180°/270° clockwise) applied to input UVs before sampling.
  - `flip_x` and `flip_y` uniforms toggle horizontal/vertical mirroring of the input.
  - Keys: `r` = rotate (default step configurable; branch uses 180° step), `h` = toggle horizontal mirror, `v` = toggle vertical mirror.

- Gap controls (new):
  - `gap_count` and `gap_rows` shader uniforms define gaps in Y between tile rows.
  - Each entry in `gap_rows` is a 1-based row index `g` meaning "treat spacing between row g and row g+1 as zero".
  - Example default: `gap_rows = {5, 10}` means no vertical spacing between rows 5↔6 and 10↔11 — models the physical display seams/pitch.
  - Set from C++ via `glUniform1i(loc_gap_count, ...)` and `glUniform1iv(loc_gap_rows, ARRAY_SIZE, ...)`.
  - Gap array size in shader is 8 by default; changeable if needed.

- Robustness / UX:
  - Shader uniforms are uploaded every frame to avoid driver optimizations causing missing uniforms.
  - The loader checks executable dir and CWD, logs attempts and read counts.
  - Textures are reallocated on V4L2 format changes; UV swap detection and optional CPU UV swap are supported.
  - Extensive console logging for debugging offsets and uniform locations.

## Files changed / primary locations
- `hdmi_simple_display.cpp`
  - Updated: offset file loader + logging, uniform uploads for `offsetxy1`, `rot`, `flip_x`, `flip_y`, `gap_count`, `gap_rows`; key handling (`k`, `h`, `v`, `r`); texture reallocation logic.
- `shaders/shader.frag.glsl`
  - Updated: applies per-tile output offsets to displayed tile rectangles, supports input UV rotation/flip and gap-aware Y-positioning of rows.
- (No changes to vertex shader.)

## Uniforms used by shader
- ivec2 offsetxy1[150]          — per-tile output offsets (pixel units)
- int rot                      — input rotation (0..3)
- int flip_x, flip_y           — input flips
- int gap_count                — number of valid entries in gap_rows
- int gap_rows[8]              — list (1-based) of rows after which spacing should be zero
- int uv_swap, full_range, use_bt709, view_mode, segmentIndex

## Module file format (`modul1.txt` / `modul2.txt` / `modul3.txt`)
- Up to 50 lines per file (missing entries are zero-filled).
- Each valid line: two signed integers separated by whitespace:
  ```
  <x> <y>
  ```
  Example:
  ```
  -3 5
  0 0
  2 -1
  ```
- Lines starting with `#` or blank lines are ignored.
- Files are searched in the executable directory and in the current working directory.

## How to configure gaps (C++)
- Default in code: two gaps after rows 5 and 10 (to match your 3-display vertical stack and tile pitch).
- Location in code: after shader link and `glGetUniformLocation` calls you can set:
  ```c++
  const int GAP_ARRAY_SIZE = 8;
  int gap_count = 2;
  int gap_rows_arr[GAP_ARRAY_SIZE] = { 5, 10, 0, 0, 0, 0, 0, 0 };
  if (loc_gap_count >= 0) glUniform1i(loc_gap_count, gap_count);
  if (loc_gap_rows >= 0)  glUniform1iv(loc_gap_rows, GAP_ARRAY_SIZE, gap_rows_arr);
  ```
- To change at runtime: (not yet in code by default) you can:
  - Modify the code to read a gap config file and reload on `k`, or
  - Add CLI options to set `gap_rows` on launch (can be added quickly).

## How to build & test
1. Build:
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(nproc)
   ```
2. Prepare module files (example, minimal):
   ```bash
   echo "-3 5" > modul1.txt   # first tile: 3px left, 5px down
   # create modul2.txt/modul3.txt similarly (or leave empty)
   ```
3. Run:
   ```bash
   ./build/hdmi_simple_display
   ```
4. Controls:
   - `k` : reload modul1/2/3 and upload offsets
   - `h` : toggle horizontal mirror (input)
   - `v` : toggle vertical mirror (input)
   - `r` : rotate input (default: 180° per press in this branch)
   - `f` : fullscreen toggle
   - `ESC`: exit

## Example workflow
1. Put `modul1.txt` in executable dir; for the first tile add:
   ```
   -3 5
   ```
2. Start program and press `k`. Console prints will show loaded entries and confirm upload. Visual result: first tile moves 3px left and 5px down.
3. If you have three displays stacked vertically and want no vertical spacing between display seams, ensure `gap_rows` includes the row indices after which spacing should be zero (e.g. 5 and 10 in the default layout).

## Notes & gotchas
- If shader compiler optimizes unused uniforms away, the program will report `loc_* == -1` in the console. The fragment shader in this branch references these uniforms so their locations should be valid.
- Offsets can cause overlapping tiles or gaps; gaps are rendered black by design. You may change that to a background color if desired.
- `gap_rows` entries are 1-based and must be in range `1 .. (numTilesPerCol-1)`. Array size is 8 by default; increase if you need more gaps.

## Suggested commit & PR meta
- Commit message:
  ```
  Add per-tile output offsets, gap support and runtime rotate/flip controls; enhance loader logging
  ```
- PR title:
  ```
  feature/auto-resize-v4l2: per-tile offsets + gap handling + runtime rotate/flip
  ```
- PR body: use the Summary + Key features + How to build & test sections above.

---

If you want, I can:
- Commit `PR_DESCRIPTION.md` to `feature/auto-resize-v4l2` (tell me the commit message), or
- Generate the exact git commands to add/commit/push your local modified files (paste `git status --porcelain` and I'll output the commands).
