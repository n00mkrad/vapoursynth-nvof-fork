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
#include "mvtools_helpers.h"

#undef min
#undef max

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

//==============================================================================

//==============================================================================
// Filter internal data structure

struct DataToMVTools
{
    PtrNodeRef pFlowNode{nullptr};
    PtrNodeRef pSourceNode{nullptr};
    const VSVideoInfo * cpFlowVideoInfo{nullptr};
    const VSVideoInfo * cpSourceVideoInfo{nullptr};
    VSVideoInfo videoInfo{};
    mvtools::Config mvtoolsConfig{};
    int64_t delta{0};
};

//==============================================================================
// Forward declarations

InitVSNodeFun initToMVTools;
GetFrameFun getFrameToMVTools;
FreeVSNodeFun freeToMVTools;

//==============================================================================

int64_t getOptionalInt(const VSMap * a_pIn, const char * a_cpName,
    int64_t a_defaultValue, const VSAPI * a_cpVSAPI)
{
    int error = 0;
    int64_t value = a_cpVSAPI->propGetInt(a_pIn, a_cpName, 0, &error);
    return (error == 0) ? value : a_defaultValue;
}

//==============================================================================

bool validateIntRange(int64_t a_value, const char * a_cpName,
    VSMap * a_pOut, const VSAPI * a_cpVSAPI)
{
    if((a_value >= std::numeric_limits<int>::min()) &&
        (a_value <= std::numeric_limits<int>::max()))
        return true;

    a_cpVSAPI->setError(a_pOut, (std::string("nvof.ToMvTools: ") +
        a_cpName + " is outside the supported integer range.").c_str());
    return false;
}

//==============================================================================

std::vector<uint8_t> makeVectorBlob(const DataToMVTools * a_cpData,
    const VSFrameRef * a_cpFlowFrame, bool a_valid, const VSAPI * a_cpVSAPI)
{
    const uint8_t * cpCostPlane = a_cpVSAPI->getReadPtr(a_cpFlowFrame, PLANE_COST);
    const uint8_t * cpVecXPlane = a_cpVSAPI->getReadPtr(a_cpFlowFrame, PLANE_VEC_X);
    const uint8_t * cpVecYPlane = a_cpVSAPI->getReadPtr(a_cpFlowFrame, PLANE_VEC_Y);
    const int costStride = a_cpVSAPI->getStride(a_cpFlowFrame, PLANE_COST);
    const int vecXStride = a_cpVSAPI->getStride(a_cpFlowFrame, PLANE_VEC_X);
    const int vecYStride = a_cpVSAPI->getStride(a_cpFlowFrame, PLANE_VEC_Y);

    return mvtools::makeVectorBlob(a_cpData->mvtoolsConfig, a_valid,
        [=](int a_x, int a_y) -> mvtools::Sample
        {
            const float * cpCostRow = (const float *)(cpCostPlane + a_y * costStride);
            const float * cpVecXRow = (const float *)(cpVecXPlane + a_y * vecXStride);
            const float * cpVecYRow = (const float *)(cpVecYPlane + a_y * vecYStride);
            return mvtools::Sample{cpVecXRow[a_x], cpVecYRow[a_x], cpCostRow[a_x]};
        });
}

//==============================================================================

