NVIDIA Optical Flow plugin for VapourSynth
by Aleksey [Mystery Keeper] Lyashin

Plugin that utilizes NVOF to calculate a dense optical flow between two frames of the same clip for the purpose of motion compensation.

At this moment (2022-06-18) this plugin is an unoptimized proof of concept. Likely, logically flawed. Not recommended for real use.

Original repository: https://bitbucket.org/mystery_keeper/vapoursynth-nvof

Filters:

--------------------------------------------------------------------------------

nvof.getFlow(clip clip, int delta [, bool chromaMotion = True, int speed = 0, getCost=True, int gpu = 0])
	clip: The clip to calculate the flow on. Must have 8bit depth. YUV420P8 always works. Other formats with subsampling only work with chromaMotion=False.
	delta: Step between the original and new frames. Can be negative for backwards flow.
	chromaMotion: Boolean flag whether to use chroma for the flow estimation. Silently ignored for RGB clips.
	speed: Speed VS quality preset. Valid values are 0 to 2, with 0 being the slowest and the best quality.
	getCost: Boolean flag whether to request the cost field from NVOF API. Not requesting the cost field results in lower RAM and VRAM consumption.
	gpu: The id of GPU used for the flow calculation.

Flow calculation filter.
The resulting flow is stored in YUV444PS float format:
	Y plane stores the cost function, normalized between 0 and 1.
	U plane stores motion vectors X value.
	V plane stores motion vectors Y value.

Note that NVOF can only produce vectors with max X and Y value of ~1024.

The size of the clip is equal to the original clip. The only requirement for working with the flow clip is that the sizes of the clip you work on and the flow clip match. For example, when working with a 10bit clip, converting it to a 8bit reference clip, calculating the flow on it, then applying it to the original 10bit clip - is valid and encouraged approach.

--------------------------------------------------------------------------------

nvof.compensate(clip clip, clip flow, int delta [, int cost_threshold = 1.0])
	clip: The clip to motion-compensate. Can be of any planar format, except half precision float formats.
	flow: The optical flow clip. Must have the same size with clip.
	delta: The step between the frames. Should be the same as used for the flow calculation. Filter makes no effort to ensure that.

Motion compensation filter.
Output format matches the clip format. Replaces the current frame pixels with the values from where they moved in the second frame, according to the motion vector. For fractional vectors bilinear sampling is used.

--------------------------------------------------------------------------------

nvof.show(clip clip, int mode = 0 [, float amplify = 1.0])
	clip: The optical flow clip.
	mode: 0 for the optical flow; 1 for the cost function.
	amplify: multiplicator for the value being visualized. Values are clamped at their mode corresponding boundaries.

Presents the flow perceivable way.
Mode 0: Output format is RGB24. The optical flow motion vectors are visualized in HSV colorspace, with Hue representing the vector angle, Saturation - the vector length, and Value set as 1. Amplify argument multiplies the vector length, resulting in higher saturation.
Mode 1: Output format is GrayS. The cost function is visualized in grayscale. Amplify argument multiplies the cost value.

--------------------------------------------------------------------------------
