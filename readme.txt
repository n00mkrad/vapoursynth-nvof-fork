NVIDIA Optical Flow plugin for VapourSynth
by Aleksey [Mystery Keeper] Lyashin

Plugin that utilizes NVOF to calculate a dense optical flow between two frames of the same clip for the purpose of motion compensation.

At this moment (2022-06-18) this plugin is an unoptimized proof of concept. Likely, logically flawed. Not recommended for real use.

Original repository: https://bitbucket.org/mystery_keeper/vapoursynth-nvof

Filters:

--------------------------------------------------------------------------------

nvof.GetFlow(clip clip, int delta [, bool chroma_motion = True, int speed = 0, get_cost=True, int gpu = 0])
	clip: The clip to calculate the flow on. Must have 8bit depth. YUV420P8 always works. Other formats with subsampling only work with chroma_motion=False.
	delta: Step between the original and new frames. Can be negative for backwards flow.
	chroma_motion: Boolean flag whether to use chroma for the flow estimation. Silently ignored for RGB clips.
	speed: Speed VS quality preset. Valid values are 0 to 2, with 0 being the slowest and the best quality.
	get_cost: Boolean flag whether to request the cost field from NVOF API. Not requesting the cost field results in lower RAM and VRAM consumption.
	gpu: The id of GPU used for the flow calculation.

Flow calculation filter.
The resulting flow is stored in YUV444PS float format:
	Y plane stores the cost function, normalized between 0 and 1.
	U plane stores motion vectors X value.
	V plane stores motion vectors Y value.

Note that NVOF can only produce vectors with max X and Y value of ~1024.

The size of the clip is equal to the original clip. The only requirement for working with the flow clip is that the sizes of the clip you work on and the flow clip match. For example, when working with a 10bit clip, converting it to a 8bit reference clip, calculating the flow on it, then applying it to the original 10bit clip - is valid and encouraged approach.

--------------------------------------------------------------------------------

nvof.ToMvTools(clip flow, clip clip, int delta [, int block_size = 8, int block_size_v = block_size, int overlap = 0, int overlap_v = overlap, int pel = 1, int fallback_sad = -1, int clipped_sad = -1])
	flow: The optical flow clip produced by nvof.GetFlow. Must be YUV444PS and match clip size and frame count.
	clip: The source clip whose format metadata should be mirrored for MVTools consumers. Must be constant integer format up to 16 bits.
	delta: The same signed frame offset used for nvof.GetFlow. Positive values are written as MVTools backward vectors, negative values as forward vectors.
	block_size, block_size_v: Horizontal and vertical MVTools block size. These affect the output grid precision, not NVOF estimation quality.
	overlap, overlap_v: Horizontal and vertical MVTools overlap. Must be smaller than the corresponding block size.
	pel: MVTools vector precision. Valid values are 1, 2, and 4. Must match the pel value used by mv.Super.
	fallback_sad: SAD used for zero-cost valid blocks. The default -1 computes block_size * block_size_v * pixelMax / 32. Use 0 to preserve old zero-SAD behavior when get_cost=False.
	clipped_sad: Minimum SAD assigned to vectors that had to be clipped or sanitized. The default -1 uses a very large SAD.

Converts dense NVOF flow to an MVTools-compatible vector clip.
The output image is a dummy Gray8 clip. MVTools reads the binary frame properties MVTools_MVAnalysisData and MVTools_vectors, not the pixels.

The converter writes a single MVTools level with hpad=0 and vpad=0. Each MVTools vector is the block average of the dense NVOF X/Y vectors, scaled to the selected pel precision, and rounded to integer MVTools units. Vectors are clipped per block so MVTools consumers do not read outside the source frame. Non-finite vectors are replaced with zero and marked with a high SAD.

SAD is synthesized from the normalized NVOF cost plane:
	SAD = round(avg_cost * block_size * block_size_v * ((1 << bitsPerSample) - 1))

NVOF cost is not the same as MVTools SAD or confidence. This value is only a placeholder for MVTools filters that require SAD to exist.

Example:
	flow = core.nvof.GetFlow(src8, delta=1, get_cost=True)
	mvbw = core.nvof.ToMvTools(flow, src, delta=1, block_size=8, overlap=0, pel=1)

--------------------------------------------------------------------------------

nvof.GetMvTools(clip clip [, clip source = clip, int tr = 1, int block_size = 8, int block_size_v = block_size, int overlap = 0, int overlap_v = overlap, int pel = 1, int fallback_sad = -1, int clipped_sad = -1, bool get_cost = False, int gpu = 0, bool bidirectional = False])
	clip: The 8-bit clip to calculate NVOF on.
	source: Optional source clip whose format metadata should be mirrored for MVTools consumers. Must match clip size and frame count and use integer samples up to 16 bits.
	tr: Temporal radius. Valid values are 1 to 3.
	block_size, block_size_v, overlap, overlap_v, pel, fallback_sad, clipped_sad: Same meaning as nvof.ToMvTools.
	get_cost: Boolean flag whether to request the cost field from NVOF API.
	gpu: The id of GPU used for the flow calculation.
	bidirectional: Boolean flag whether to use NV_OF_PRED_DIRECTION_BOTH. The default False uses forward-only NVOF calls because it can be faster on some GPUs/drivers.

Returns MVTools-compatible vector clips directly, without first creating intermediate YUV444PS flow clips. The returned clip array is ordered as:
	[bwd1, fwd1, bwd2, fwd2, bwd3, fwd3]

Use the first 2 * tr clips with mv.Degrain1, mv.Degrain2, or mv.Degrain3. Positive-distance clips are written as MVTools backward vectors and negative-distance clips are written as forward vectors.

By default each requested direction is calculated with regular forward NVOF execution. Setting bidirectional=True computes both directions for each frame pair with NV_OF_PRED_DIRECTION_BOTH and can help when the driver/GPU makes that path faster than two forward-only calls.

Example:
	mvs = core.nvof.GetMvTools(src8, source=src, tr=2, block_size=8, overlap=4, pel=1)
	out = core.mv.Degrain2(src, sup, mvs[0], mvs[1], mvs[2], mvs[3], thsad=600)

--------------------------------------------------------------------------------

nvof.Compensate(clip clip, clip flow, int delta [, int cost_threshold = 1.0])
	clip: The clip to motion-compensate. Can be of any planar format, except half precision float formats.
	flow: The optical flow clip. Must have the same size with clip.
	delta: The step between the frames. Should be the same as used for the flow calculation. Filter makes no effort to ensure that.

Motion compensation filter.
Output format matches the clip format. Replaces the current frame pixels with the values from where they moved in the second frame, according to the motion vector. For fractional vectors bilinear sampling is used.

--------------------------------------------------------------------------------

nvof.Show(clip clip, int mode = 0 [, float amplify = 1.0])
	clip: The optical flow clip.
	mode: 0 for the optical flow; 1 for the cost function.
	amplify: multiplicator for the value being visualized. Values are clamped at their mode corresponding boundaries.

Presents the flow perceivable way.
Mode 0: Output format is RGB24. The optical flow motion vectors are visualized in HSV colorspace, with Hue representing the vector angle, Saturation - the vector length, and Value set as 1. Amplify argument multiplies the vector length, resulting in higher saturation.
Mode 1: Output format is GrayS. The cost function is visualized in grayscale. Amplify argument multiplies the cost value.

--------------------------------------------------------------------------------