void VS_CC createToMVTools(const VSMap * a_pIn, VSMap * a_pOut,
    void * a_pUserData, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
    UNUSED(a_pUserData);

    DataToMVTools internalData;

    internalData.pFlowNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "flow", 0, 0),
        FreeNodeRef{a_cpVSAPI});
    internalData.pSourceNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "clip", 0, 0),
        FreeNodeRef{a_cpVSAPI});
    internalData.cpFlowVideoInfo = a_cpVSAPI->getVideoInfo(internalData.pFlowNode.get());
    internalData.cpSourceVideoInfo = a_cpVSAPI->getVideoInfo(internalData.pSourceNode.get());

    //--------------------------------------------------------------------------
    // Format check

    const VSFormat * cpFlowFormat = internalData.cpFlowVideoInfo->format;
    bool acceptableFormat = isConstantFormat(internalData.cpFlowVideoInfo) &&
        (internalData.cpFlowVideoInfo->numFrames != 0) &&
        (cpFlowFormat == a_cpVSAPI->getFormatPreset(pfYUV444PS, a_pCore));
    if(!acceptableFormat)
    {
        a_cpVSAPI->setError(a_pOut, "nvof.ToMvTools: "
            "flow clip format must be the NVOF YUV444PS flow format.");
        return;
    }

    const VSFormat * cpSourceFormat = internalData.cpSourceVideoInfo->format;
    acceptableFormat = isConstantFormat(internalData.cpSourceVideoInfo) &&
        (internalData.cpSourceVideoInfo->numFrames != 0) &&
        (cpSourceFormat->sampleType == stInteger) &&
        (cpSourceFormat->bitsPerSample > 0) &&
        (cpSourceFormat->bitsPerSample <= 16);
    if(!acceptableFormat)
    {
        a_cpVSAPI->setError(a_pOut, "nvof.ToMvTools: "
            "source clip must have constant integer format with 1 to 16 bits per sample.");
        return;
    }

    bool match = (internalData.cpSourceVideoInfo->width == internalData.cpFlowVideoInfo->width) &&
        (internalData.cpSourceVideoInfo->height == internalData.cpFlowVideoInfo->height) &&
        (internalData.cpSourceVideoInfo->numFrames == internalData.cpFlowVideoInfo->numFrames);
    if(!match)
    {
        a_cpVSAPI->setError(a_pOut, "nvof.ToMvTools: "
            "source clip and flow clip must match in size and length.");
        return;
    }

    //--------------------------------------------------------------------------
    // delta

    int error = 0;
    internalData.delta = a_cpVSAPI->propGetInt(a_pIn, "delta", 0, &error);
    if(error != 0)
    {
        a_cpVSAPI->setError(a_pOut, "nvof.ToMvTools: "
            "failed to initialize the \"delta\" argument.");
        return;
    }

    if((internalData.delta == 0) ||
        (internalData.delta < -(int64_t)std::numeric_limits<int>::max()) ||
        (internalData.delta > (int64_t)std::numeric_limits<int>::max()))
    {
        a_cpVSAPI->setError(a_pOut, "nvof.ToMvTools: "
            "delta must be non-zero and fit in a positive MVTools delta frame.");
        return;
    }

    //--------------------------------------------------------------------------
    // block geometry

    int64_t blockSizeX64 = getOptionalInt(a_pIn, "block_size", 8, a_cpVSAPI);
    int64_t blockSizeY64 = getOptionalInt(a_pIn, "block_size_v", blockSizeX64, a_cpVSAPI);
    int64_t overlapX64 = getOptionalInt(a_pIn, "overlap", 0, a_cpVSAPI);
    int64_t overlapY64 = getOptionalInt(a_pIn, "overlap_v", overlapX64, a_cpVSAPI);
    int64_t pel64 = getOptionalInt(a_pIn, "pel", 1, a_cpVSAPI);
    int64_t fallbackSAD = getOptionalInt(a_pIn, "fallback_sad", -1, a_cpVSAPI);
    int64_t clippedSAD = getOptionalInt(a_pIn, "clipped_sad", -1, a_cpVSAPI);

    if(!validateIntRange(blockSizeX64, "block_size", a_pOut, a_cpVSAPI) ||
        !validateIntRange(blockSizeY64, "block_size_v", a_pOut, a_cpVSAPI) ||
        !validateIntRange(overlapX64, "overlap", a_pOut, a_cpVSAPI) ||
        !validateIntRange(overlapY64, "overlap_v", a_pOut, a_cpVSAPI) ||
        !validateIntRange(pel64, "pel", a_pOut, a_cpVSAPI))
        return;

    const int blockSizeX = (int)blockSizeX64;
    const int blockSizeY = (int)blockSizeY64;
    const int overlapX = (int)overlapX64;
    const int overlapY = (int)overlapY64;
    const int pel = (int)pel64;
    if((blockSizeX <= 0) || (blockSizeY <= 0) ||
        (overlapX < 0) || (overlapY < 0) ||
        (overlapX >= blockSizeX) || (overlapY >= blockSizeY))
    {
        a_cpVSAPI->setError(a_pOut, "nvof.ToMvTools: "
            "block sizes must be positive and overlap must be non-negative and smaller than block size.");
        return;
    }

    if((pel != 1) && (pel != 2) && (pel != 4))
    {
        a_cpVSAPI->setError(a_pOut, "nvof.ToMvTools: "
            "pel must be 1, 2, or 4.");
        return;
    }

    //--------------------------------------------------------------------------
    // MVTools metadata

    mvtools::AnalysisData analysisData{};
    std::string mvtoolsError;
    const int metadataDelta = (int)internalData.delta;
    if(!mvtools::buildAnalysisData(analysisData, *internalData.cpSourceVideoInfo,
        metadataDelta, blockSizeX, blockSizeY, overlapX, overlapY, pel,
        mvtoolsError))
    {
        a_cpVSAPI->setError(a_pOut, (std::string("nvof.ToMvTools: ") +
            mvtoolsError).c_str());
        return;
    }

    if(!mvtools::buildConfig(internalData.mvtoolsConfig, analysisData,
        fallbackSAD, clippedSAD, mvtoolsError))
    {
        a_cpVSAPI->setError(a_pOut, (std::string("nvof.ToMvTools: ") +
            mvtoolsError).c_str());
        return;
    }

    //--------------------------------------------------------------------------
    // output video info

    internalData.videoInfo = *internalData.cpSourceVideoInfo;
    internalData.videoInfo.format = a_cpVSAPI->getFormatPreset(pfGray8, a_pCore);

    //--------------------------------------------------------------------------

    DataToMVTools * pInternalData = new DataToMVTools;
    *pInternalData = std::move(internalData);
    a_cpVSAPI->createFilter(a_pIn, a_pOut, "ToMvTools", initToMVTools,
        getFrameToMVTools, freeToMVTools, fmParallel, 0, pInternalData, a_pCore);
}

