# MVTools Degrain Safety And Batch Vector Export

## Summary
The current `ToMvTools` implementation only invalidates whole frames when `n + delta` is outside the clip. It does not clamp per-block vector coordinates. MVTools Degrain dereferences vector-shifted source pointers directly, so out-of-bounds or non-finite vectors can plausibly explain warped colors and freezes.

Fix this in two layers: harden `ToMvTools` so exported vectors are safe for Degrain, then add a batched bidirectional MVTools-vector API that produces all Degrain vector clips for a temporal radius with shared NVOF work instead of many independent `GetFlow()` calls.

## API Changes
- Extend `nvof.ToMvTools()`:
  ```text
  nvof.ToMvTools(clip flow, clip clip, int delta [, int block_size = 8, int block_size_v = block_size, int overlap = 0, int overlap_v = overlap, int pel = 1, int fallback_sad = -1, int clipped_sad = -1])
  ```
- Add `nvof.GetMvTools()`:
  ```text
  nvof.GetMvTools(clip clip [, clip source = clip, int tr = 1, int block_size = 8, int block_size_v = block_size, int overlap = 0, int overlap_v = overlap, int pel = 1, int fallback_sad = -1, int clipped_sad = -1, bool get_cost = false, int gpu = 0])
  ```
- `GetMvTools()` returns clip arrays ordered `[bwd1, fwd1, bwd2, fwd2, bwd3, fwd3]`, truncated to `2 * tr`.

## Implementation Notes
- Refactor MVTools metadata and vector serialization into shared helpers.
- Scale vectors by `pel`, round to integer MVTools units, sanitize non-finite values, and clamp every block vector to source-frame bounds.
- Assign high SAD to clipped or sanitized vectors and use an automatic conservative SAD for zero-cost valid blocks unless `fallback_sad=0`.
- Add NVOF bidirectional execution support via `NV_OF_PRED_DIRECTION_BOTH` and direct CPU flow/cost downloads.
- Share one NVOF backend and a bounded pair cache across all output clips returned by one `GetMvTools()` invocation.

## Test Plan
- No build unless `BUILD` is provided.
- Static checks: registration, project inclusion, metadata fields, blob sizes, and direction flags.
- Runtime checks when builds/tests are allowed: clipped vectors do not corrupt Degrain, `get_cost=False` does not export all-zero SAD by default, `pel=2` works with `mv.Super(pel=2)`, and `tr=2/3` return the expected clip order.
