# Add MVTools Vector Export

## Summary
Add `nvof.ToMvTools()` in a new source file to convert dense `nvof.GetFlow()` `YUV444PS` frames into MVTools-compatible vector clips. The output will be a dummy `Gray8` clip with MVTools' required binary frame props attached.

## Public API
Register:

```text
nvof.ToMvTools(clip flow, clip clip, int delta [, int block_size = 8, int block_size_v = block_size, int overlap = 0, int overlap_v = overlap])
```

- `flow`: `nvof.GetFlow()` output, `YUV444PS`, same size/frame count as `clip`.
- `clip`: original/source clip used only for MVTools metadata: dimensions, frame count, bit depth, chroma ratios.
- `delta`: signed reference offset. Positive maps to MVTools backward vectors; negative maps to forward vectors.
- `block_size/block_size_v/overlap/overlap_v`: MVTools block geometry. `pel=1`, `hpad=0`, `vpad=0`, `nLvCount=1`.

## Implementation Changes
- Add `src/to_mvtools.cpp` with a normal VapourSynth filter implementation matching existing style.
- Add `createToMVTools` declaration and registration in `src/main.cpp`.
- Add `src/to_mvtools.cpp` to `msvs/vapoursynth-nvof/vapoursynth-nvof.vcxproj`.
- Define local MVTools-compatible structs with `static_assert(sizeof(VECTOR) == 16)` and `static_assert(sizeof(MVAnalysisData) == 84)`.
- Serialize per frame:
  - `MVTools_MVAnalysisData`: one native `MVAnalysisData` blob.
  - `MVTools_vectors`: `int32 group_size`, `int32 validity`, `int32 plane_size`, then row-major `VECTOR[nBlkX * nBlkY]`.
- Compute block vectors by averaging float X/Y over each block and rounding to integer pixels because `pel=1`.
- Synthesize SAD as `round(avg_cost * block_size * block_size_v * ((1 << bitsPerSample) - 1))`, clamped nonnegative. If `get_cost=False`, the existing zero cost plane yields zero SAD for valid blocks.
- Mark frames invalid when `n + delta` is outside the clip; invalid frames use `{0, 0, verybigSAD}` vectors.
- Output `Gray8` dummy frames with zeroed pixels and copied input frame props, then replace/add the MVTools props.

## Documentation
- Create `plan/nvof-to-mvtools.md` containing this finalized plan.
- Create a new markdown implementation log, `docs/to_mvtools_implementation.md`, and document each implementation step as it is performed.
- Update `readme.txt` with the new filter signature, argument meanings, direction mapping, SAD caveat, and example usage.
- Append compact entries to `notes/history.md` for the request, selected API choices, finalized plan, and later edits.

## Test Plan
- No build will be run unless `BUILD` is provided later, per repo instructions.
- Static validation after edits:
  - Confirm the new source is registered and included in the Visual Studio project.
  - Confirm all new indentation uses 4 spaces and comments match existing style.
  - Confirm metadata fields match MVTools expectations: `nPel=1`, `nLvCount=1`, `nHPadding=0`, `nVPadding=0`, correct direction flags.
  - Confirm vector blob sizes match the exact serialized payload length.
- Scenario checks to run when builds/tests are allowed:
  - Positive `delta=1` produces `isBackward=1`, valid frames except the last.
  - Negative `delta=-1` produces `isBackward=0`, valid frames except the first.
  - YUV420 source metadata yields `xRatioUV=2`, `yRatioUV=2`; Gray/RGB-like metadata yields `1,1`.
  - Overlap validation rejects overlap greater than or equal to block size.