//==============================================================================

void VS_CC initToMVTools(VSMap * a_pIn, VSMap * a_pOut,
    void ** a_ppInstanceData, VSNode * a_pNode, VSCore * a_pCore,
    const VSAPI * a_cpVSAPI)
{
    UNUSED(a_pIn);
    UNUSED(a_pOut);
    UNUSED(a_pCore);

    DataToMVTools * pInternalData = (DataToMVTools *) *a_ppInstanceData;
    a_cpVSAPI->setVideoInfo(&pInternalData->videoInfo, 1, a_pNode);
}

//==============================================================================

// Free all allocated data on filter destruction
void VS_CC freeToMVTools(void * a_pInstanceData, VSCore * a_pCore,
    const VSAPI * a_cpVSAPI)
{
    UNUSED(a_pCore);
    UNUSED(a_cpVSAPI);

    DataToMVTools * pInternalData = (DataToMVTools *)a_pInstanceData;
    delete pInternalData;
}

//==============================================================================

const VSFrameRef * VS_CC getFrameToMVTools(int a_n, int a_activationReason,
    void ** a_ppInstanceData, void ** a_ppFrameData,
    VSFrameContext * a_pFrameCtx, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
    UNUSED(a_ppFrameData);

    DataToMVTools * pInternalData = (DataToMVTools *) *a_ppInstanceData;
    int framesNumber = pInternalData->cpSourceVideoInfo->numFrames;

    a_n = clamp(a_n, 0, framesNumber - 1);
    int64_t referenceFrame = (int64_t)a_n + pInternalData->delta;
    bool valid = (referenceFrame >= 0) && (referenceFrame < framesNumber);

    if(a_activationReason == arInitial)
    {
        a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pFlowNode.get(), a_pFrameCtx);
        return nullptr;
    }
    else if(a_activationReason != arAllFramesReady)
        return nullptr;

    const VSFrameRef * cpFlowFrame = a_cpVSAPI->getFrameFilter(a_n,
        pInternalData->pFlowNode.get(), a_pFrameCtx);

    VSFrameRef * pOutFrame = a_cpVSAPI->newVideoFrame(pInternalData->videoInfo.format,
        pInternalData->videoInfo.width, pInternalData->videoInfo.height,
        cpFlowFrame, a_pCore);

    uint8_t * pDstRow = a_cpVSAPI->getWritePtr(pOutFrame, PLANE_Y);
    const int dstStride = a_cpVSAPI->getStride(pOutFrame, PLANE_Y);
    memset(pDstRow, 0, (size_t)dstStride * pInternalData->videoInfo.height);

    std::vector<uint8_t> vectorBlob = makeVectorBlob(pInternalData, cpFlowFrame,
        valid, a_cpVSAPI);

    bool success = mvtools::writeDataProp(pOutFrame, mvtools::PROP_ANALYSIS_DATA,
        &pInternalData->mvtoolsConfig.analysisData,
        sizeof(pInternalData->mvtoolsConfig.analysisData), a_cpVSAPI) &&
        mvtools::writeDataProp(pOutFrame, mvtools::PROP_VECTORS, vectorBlob.data(),
            vectorBlob.size(), a_cpVSAPI);
    if(!success)
    {
        a_cpVSAPI->setFilterError("nvof.ToMvTools: "
            "failed to write MVTools frame properties.", a_pFrameCtx);
        a_cpVSAPI->freeFrame(cpFlowFrame);
        a_cpVSAPI->freeFrame(pOutFrame);
        return nullptr;
    }

    a_cpVSAPI->freeFrame(cpFlowFrame);

    return pOutFrame;
}

//==============================================================================
