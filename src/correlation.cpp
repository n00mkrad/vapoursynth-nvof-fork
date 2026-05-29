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

#include <vector>

#undef min
#undef max
#include <algorithm>

//==============================================================================

struct PlaneWithCorrelation
{
	int width{0};
	int height{0};
	int subSamplingW{1};
	int subSamplingH{1};
	const uint8_t * cpSourceBytes{nullptr};
	int sourceStride{0};
	const uint8_t * cpCompensatedBytes{nullptr};
	int compensatedStride{0};
	std::vector<float> correlation;
};

//==============================================================================
// Filter internal data structure

struct DataCorrelation
{
	PtrNodeRef pInputNode{nullptr};
	PtrNodeRef pCompensatedNode{nullptr};
	const VSVideoInfo * cpSourceVideoInfo{nullptr};
	VSVideoInfo videoInfo{};
	int radius{2};
};

//==============================================================================
// Forward declarations

InitVSNodeFun initCorrelation;
GetFrameFun getFrameCorrelation;
FreeVSNodeFun freeCorrelation;

//==============================================================================

void VS_CC createCorrelation(const VSMap * a_pIn, VSMap * a_pOut, void * a_pUserData,
	VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pUserData);

	DataCorrelation internalData;

	internalData.pInputNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "clip", 0, 0),
		FreeNodeRef{a_cpVSAPI});
	internalData.pCompensatedNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "compensated", 0, 0),
		FreeNodeRef{a_cpVSAPI});
	internalData.cpSourceVideoInfo = a_cpVSAPI->getVideoInfo(internalData.pInputNode.get());

	//--------------------------------------------------------------------------
	// Format check

	bool acceptableFormat = isConstantFormat(internalData.cpSourceVideoInfo) &&
		(internalData.cpSourceVideoInfo->numFrames != 0);
	if(!acceptableFormat)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.correlation: "
			"unacceptable input clip format.");
		return;
	}

	const VSVideoInfo * cpCompensatedVideoInfo = a_cpVSAPI->getVideoInfo(
		internalData.pCompensatedNode.get());
	bool equalClips = isSameFormat(internalData.cpSourceVideoInfo, cpCompensatedVideoInfo) &&
		(internalData.cpSourceVideoInfo->numFrames == cpCompensatedVideoInfo->numFrames);
	if(!equalClips)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.correlation: "
			"source and compensated clip format and frame number must match.");
		return;
	}

	//--------------------------------------------------------------------------
	// radius

	int error = 0;
	int64_t radius = a_cpVSAPI->propGetInt(a_pIn, "radius", 0, &error);
	if(error == 0)
		internalData.radius = (int)radius;

	//--------------------------------------------------------------------------
	// output video info

	internalData.videoInfo = *internalData.cpSourceVideoInfo;
	internalData.videoInfo.format = a_cpVSAPI->getFormatPreset(pfGrayS, a_pCore);

	//--------------------------------------------------------------------------

	DataCorrelation * pInternalData = new DataCorrelation;
	*pInternalData = std::move(internalData);
	a_cpVSAPI->createFilter(a_pIn, a_pOut, "correlation", initCorrelation,
		getFrameCorrelation, freeCorrelation, fmParallel, 0, pInternalData, a_pCore);
}

//==============================================================================

void VS_CC initCorrelation(VSMap * a_pIn, VSMap * a_pOut, void ** a_ppInstanceData,
	VSNode * a_pNode, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pIn);
	UNUSED(a_pOut);
	UNUSED(a_pCore);

	DataCorrelation * pInternalData = (DataCorrelation *) *a_ppInstanceData;
	a_cpVSAPI->setVideoInfo(&pInternalData->videoInfo, 1, a_pNode);
}

//==============================================================================

// Free all allocated data on filter destruction
void VS_CC freeCorrelation(void * a_pInstanceData, VSCore * a_pCore,
	const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pCore);

	DataCorrelation * pInternalData = (DataCorrelation *)a_pInstanceData;
	delete pInternalData;
}

//==============================================================================

template<typename T>
float calculateBlockCorrelation(const ConstSampler2D<T> & a_sampler1,
	const ConstSampler2D<T> & a_sampler2, int a_hMin, int a_hMax, int a_wMin, int a_wMax)
{
	int blockSize = (a_hMax - a_hMin + 1) * (a_wMax - a_wMin + 1);

	std::vector<double> block1(blockSize, 0.0);
	std::vector<double> block2(blockSize, 0.0);

	double mean1 = 0.0;
	double mean2 = 0.0;

	size_t i = 0;
	for(int h = a_hMin; h <= a_hMax; ++h)
	{
		for(int w = a_wMin; w < a_wMax; ++w)
		{
			double value1(a_sampler1[h][w]);
			double value2(a_sampler2[h][w]);
			block1[i] = value1;
			block2[i] = value2;
			mean1 += value1;
			mean2 += value2;
			i++;
		}
	}

	mean1 /= blockSize;
	mean2 /= blockSize;

	double correlation = 0.0;

	for(i = 0; i < blockSize; ++i)
	{
		static const double EPSILON = 1.E-20;

		double value1 = block1[i] - mean1;
		double value2 = block2[i] - mean2;

		if((std::abs(value1) < EPSILON) && (std::abs(value2) < EPSILON))
		{
			correlation += 1.0;
			continue;
		}

		double denom = std::max(std::abs(value1), std::abs(value2));
		correlation += value1 * value2 / denom / denom;
	}

	correlation /= blockSize;
	correlation = clamp(correlation, 0.0, 1.0);

	return (float)correlation;
}

