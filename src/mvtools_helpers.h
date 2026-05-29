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

#ifndef MVTOOLS_HELPERS_H
#define MVTOOLS_HELPERS_H

#include "common.h"

#undef min
#undef max

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

//==============================================================================

namespace mvtools
{

static const char PROP_ANALYSIS_DATA[] = "MVTools_MVAnalysisData";
static const char PROP_VECTORS[] = "MVTools_vectors";

static const int ANALYSIS_DATA_VERSION = 5;
static const int MOTION_IS_BACKWARD = 0x00000002;

typedef int MVArraySizeType;

struct Vector
{
    int x;
    int y;
    int64_t sad;
};

struct AnalysisData
{
    int nMagicKey;
    int nVersion;
    int nBlkSizeX;
    int nBlkSizeY;
    int nPel;
    int nLvCount;
    int nDeltaFrame;
    int isBackward;
    int nCPUFlags;
    int nMotionFlags;
    int nWidth;
    int nHeight;
    int nOverlapX;
    int nOverlapY;
    int nBlkX;
    int nBlkY;
    int bitsPerSample;
    int yRatioUV;
    int xRatioUV;
    int nHPadding;
    int nVPadding;
};

static_assert(sizeof(Vector) == 16, "MVTools VECTOR layout changed.");
static_assert(sizeof(AnalysisData) == 84, "MVTools MVAnalysisData layout changed.");

struct Sample
{
    double x{0.0};
    double y{0.0};
    double cost{0.0};
};

struct Config
{
    AnalysisData analysisData{};
    int pixelMax{0};
    int64_t veryBigSAD{0};
    int64_t fallbackSAD{-1};
    int64_t clippedSAD{-1};
    size_t blockCount{0};
    size_t planeSize{0};
    size_t groupSize{0};
};

bool buildAnalysisData(AnalysisData & a_data, const VSVideoInfo & a_sourceVideoInfo,
    int a_delta, int a_blockSizeX, int a_blockSizeY, int a_overlapX,
    int a_overlapY, int a_pel, std::string & a_errorString);
bool buildConfig(Config & a_config, const AnalysisData & a_analysisData,
    int64_t a_fallbackSAD, int64_t a_clippedSAD, std::string & a_errorString);
bool writeDataProp(VSFrameRef * a_pFrame, const char * a_cpName,
    const void * a_cpData, size_t a_size, const VSAPI * a_cpVSAPI);
int64_t effectiveClippedSAD(const Config & a_config);

//==============================================================================

inline int roundToInt(double a_value, bool & a_sanitized)
{
    if(!std::isfinite(a_value))
    {
        a_sanitized = true;
        return 0;
    }
    if(a_value <= (double)std::numeric_limits<int>::min())
    {
        a_sanitized = true;
        return std::numeric_limits<int>::min();
    }
    if(a_value >= (double)std::numeric_limits<int>::max())
    {
        a_sanitized = true;
        return std::numeric_limits<int>::max();
    }
    return (int)std::llround(a_value);
}

//==============================================================================

inline int64_t synthesizeSAD(double a_averageCost, const Config & a_config)
{
    const int blockSizeX = a_config.analysisData.nBlkSizeX;
    const int blockSizeY = a_config.analysisData.nBlkSizeY;

    if(!std::isfinite(a_averageCost) || (a_averageCost <= 0.0))
    {
        if(a_config.fallbackSAD >= 0)
            return a_config.fallbackSAD;
        return (int64_t)std::llround((double)blockSizeX * (double)blockSizeY *
            (double)a_config.pixelMax / 32.0);
    }

    double cost = clamp(a_averageCost, 0.0, 1.0);
    double sad = cost * (double)blockSizeX * (double)blockSizeY *
        (double)a_config.pixelMax;
    if(sad >= (double)std::numeric_limits<int64_t>::max())
        return std::numeric_limits<int64_t>::max();
    return (int64_t)std::llround(sad);
}

//==============================================================================

inline void writeArraySize(std::vector<uint8_t> & a_blob, size_t a_offset,
    MVArraySizeType a_value)
{
    memcpy(a_blob.data() + a_offset, &a_value, sizeof(a_value));
}

//==============================================================================

inline void writeVector(std::vector<uint8_t> & a_blob, size_t a_baseOffset,
    size_t a_index, const Vector & a_vector)
{
    memcpy(a_blob.data() + a_baseOffset + a_index * sizeof(a_vector),
        &a_vector, sizeof(a_vector));
}

//==============================================================================

inline int clipVectorValue(int a_value, int64_t a_minValue, int64_t a_maxValue,
    bool & a_clipped)
{
    const int64_t original = a_value;
    const int64_t clipped = clamp(original, a_minValue, a_maxValue);
    if(clipped != original)
        a_clipped = true;
    return (int)clipped;
}

//==============================================================================

inline Vector finalizeVector(double a_sumX, double a_sumY, double a_sumCost,
    double a_blockArea, int a_blockX, int a_blockY, const Config & a_config)
{
    bool sanitized = false;
    bool clipped = false;
    const int pel = a_config.analysisData.nPel;
    const int blockSizeX = a_config.analysisData.nBlkSizeX;
    const int blockSizeY = a_config.analysisData.nBlkSizeY;

    Vector vector;
    vector.x = roundToInt((a_sumX / a_blockArea) * (double)pel, sanitized);
    vector.y = roundToInt((a_sumY / a_blockArea) * (double)pel, sanitized);

    const int64_t minX = -(int64_t)a_blockX * pel;
    const int64_t maxX = ((int64_t)a_config.analysisData.nWidth - blockSizeX - a_blockX) * pel;
    const int64_t minY = -(int64_t)a_blockY * pel;
    const int64_t maxY = ((int64_t)a_config.analysisData.nHeight - blockSizeY - a_blockY) * pel;
    vector.x = clipVectorValue(vector.x, minX, maxX, clipped);
    vector.y = clipVectorValue(vector.y, minY, maxY, clipped);

    vector.sad = synthesizeSAD(a_sumCost / a_blockArea, a_config);
    if(sanitized || clipped)
        vector.sad = std::max(vector.sad, effectiveClippedSAD(a_config));
    return vector;
}

//==============================================================================

template<typename Sampler>
std::vector<uint8_t> makeVectorBlob(const Config & a_config, bool a_valid,
    Sampler a_sampler)
{
    const MVArraySizeType groupSize = (MVArraySizeType)a_config.groupSize;
    const MVArraySizeType validity = a_valid ? 1 : 0;
    const MVArraySizeType planeSize = (MVArraySizeType)a_config.planeSize;

    std::vector<uint8_t> blob(a_config.groupSize, 0);
    writeArraySize(blob, 0, groupSize);
    writeArraySize(blob, sizeof(MVArraySizeType), validity);
    writeArraySize(blob, 2 * sizeof(MVArraySizeType), planeSize);

    const size_t vectorsOffset = 3 * sizeof(MVArraySizeType);
    if(!a_valid)
    {
        Vector vector{0, 0, a_config.veryBigSAD};
        for(size_t i = 0; i < a_config.blockCount; ++i)
            writeVector(blob, vectorsOffset, i, vector);
        return blob;
    }

    const int blockSizeX = a_config.analysisData.nBlkSizeX;
    const int blockSizeY = a_config.analysisData.nBlkSizeY;
    const int stepX = blockSizeX - a_config.analysisData.nOverlapX;
    const int stepY = blockSizeY - a_config.analysisData.nOverlapY;
    const double blockArea = (double)blockSizeX * (double)blockSizeY;

    size_t vectorIndex = 0;
    for(int by = 0; by < a_config.analysisData.nBlkY; ++by)
    {
        const int blockY = by * stepY;
        for(int bx = 0; bx < a_config.analysisData.nBlkX; ++bx)
        {
            const int blockX = bx * stepX;
            double sumX = 0.0;
            double sumY = 0.0;
            double sumCost = 0.0;

            for(int y = 0; y < blockSizeY; ++y)
            {
                for(int x = 0; x < blockSizeX; ++x)
                {
                    Sample sample = a_sampler(blockX + x, blockY + y);
                    sumX += sample.x;
                    sumY += sample.y;
                    sumCost += sample.cost;
                }
            }

            writeVector(blob, vectorsOffset, vectorIndex,
                finalizeVector(sumX, sumY, sumCost, blockArea, blockX,
                    blockY, a_config));
            vectorIndex++;
        }
    }

    return blob;
}

//==============================================================================

} // namespace mvtools

#endif
