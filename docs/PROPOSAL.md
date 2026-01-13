# Proposal: Address Warning-as-Error Safety Gaps

## Scope
Review the recent warning-driven changes and identify areas where casts hide potential
input validation issues or silent truncation. The goal is to replace “cast-to-silence”
with explicit bounds checks, safer arithmetic, and clearer invariants.

## Key Issues Observed
- Signed-to-unsigned index casts are used without bounds checks in decode loops
  (`include/crate/formats/arc.hh`, `include/crate/formats/zoo.hh`).
- Bit-field truncation for `child_index` in LHA tree logic can silently redirect traversal
  (`include/crate/formats/lha.hh`).
- PPM model arithmetic relies on narrow integer fields with manual casts; overflow or
  truncation can occur with corrupted data (`include/crate/compression/rar_ppm.hh`).
- Iterator construction in RAR data gathering is based on large offsets without explicit
  overflow/underflow guards (`include/crate/formats/rar.hh`).
- Multiple decode paths use `int` intermediates that later become sizes without
  validating non-negativity (LHA/LK7, ARC/ZOO LZW, ACE bit stream).

## Proposed Fixes
1) Add explicit validation before casts
   - For index usage: check `0 <= index && index < container.size()` before any
     `static_cast<size_t>(index)` and return an error on failure.
   - For ranges derived from file metadata (`data_pos`, `compressed_size`), validate
     `data_pos <= vol.size()` and `data_pos + compressed_size <= vol.size()` using
     overflow-safe comparisons before iterator math.

2) Replace bit-field truncation with guarded assignments
   - Introduce helper functions like `checked_u16(value, max)` or `checked_index(value, limit)`
     that return `result_t` and propagate `error_code::CorruptData` on overflow.
   - Use these helpers where `child_index` and node references are assigned in
     `include/crate/formats/lha.hh`.

3) Widen arithmetic for cumulative counters
   - In `include/crate/compression/rar_ppm.hh`, perform intermediate arithmetic in `u32`
     or `u64` and clamp to the target `u16`/`u8` where required. Document the valid ranges
     and add assertions in debug builds if available.

4) Normalize decode loops to size_t
   - Convert loop counters that drive buffer access to `size_t` and retain `int` only for
     sentinel values (e.g., `-1` from bit readers). Once validated, cast to `size_t` once.

5) Add targeted tests for boundary conditions
   - Add tests that feed minimal/empty or malformed inputs to LHA/ARC/ZOO/RAR decoders and
     assert clean error returns instead of undefined behavior.

## Implementation Notes
- Prefer helper utilities in `include/crate/core/` for common checks to avoid repeated
  manual comparisons.
- Keep warning flags intact; use explicit checks instead of disabling diagnostics.
- Ensure behavior for valid archives is unchanged by adding tests for known-good samples.

## Validation Plan
- Build: `cmake -B build && cmake --build build`
- Tests: `ctest --test-dir build -V`
- Add a small corpus of malformed archives in `testdata/` to exercise new error paths.
