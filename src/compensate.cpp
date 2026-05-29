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

#include <cassert>
#include <cmath>

//==============================================================================
// Filter internal data structure

struct DataCompensate
{
	PtrNodeRef pInputNode{nullptr};
	PtrNodeRef pFlowNode{nullptr};
	const VSVideoInfo * cpSourceVideoInfo{nullptr};
	int64_t delta{0};
};

//==============================================================================
// Forward declarations

InitVSNodeFun initCompensate;
GetFrameFun getFrameCompensate;
FreeVSNodeFun freeCompensate;

//==============================================================================

void VS_CC createCompensate(const VSMap * a_pIn, VSMap * a_pOut, void * a_pUserData,
	VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pUserData);

	DataCompensate internalData;

	internalData.pInputNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "clip", 0, 0), FreeNodeRef{a_cpVSAPI});
	internalData.pFlowNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "flow", 0, 0), FreeNodeRef{a_cpVSAPI});
	
	internalData.cpSourceVideoInfo = a_cpVSAPI->getVideoInfo(internalData.pInputNode.get());

	//--------------------------------------------------------------------------
	// Format check

	bool acceptableFormat = isConstantFormat(internalData.cpSourceVideoInfo) &&
		(internalData.cpSourceVideoInfo->numFrames != 0);
	if(!acceptableFormat)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Compensate: "
			"only constant format input with fixed frame number is supported.");
		return;
	}

	const VSFormat * cpSourceFormat = internalData.cpSourceVideoInfo->format;
	if((cpSourceFormat->sampleType == stFloat) && (cpSourceFormat->bytesPerSample == 2))
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Compensate: "
			"half precision float formats are not supported.");
		return;
	}

	const VSVideoInfo * cpFlowInfo = a_cpVSAPI->getVideoInfo(internalData.pFlowNode.get());
	const VSFormat * cpFlowFormat = cpFlowInfo->format;

	acceptableFormat = isConstantFormat(cpFlowInfo) &&
		(cpFlowFormat == a_cpVSAPI->getFormatPreset(pfYUV444PS, a_pCore));
	if(!acceptableFormat)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Compensate: "
			"flow clip format is invalid.");
		return;
	}

	bool match = (internalData.cpSourceVideoInfo->width == cpFlowInfo->width) &&
		(internalData.cpSourceVideoInfo->height == cpFlowInfo->height) &&
		(internalData.cpSourceVideoInfo->numFrames == cpFlowInfo->numFrames);
	if(!match)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Compensate: "
			"input clip and flow don't match in size or length.");
		return;
	}

	//--------------------------------------------------------------------------
	// delta

	int error = 0;
	internalData.delta = a_cpVSAPI->propGetInt(a_pIn, "delta", 0, &error);
	if(error != 0)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.Compensate: "
			"failed to initialize the \"delta\" argument.");
		return;
	}

	//--------------------------------------------------------------------------

	DataCompensate * pInternalData = new DataCompensate;
	*pInternalData = std::move(internalData);
	a_cpVSAPI->createFilter(a_pIn, a_pOut, "Compensate", initCompensate,
		getFrameCompensate, freeCompensate, fmParallel, 0, pInternalData, a_pCore);
}

//==============================================================================

void VS_CC initCompensate(VSMap * a_pIn, VSMap * a_pOut, void ** a_ppInstanceData,
	VSNode * a_pNode, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pIn);
	UNUSED(a_pOut);
	UNUSED(a_pCore);

	DataCompensate * pInternalData = (DataCompensate *) *a_ppInstanceData;
	a_cpVSAPI->setVideoInfo(pInternalData->cpSourceVideoInfo, 1, a_pNode);
}

//==============================================================================

// Free all allocated data on filter destruction
void VS_CC freeCompensate(void * a_pInstanceData, VSCore * a_pCore,
	const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pCore);

	DataCompensate * pInternalData = (DataCompensate *)a_pInstanceData;
	delete pInternalData;
}

//==============================================================================

