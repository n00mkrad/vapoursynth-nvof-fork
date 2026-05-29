# ToMvTools Implementation Log

## Implemented Steps
- Reviewed the existing plugin entrypoint, NVOF flow frame layout, MVTools vector clip spec, MVTools fake reader code, and the MSVC project file.
- Added `src/to_mvtools.cpp` as a separate converter source file to keep MVTools serialization separate from the NVOF flow generation code.
- Added local MVTools ABI structs and compile-time size checks for `VECTOR` and `MVAnalysisData`.
- Implemented `nvof.ToMvTools(flow, clip, delta, block_size, block_size_v, overlap, overlap_v)` with source metadata derived from `clip` and vector/cost data read from `flow`.
- Implemented signed `delta` mapping so positive deltas produce MVTools backward metadata and negative deltas produce forward metadata.
- Implemented block-averaged vector conversion from dense float X/Y planes to one integer `pel=1` MVTools vector per block.
- Implemented placeholder SAD synthesis from the normalized NVOF cost plane, with invalid frames filled by `{0, 0, verybigSAD}`.
- Implemented per-frame serialization of `MVTools_MVAnalysisData` and `MVTools_vectors` frame properties.
- Made the output frames dummy `Gray8` frames with zeroed pixels and copied flow frame properties before replacing the MVTools props.
- Registered the new function in `src/main.cpp`.
- Added the new source file to `msvs/vapoursynth-nvof/vapoursynth-nvof.vcxproj`.
- Copied the finalized implementation plan to `plan/nvof-to-mvtools.md`.
- Updated `readme.txt` with the new filter documentation and an example.
- Updated `notes/history.md` with compact entries for the request and implementation.

## Notes
- The NVOF cost field is not a true confidence or SAD value. The generated SAD is intentionally a low-priority placeholder intended to satisfy MVTools consumers that require the field.
- The converter uses `nLvCount=1`, configurable `pel`, `nHPadding=0`, and `nVPadding=0`.
- No build was run because the prompt did not include `BUILD`.

## Degrain Safety And Batch Export Update
- Investigated the Degrain corruption/freeze report and confirmed the original converter only invalidated whole frames, but did not clamp individual block vectors before writing MVTools props.
- Added shared MVTools helper code for metadata generation, blob serialization, vector clipping, non-finite vector sanitization, pel scaling, and SAD synthesis.
- Updated `nvof.ToMvTools()` with `pel`, `fallback_sad`, and `clipped_sad` arguments.
- Changed `ToMvTools` vector conversion so each block average is scaled by `pel`, rounded to MVTools units, clipped to source-frame bounds, and marked with high SAD if clipped or sanitized.
- Changed zero-cost valid blocks to use an automatic conservative fallback SAD by default. Passing `fallback_sad=0` preserves the earlier zero-SAD behavior.
- Added bidirectional CPU flow download support to the NVOF wrapper using `NV_OF_PRED_DIRECTION_BOTH`.
- Added `nvof.GetMvTools()` to return Degrain-ready vector clips in `[bwd1, fwd1, bwd2, fwd2, bwd3, fwd3]` order for `tr=1..3`.
- Implemented shared backend state and a bounded pair cache for `GetMvTools()` so all returned vector clips from one call reuse one NVOF instance and avoid repeated forward/backward pair work.
- Registered `GetMvTools()` and added the new source/helper files to the Visual Studio project.
- Updated the readme with the new arguments, clipping behavior, SAD caveat, and batched Degrain usage.
- No build was run because the prompt did not include `BUILD`.

## GetMvTools Crash Fix
- Investigated a crash where `bwd1` crashed on frame 0 and `fwd1` produced only the initial invalid edge frame before crashing on the first valid pair.
- Identified that `GetMvTools()` was calling NVOF directly from VapourSynth worker threads, bypassing the dedicated-thread pattern previously needed for stable `GetFlow()` execution.
- Added a dedicated `NVidiaOpticalFlowDataWorker` inside `GetMvTools()` so bidirectional NVOF initialization, execution, and destruction happen on a stable worker thread.
- Kept the existing process-wide NVOF lock around worker-thread NVOF calls and retained the shared pair cache around the worker.
- No build was run because the prompt did not include `BUILD`.
