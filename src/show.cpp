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

#include <map>

//==============================================================================

const double pi = 3.14159265358979323846;

struct RGB{
	double r;       // ∈ [0, 1]
	double g;       // ∈ [0, 1]
	double b;       // ∈ [0, 1]
};

struct RGB8{
	uint8_t r;       // ∈ [0, 255]
	uint8_t g;       // ∈ [0, 255]
	uint8_t b;       // ∈ [0, 255]
};

struct HSV{
	double h;       // ∈ [0, 2 * pi]
	double s;       // ∈ [0, 1]
	double v;       // ∈ [0, 1]
};

RGB hsv2rgb(HSV hsv)
{
	RGB rgb;
	double h = hsv.h;
	double s = hsv.s;
	double v = hsv.v;

	while(h >= 2. * pi)
		h -= 2. * pi;
	while(h < 0.)
		h += 2. * pi;

	h /= (pi / 3.);
	double fract = h - floor(h);

	double P = v * (1. - s);
	double Q = v * (1. - s * fract);
	double T = v * (1. - s * (1. - fract));

	if(0. <= h && h < 1.)
		rgb = RGB{v, T, P};
	else if (1. <= h && h < 2.)
		rgb = RGB{Q, v, P};
	else if (2. <= h && h < 3.)
		rgb = RGB{P, v, T};
	else if (3. <= h && h < 4.)
		rgb = RGB{P, Q, v};
	else if (4. <= h && h < 5.)
		rgb = RGB{T, P, v};
	else if (5. <= h && h < 6.)
		rgb = RGB{v, P, Q};
	else
		rgb = RGB{0., 0., 0.};

	return rgb;
}

RGB8 vecToRGB(float x, float y, double amplify = 1.)
{
	const double VEC_MAX_LEN = 1024. * sqrt(2.);

	double h = atan2(-y, x);
	double s = sqrt(x * x + y * y) / VEC_MAX_LEN * amplify;
	s = clamp(s, 0., 1.);

	RGB rgb = hsv2rgb({h, s, 1.0});

	uint8_t r8 = uint8_t(rgb.r * 255.);
	uint8_t g8 = uint8_t(rgb.g * 255.);
	uint8_t b8 = uint8_t(rgb.b * 255.);

	return RGB8{r8, g8, b8};
}

//==============================================================================

enum class ShowMode
{
	Flow,
	Cost,
	COUNT,
};

//==============================================================================
// Filter internal data structure

struct DataShow
{
	PtrNodeRef pInputNode{nullptr};
	const VSVideoInfo * cpSourceVideoInfo{nullptr};
	VSVideoInfo videoInfo{};
	ShowMode mode{0};
	double amplify{1.};
};

//==============================================================================
// Forward declarations

InitVSNodeFun initShow;
GetFrameFun getFrameShow;
FreeVSNodeFun freeShow;

//==============================================================================

void VS_CC createShow(const VSMap * a_pIn, VSMap * a_pOut, void * a_pUserData,
	VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pUserData);

	DataShow internalData;

	internalData.pInputNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "clip", 0, 0), FreeNodeRef{a_cpVSAPI});
	internalData.cpSourceVideoInfo = a_cpVSAPI->getVideoInfo(internalData.pInputNode.get());

	//--------------------------------------------------------------------------
	// Format check

	const VSFormat * cpFormat = internalData.cpSourceVideoInfo->format;

	bool acceptableFormat = isConstantFormat(internalData.cpSourceVideoInfo) &&
		(internalData.cpSourceVideoInfo->numFrames != 0) && 
		(cpFormat == a_cpVSAPI->getFormatPreset(pfYUV444PS, a_pCore));
	if(!acceptableFormat)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Show: "
			"input clip format doesn't match the NVOF flow format.");
		return;
	}

	//--------------------------------------------------------------------------
	// mode

	int error = 0;
	int64_t mode = a_cpVSAPI->propGetInt(a_pIn, "mode", 0, &error);
	if(error != 0)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Show: "
			"failed to initialize the \"mode\" argument.");
		return;
	}

	if((mode < 0) || (mode >= (int64_t)ShowMode::COUNT))
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Show: "
			"invalid mode.\n"
			"Supported modes are:\n"
			"0: Flow\n"
			"1: Cost");
		return;
	}

	internalData.mode = (ShowMode)mode;

	//--------------------------------------------------------------------------
	// amplify

	double amplify = a_cpVSAPI->propGetFloat(a_pIn, "amplify", 0, &error);
	if(error == 0)
		internalData.amplify = amplify;

	//--------------------------------------------------------------------------
	// output video info

	static std::map<ShowMode, const VSFormat *> formats{
		{ShowMode::Flow, a_cpVSAPI->getFormatPreset(pfRGB24, a_pCore)},
		{ShowMode::Cost, a_cpVSAPI->getFormatPreset(pfGrayS, a_pCore)},
	};
	std::map<ShowMode, const VSFormat *>::const_iterator it =
		formats.find(internalData.mode);
	if(it == formats.end())
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Show: "
			"no output format for selected mode. Kick the programmer.");
		return;
	}

	internalData.videoInfo = *internalData.cpSourceVideoInfo;
	internalData.videoInfo.format = it->second;

	//--------------------------------------------------------------------------

	DataShow * pInternalData = new DataShow;
	*pInternalData = std::move(internalData);
	a_cpVSAPI->createFilter(a_pIn, a_pOut, "Show", initShow,
		getFrameShow, freeShow, fmParallel, 0, pInternalData, a_pCore);
}