//==============================================================================

template<typename T>
void calculatePlaneCorelation(PlaneWithCorrelation & a_plane, int a_radius)
{
	int radX = a_radius >> a_plane.subSamplingW;
	int radY = a_radius >> a_plane.subSamplingH;
	
	ConstSampler2D<T> samplerSource{a_plane.cpSourceBytes, a_plane.sourceStride};
	ConstSampler2D<T> samplerCompensated{a_plane.cpCompensatedBytes, a_plane.compensatedStride};

	float * pCorrelation = a_plane.correlation.data();

	for(int h = 0; h < a_plane.height; ++h)
	{
		int hMin = std::max(h - radY, 0);
		int hMax = std::min(h + radY, a_plane.height - 1);

		for(int w = 0; w < a_plane.width; ++w)
		{
			int wMin = std::max(w - radX, 0);
			int wMax = std::min(w + radX, a_plane.width - 1);

			*pCorrelation = calculateBlockCorrelation<T>(samplerSource, samplerCompensated,
				hMin, hMax, wMin, wMax);
			pCorrelation++;
		}
	}
}

//==============================================================================

const VSFrameRef * VS_CC getFrameCorrelation(int a_n, int a_activationReason,
	void ** a_ppInstanceData, void ** a_ppFrameData,
	VSFrameContext * a_pFrameCtx, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_ppFrameData);

	DataCorrelation * pInternalData = (DataCorrelation *) *a_ppInstanceData;
	int framesNumber = pInternalData->cpSourceVideoInfo->numFrames;
	int width = pInternalData->cpSourceVideoInfo->width;
	int height = pInternalData->cpSourceVideoInfo->height;
	const VSFormat * cpSourceFormat = pInternalData->cpSourceVideoInfo->format;

	a_n = clamp(a_n, 0, framesNumber - 1);

	if(a_activationReason == arInitial)
	{
		a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pInputNode.get(), a_pFrameCtx);
		a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pCompensatedNode.get(), a_pFrameCtx);
		return nullptr;
	}
	else if(a_activationReason != arAllFramesReady)
		return nullptr;

	const VSFormat * cpFormat = pInternalData->videoInfo.format;
	size_t planesCount = cpFormat->numPlanes;

	const VSFrameRef * cpSourceFrame = a_cpVSAPI->getFrameFilter(a_n,
		pInternalData->pInputNode.get(), a_pFrameCtx);
	const VSFrameRef * cpCompensatedFrame = a_cpVSAPI->getFrameFilter(a_n,
		pInternalData->pCompensatedNode.get(), a_pFrameCtx);

	VSFrameRef * pOutFrame = a_cpVSAPI->newVideoFrame(cpFormat, width, height,
		nullptr, a_pCore);

	uint8_t * pOutData = a_cpVSAPI->getWritePtr(pOutFrame, 0);
	int outStride = a_cpVSAPI->getStride(pOutFrame, 0);
	memset(pOutData, 0, (size_t)outStride * height);

	Sampler2D<float> correlationSampler{pOutData, outStride};

	for(int planeNumber = 0; planeNumber < planesCount; ++planeNumber)
	{
		PlaneWithCorrelation plane;
		plane.width = a_cpVSAPI->getFrameWidth(cpSourceFrame, planeNumber);
		plane.height = a_cpVSAPI->getFrameHeight(cpSourceFrame, planeNumber);
		plane.subSamplingW = (planeNumber == 0) ? 0 : cpFormat->subSamplingW;
		plane.subSamplingH = (planeNumber == 0) ? 0 : cpFormat->subSamplingH;
		plane.cpSourceBytes = a_cpVSAPI->getReadPtr(cpSourceFrame, planeNumber);
		plane.sourceStride = a_cpVSAPI->getStride(cpSourceFrame, planeNumber);
		plane.cpCompensatedBytes = a_cpVSAPI->getReadPtr(cpCompensatedFrame, planeNumber);
		plane.compensatedStride = a_cpVSAPI->getStride(cpCompensatedFrame, planeNumber);
		plane.correlation = std::vector<float>(plane.height * plane.width, 0.0f);

		auto calculatePlaneCorrelationType = [&](auto arg)
		{
			using T = std::decay_t<decltype(arg)>;
			calculatePlaneCorelation<T>(plane, pInternalData->radius);
		};

		if(cpSourceFormat->sampleType == stFloat)
		{
			if(cpSourceFormat->bytesPerSample == 4)
				calculatePlaneCorrelationType(float());
			else
				assert(false);
		}
		else
		{
			switch(cpSourceFormat->bytesPerSample)
			{
			case 1: calculatePlaneCorrelationType(uint8_t()); break;
			case 2: calculatePlaneCorrelationType(uint16_t()); break;
			case 4: calculatePlaneCorrelationType(uint32_t()); break;
			default: assert(false);
			}
		}

		ConstSampler2D<float> planeCorrSampler{plane.correlation.data(), plane.width * sizeof(float)};

		int subW = plane.subSamplingW;
		int subH = plane.subSamplingH;

		for(int h = 0; h < height; ++h)
			for(int w = 0; w < width; ++w)
				correlationSampler[h][w] += planeCorrSampler[h >> subH][w >> subW] / (float)planesCount;
	}

	a_cpVSAPI->freeFrame(cpSourceFrame);
	a_cpVSAPI->freeFrame(cpCompensatedFrame);

	return pOutFrame;
}

//==============================================================================