template<typename T>
void compensatePlane(int a_plane, const VSFrameRef * a_cpCurrentFrame,
	const VSFrameRef * a_cpDeltaFrame, const VSFrameRef * a_cpFlowFrame,
	VSFrameRef * a_pOutFrame, const VSFormat * a_cpSourceFormat,
	const VSAPI * a_cpVSAPI)
{
	int divW = (a_plane == 0) ? 1 : (1 << a_cpSourceFormat->subSamplingW);
	int divH = (a_plane == 0) ? 1 : (1 << a_cpSourceFormat->subSamplingH);
	double divVecX = double(divW);
	double divVecY = double(divH);

	int width = a_cpVSAPI->getFrameWidth(a_cpCurrentFrame, a_plane);
	int height = a_cpVSAPI->getFrameHeight(a_cpCurrentFrame, a_plane);

	const uint8_t * cpCurrentPlane = a_cpVSAPI->getReadPtr(a_cpCurrentFrame, a_plane);
	const uint8_t * cpDeltaPlane = a_cpVSAPI->getReadPtr(a_cpDeltaFrame, a_plane);
	int sourceStride = a_cpVSAPI->getStride(a_cpCurrentFrame, a_plane);

	const uint8_t * cpVecXPlane = a_cpVSAPI->getReadPtr(a_cpFlowFrame, PLANE_VEC_X);
	const uint8_t * cpVecYPlane = a_cpVSAPI->getReadPtr(a_cpFlowFrame, PLANE_VEC_Y);
	// Stride should be the same for all flow planes.
	int flowStride = a_cpVSAPI->getStride(a_cpFlowFrame, PLANE_COST);

	const uint8_t * cpSrcRow = cpCurrentPlane;
	uint8_t * pDstRow = a_cpVSAPI->getWritePtr(a_pOutFrame, a_plane);
	int dstStride = a_cpVSAPI->getStride(a_pOutFrame, a_plane);

	for(int h = 0; h < height; ++h)
	{
		const ptrdiff_t flowRowShift = (ptrdiff_t)h * divH * flowStride;
		const float * cpVecXRow = (const float *)(cpVecXPlane + flowRowShift);
		const float * cpVecYRow = (const float *)(cpVecYPlane + flowRowShift);

		const T * cpSrcTypedRow = (const T *)cpSrcRow;
		T * pDstTypedRow = (T *)pDstRow;
		for(int w = 0; w < width; ++w)
		{
			double vecX = (double)cpVecXRow[w * divW] / divVecX;
			double vecY = (double)cpVecYRow[w * divW] / divVecY;
			double x = (double)w + vecX;
			double y = (double)h + vecY;

			bool discard = (x < 0) || (x > (width - 1)) ||
				(y < 0) || (y > (height - 1));

			if(discard)
			{
				pDstTypedRow[w] = cpSrcTypedRow[w];
				continue;
			}

			int x1 = (int)floor(x);
			int x2 = (int)ceil(x);
			double fracX = x - x1;
			int y1 = (int)floor(y);
			int y2 = (int)ceil(y);
			double fracY = y - y1;

			const T * cpDeltaRow1 = (const T *)(cpDeltaPlane + sourceStride * y1);
			const T * cpDeltaRow2 = (const T *)(cpDeltaPlane + sourceStride * y2);
			double rowSum1 = cpDeltaRow1[x1] * (1.0 - fracX) + cpDeltaRow1[x2] * fracX;
			double rowSum2 = cpDeltaRow2[x1] * (1.0 - fracX) + cpDeltaRow2[x2] * fracX;
			pDstTypedRow[w] = T(rowSum1 * (1.0 - fracY) + rowSum2 * fracY);
		}
		cpSrcRow += sourceStride;
		pDstRow += dstStride;
	}
}

//==============================================================================

const VSFrameRef * VS_CC getFrameCompensate(int a_n, int a_activationReason,
	void ** a_ppInstanceData, void ** a_ppFrameData,
	VSFrameContext * a_pFrameCtx, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_ppFrameData);

	DataCompensate * pInternalData = (DataCompensate *) *a_ppInstanceData;
	int framesNumber = pInternalData->cpSourceVideoInfo->numFrames;

	a_n = clamp(a_n, 0, framesNumber - 1);
	int n2 = clamp(a_n + (int)pInternalData->delta, 0, framesNumber - 1);

	if(a_activationReason == arInitial)
	{
		a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pInputNode.get(), a_pFrameCtx);
		a_cpVSAPI->requestFrameFilter(n2, pInternalData->pInputNode.get(), a_pFrameCtx);
		a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pFlowNode.get(), a_pFrameCtx);
		return nullptr;
	}
	else if(a_activationReason != arAllFramesReady)
		return nullptr;

	const VSFormat * cpSourceFormat = pInternalData->cpSourceVideoInfo->format;

	const VSFrameRef * cpCurrentFrame = a_cpVSAPI->getFrameFilter(a_n,
		pInternalData->pInputNode.get(), a_pFrameCtx);
	const VSFrameRef * cpDeltaFrame = a_cpVSAPI->getFrameFilter(n2,
		pInternalData->pInputNode.get(), a_pFrameCtx);
	const VSFrameRef * cpFlowFrame = a_cpVSAPI->getFrameFilter(a_n,
		pInternalData->pFlowNode.get(), a_pFrameCtx);

	VSFrameRef * pOutFrame = a_cpVSAPI->newVideoFrame(cpSourceFormat,
		pInternalData->cpSourceVideoInfo->width, pInternalData->cpSourceVideoInfo->height,
		cpCurrentFrame, a_pCore);

	for(int plane = 0; plane < cpSourceFormat->numPlanes; ++plane)
	{
		auto compensatePlaneType = [&](auto arg)
		{
			using T = std::decay_t<decltype(arg)>;
			compensatePlane<T>(plane, cpCurrentFrame, cpDeltaFrame,
				cpFlowFrame, pOutFrame, cpSourceFormat, a_cpVSAPI);
		};

		if(cpSourceFormat->sampleType == stFloat)
		{
			if(cpSourceFormat->bytesPerSample == 4)
				compensatePlaneType(float());
			else
				assert(false);
		}
		else
		{
			switch(cpSourceFormat->bytesPerSample)
			{
			case 1: compensatePlaneType(uint8_t()); break;
			case 2: compensatePlaneType(uint16_t()); break;
			case 4: compensatePlaneType(uint32_t()); break;
			default: assert(false);
			}
		}
	}

	a_cpVSAPI->freeFrame(cpCurrentFrame);
	a_cpVSAPI->freeFrame(cpDeltaFrame);
	a_cpVSAPI->freeFrame(cpFlowFrame);

	return pOutFrame;
}

//==============================================================================