//==============================================================================

void VS_CC initShow(VSMap * a_pIn, VSMap * a_pOut, void ** a_ppInstanceData,
	VSNode * a_pNode, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pIn);
	UNUSED(a_pOut);
	UNUSED(a_pCore);

	DataShow * pInternalData = (DataShow *) *a_ppInstanceData;
	a_cpVSAPI->setVideoInfo(&pInternalData->videoInfo, 1, a_pNode);
}

//==============================================================================

// Free all allocated data on filter destruction
void VS_CC freeShow(void * a_pInstanceData, VSCore * a_pCore,
	const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pCore);

	DataShow * pInternalData = (DataShow *)a_pInstanceData;
	delete pInternalData;
}

//==============================================================================

const VSFrameRef * VS_CC getFrameShow(int a_n, int a_activationReason,
	void ** a_ppInstanceData, void ** a_ppFrameData,
	VSFrameContext * a_pFrameCtx, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_ppFrameData);

	DataShow * pInternalData = (DataShow *) *a_ppInstanceData;
	int framesNumber = pInternalData->cpSourceVideoInfo->numFrames;
	int width = pInternalData->cpSourceVideoInfo->width;
	int height = pInternalData->cpSourceVideoInfo->height;

	a_n = clamp(a_n, 0, framesNumber - 1);

	if(a_activationReason == arInitial)
	{
		a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pInputNode.get(), a_pFrameCtx);
		return nullptr;
	}
	else if(a_activationReason != arAllFramesReady)
		return nullptr;

	const VSFormat * cpFormat = pInternalData->videoInfo.format;

	const VSFrameRef * cpFlowFrame = a_cpVSAPI->getFrameFilter(a_n,
		pInternalData->pInputNode.get(), a_pFrameCtx);

	VSFrameRef * pOutFrame = a_cpVSAPI->newVideoFrame(cpFormat, width, height,
		nullptr, a_pCore);

	if(pInternalData->mode == ShowMode::Flow)
	{
		int vecXStride = a_cpVSAPI->getStride(cpFlowFrame, PLANE_VEC_X);
		const uint8_t * cpVecXRow = a_cpVSAPI->getReadPtr(cpFlowFrame, PLANE_VEC_X);
		int vecYStride = a_cpVSAPI->getStride(cpFlowFrame, PLANE_VEC_Y);
		const uint8_t * cpVecYRow = a_cpVSAPI->getReadPtr(cpFlowFrame, PLANE_VEC_Y);

		int dstRStride = a_cpVSAPI->getStride(pOutFrame, PLANE_R);
		uint8_t * pDstRRow = a_cpVSAPI->getWritePtr(pOutFrame, PLANE_R);
		int dstGStride = a_cpVSAPI->getStride(pOutFrame, PLANE_G);
		uint8_t * pDstGRow = a_cpVSAPI->getWritePtr(pOutFrame, PLANE_G);
		int dstBStride = a_cpVSAPI->getStride(pOutFrame, PLANE_B);
		uint8_t * pDstBRow = a_cpVSAPI->getWritePtr(pOutFrame, PLANE_B);

		for(int h = 0; h < height; ++h)
		{
			const float * cpVecXFloatRow = (const float *)cpVecXRow;
			const float * cpVecYFloatRow = (const float *)cpVecYRow;
			for(int w = 0; w < width; ++w)
			{
				RGB8 rgb = vecToRGB(cpVecXFloatRow[w], cpVecYFloatRow[w], pInternalData->amplify);
				pDstRRow[w] = rgb.r;
				pDstGRow[w] = rgb.g;
				pDstBRow[w] = rgb.b;
			}
			
			cpVecXRow += vecXStride;
			cpVecYRow += vecYStride;
			pDstRRow += dstRStride;
			pDstGRow += dstGStride;
			pDstBRow += dstBStride;
		}
	}
	else if(pInternalData->mode == ShowMode::Cost)
	{
		int srcStride = a_cpVSAPI->getStride(cpFlowFrame, PLANE_COST);
		const uint8_t * cpSrcRow = a_cpVSAPI->getReadPtr(cpFlowFrame, PLANE_COST);
		int dstStride = a_cpVSAPI->getStride(pOutFrame, PLANE_Y);
		uint8_t * pDstRow = a_cpVSAPI->getWritePtr(pOutFrame, PLANE_Y);

		for(int h = 0; h < height; ++h)
		{
			const float * pFloatSrcRow = (const float *)cpSrcRow;
			float * pFloatDstRow = (float *)pDstRow;
			for(int w = 0; w < width; ++w)
			{
				float cost = float(pFloatSrcRow[w] * pInternalData->amplify);
				pFloatDstRow[w] = clamp(cost, 0.f, 1.0f);
			}
			cpSrcRow += srcStride;
			pDstRow += dstStride;
		}
	}

	a_cpVSAPI->freeFrame(cpFlowFrame);

	return pOutFrame;
}

//==============================================================================
