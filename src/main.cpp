//==============================================================================
// Copyright (c) 2022 Aleksey [Mystery Keeper] Lyashin (mystkeeper@gmail.com)
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//==============================================================================

#include "common.h"

CreateVSNodeFun createGetFlow;
CreateVSNodeFun createShow;
CreateVSNodeFun createCompensate;
CreateVSNodeFun createCorrelation;
CreateVSNodeFun createToMVTools;
CreateVSNodeFun createGetMVTools;

//==============================================================================

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin a_configFunc,
	VSRegisterFunction a_registerFunc, VSPlugin * a_pPlugin)
{
	a_configFunc("motion.opticalflow.nvidia", "nvof",
		"nVidia optical flow tools",
		VAPOURSYNTH_API_VERSION, 1, a_pPlugin);

	a_registerFunc("GetFlow",
		"clip:clip;"
		"delta:int;"
		"chroma_motion:int:opt;"
		"speed:int:opt;"
		"get_cost:int:opt;"
		"gpu:int:opt;"
		, createGetFlow, nullptr, a_pPlugin);

	a_registerFunc("GetMvTools",
		"clip:clip;"
		"source:clip:opt;"
		"tr:int:opt;"
		"block_size:int:opt;"
		"block_size_v:int:opt;"
		"overlap:int:opt;"
		"overlap_v:int:opt;"
		"pel:int:opt;"
		"fallback_sad:int:opt;"
		"clipped_sad:int:opt;"
		"get_cost:int:opt;"
		"gpu:int:opt;"
		"bidirectional:int:opt;"
		, createGetMVTools, nullptr, a_pPlugin);

	a_registerFunc("Show",
		"clip:clip;"
		"mode:int;"
		"amplify:float:opt;"
		, createShow, nullptr, a_pPlugin);

	a_registerFunc("Compensate",
		"clip:clip;"
		"flow:clip;"
		"delta:int;"
		, createCompensate, nullptr, a_pPlugin);

	a_registerFunc("Correlation",
		"clip:clip;"
		"compensated:clip;"
		"radius:int:opt;"
		, createCorrelation, nullptr, a_pPlugin);

    a_registerFunc("ToMvTools",
        "flow:clip;"
        "clip:clip;"
        "delta:int;"
        "block_size:int:opt;"
        "block_size_v:int:opt;"
        "overlap:int:opt;"
        "overlap_v:int:opt;"
        "pel:int:opt;"
        "fallback_sad:int:opt;"
        "clipped_sad:int:opt;"
        , createToMVTools, nullptr, a_pPlugin);
}

//==============================================================================
