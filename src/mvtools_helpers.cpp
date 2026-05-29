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

#include "mvtools_helpers.h"

#include <algorithm>

//==============================================================================

namespace mvtools
{

bool buildAnalysisData(AnalysisData & a_data, const VSVideoInfo & a_sourceVideoInfo,
    int a_delta, int a_blockSizeX, int a_blockSizeY, int a_overlapX,
    int a_overlapY, int a_pel, std::string & a_errorString)
{
    const VSFormat * cpSourceFormat = a_sourceVideoInfo.format;
    const int stepX = a_blockSizeX - a_overlapX;
    const int stepY = a_blockSizeY - a_overlapY;
    const int nBlkX = (a_sourceVideoInfo.width - a_overlapX) / stepX;
    const int nBlkY = (a_sourceVideoInfo.height - a_overlapY) / stepY;
    if((nBlkX <= 0) || (nBlkY <= 0))
    {
        a_errorString = "block geometry does not produce any complete blocks.";
        return false;
    }

    const VSColorFamily sourceColorFamily = (VSColorFamily)cpSourceFormat->colorFamily;
    int xRatioUV = 1;
    int yRatioUV = 1;
    if((sourceColorFamily == cmYUV) || (sourceColorFamily == cmYCoCg))
    {
        if((cpSourceFormat->subSamplingW > 1) || (cpSourceFormat->subSamplingH > 1))
        {
            a_errorString = "MVTools-compatible metadata only supports up to 4:2:0 chroma subsampling.";
            return false;
        }
        xRatioUV = 1 << cpSourceFormat->subSamplingW;
        yRatioUV = 1 << cpSourceFormat->subSamplingH;
    }

    const bool isBackward = a_delta > 0;
    a_data = AnalysisData{};
    a_data.nMagicKey = 0;
    a_data.nVersion = ANALYSIS_DATA_VERSION;
    a_data.nBlkSizeX = a_blockSizeX;
    a_data.nBlkSizeY = a_blockSizeY;
    a_data.nPel = a_pel;
    a_data.nLvCount = 1;
    a_data.nDeltaFrame = (int)(isBackward ? a_delta : -a_delta);
    a_data.isBackward = isBackward ? 1 : 0;
    a_data.nCPUFlags = 0;
    a_data.nMotionFlags = isBackward ? MOTION_IS_BACKWARD : 0;
    a_data.nWidth = a_sourceVideoInfo.width;
    a_data.nHeight = a_sourceVideoInfo.height;
    a_data.nOverlapX = a_overlapX;
    a_data.nOverlapY = a_overlapY;
    a_data.nBlkX = nBlkX;
    a_data.nBlkY = nBlkY;
    a_data.bitsPerSample = cpSourceFormat->bitsPerSample;
    a_data.yRatioUV = yRatioUV;
    a_data.xRatioUV = xRatioUV;
    a_data.nHPadding = 0;
    a_data.nVPadding = 0;
    return true;
}

//==============================================================================

bool buildConfig(Config & a_config, const AnalysisData & a_analysisData,
    int64_t a_fallbackSAD, int64_t a_clippedSAD, std::string & a_errorString)
{
    a_config = Config{};
    a_config.analysisData = a_analysisData;
    a_config.pixelMax = (1 << a_analysisData.bitsPerSample) - 1;
    a_config.veryBigSAD = (int64_t)a_analysisData.nBlkSizeX *
        a_analysisData.nBlkSizeY * ((int64_t)1 << a_analysisData.bitsPerSample);
    a_config.fallbackSAD = a_fallbackSAD;
    a_config.clippedSAD = a_clippedSAD;
    a_config.blockCount = (size_t)a_analysisData.nBlkX * (size_t)a_analysisData.nBlkY;
    a_config.planeSize = sizeof(MVArraySizeType) +
        a_config.blockCount * sizeof(Vector);
    a_config.groupSize = 2 * sizeof(MVArraySizeType) + a_config.planeSize;

    if((a_config.groupSize > (size_t)std::numeric_limits<MVArraySizeType>::max()) ||
        (a_config.planeSize > (size_t)std::numeric_limits<MVArraySizeType>::max()))
    {
        a_errorString = "serialized MVTools vector data is too large.";
        return false;
    }

    return true;
}

//==============================================================================

bool writeDataProp(VSFrameRef * a_pFrame, const char * a_cpName,
    const void * a_cpData, size_t a_size, const VSAPI * a_cpVSAPI)
{
    if(a_size > (size_t)std::numeric_limits<int>::max())
        return false;

    VSMap * pProps = a_cpVSAPI->getFramePropsRW(a_pFrame);
    return a_cpVSAPI->propSetData(pProps, a_cpName, (const char *)a_cpData,
        (int)a_size, paReplace) == 0;
}

//==============================================================================

int64_t effectiveClippedSAD(const Config & a_config)
{
    return (a_config.clippedSAD >= 0) ? a_config.clippedSAD : a_config.veryBigSAD;
}

//==============================================================================

} // namespace mvtools
