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
#include "nvidia-optical-flow.h"

#include <map>
#include <cstring>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

//==============================================================================

struct PairKey
{
    int first{0};
    int second{0};

    bool operator<(const PairKey & a_other) const
    {
        return (first < a_other.first) ||
            ((first == a_other.first) && (second < a_other.second));
    }
};

struct PairBlobs
{
    std::vector<uint8_t> forward;
    std::vector<uint8_t> backward;
};

class NVidiaOpticalFlowDataWorker
{
public:
    NVidiaOpticalFlowDataWorker(const VSAPI * a_cpVSAPI,
        const VSVideoInfo & a_inputVideoInfo, bool a_getCost, int a_gpu,
        bool a_bidirectional):
          m_cpVSAPI(a_cpVSAPI)
        , m_inputVideoInfo(a_inputVideoInfo)
        , m_getCost(a_getCost)
        , m_gpu(a_gpu)
        , m_bidirectional(a_bidirectional)
    {
        m_thread = std::thread(&NVidiaOpticalFlowDataWorker::threadMain, this);
    }

    ~NVidiaOpticalFlowDataWorker()
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_stop = true;
        }
        m_condition.notify_one();
        if(m_thread.joinable())
            m_thread.join();
    }

    bool getFlowData(const VSFrameRef * a_cpFirstFrame,
        const VSFrameRef * a_cpSecondFrame, NVOFFlowData & a_forwardData,
        NVOFFlowData * a_pBackwardData, std::string & a_errorString)
    {
        std::lock_guard<std::mutex> requestLock(m_requestLock);
        std::unique_lock<std::mutex> lock(m_lock);
        m_cpFirstFrame = a_cpFirstFrame;
        m_cpSecondFrame = a_cpSecondFrame;
        m_backwardRequested = a_pBackwardData != nullptr;
        m_forwardData = NVOFFlowData{};
        m_backwardData = NVOFFlowData{};
        m_success = false;
        m_errorString.clear();
        m_responseReady = false;
        m_requestPending = true;
        m_condition.notify_one();
        m_condition.wait(lock, [this] { return m_responseReady; });
        a_forwardData = std::move(m_forwardData);
        if(a_pBackwardData)
            *a_pBackwardData = std::move(m_backwardData);
        a_errorString = m_errorString;
        return m_success;
    }
private:
    void threadMain()
    {
        std::unique_ptr<NVidiaOpticalFlow> pFlow(new NVidiaOpticalFlow(m_cpVSAPI,
            m_inputVideoInfo, true, NV_OF_PERF_LEVEL_SLOW, m_getCost, m_gpu,
            m_bidirectional));
        while(true)
        {
            std::unique_lock<std::mutex> lock(m_lock);
            m_condition.wait(lock, [this] { return m_requestPending || m_stop; });
            if(m_stop && !m_requestPending)
                break;

            const VSFrameRef * cpFirstFrame = m_cpFirstFrame;
            const VSFrameRef * cpSecondFrame = m_cpSecondFrame;
            const bool backwardRequested = m_backwardRequested;
            m_requestPending = false;
            lock.unlock();

            NVOFFlowData forwardData;
            NVOFFlowData backwardData;
            std::string errorString;
            bool success = false;
            {
                std::lock_guard<std::mutex> nvofLock(nvofGlobalLock());
                success = pFlow->init(errorString);
                if(success)
                    success = pFlow->getFlowData(cpFirstFrame, cpSecondFrame,
                        forwardData, backwardRequested ? &backwardData : nullptr,
                        errorString);
                else
                    errorString = std::string("failed to initialize the flow calculator. ") +
                        errorString;
            }

            lock.lock();
            m_forwardData = std::move(forwardData);
            m_backwardData = std::move(backwardData);
            m_success = success;
            m_errorString = errorString;
            m_responseReady = true;
            lock.unlock();
            m_condition.notify_one();
        }

        std::lock_guard<std::mutex> nvofLock(nvofGlobalLock());
        pFlow.reset();
    }

    const VSAPI * m_cpVSAPI{nullptr};
    VSVideoInfo m_inputVideoInfo{};
    bool m_getCost{false};
    int m_gpu{0};
    bool m_bidirectional{false};
    std::thread m_thread;
    std::mutex m_requestLock;
    std::mutex m_lock;
    std::condition_variable m_condition;
    const VSFrameRef * m_cpFirstFrame{nullptr};
    const VSFrameRef * m_cpSecondFrame{nullptr};
    bool m_backwardRequested{false};
    NVOFFlowData m_forwardData;
    NVOFFlowData m_backwardData;
    bool m_requestPending{false};
    bool m_responseReady{false};
    bool m_stop{false};
    bool m_success{false};
    std::string m_errorString;
};

class SharedGetMVTools
{
public:
    SharedGetMVTools(const VSAPI * a_cpVSAPI, const VSVideoInfo & a_inputVideoInfo,
        bool a_getCost, int a_gpu, int a_tr, bool a_bidirectional):
          m_cpVSAPI(a_cpVSAPI)
        , m_inputVideoInfo(a_inputVideoInfo)
        , m_getCost(a_getCost)
        , m_gpu(a_gpu)
        , m_tr(a_tr)
        , m_bidirectional(a_bidirectional)
    {
    }

    bool getPairBlobs(int a_firstFrame, int a_secondFrame,
        const VSFrameRef * a_cpFirstFrame, const VSFrameRef * a_cpSecondFrame,
        const mvtools::Config & a_forwardConfig,
        const mvtools::Config & a_backwardConfig, PairBlobs & a_blobs,
        std::string & a_errorString)
    {
        const PairKey key{a_firstFrame, a_secondFrame};
        {
            std::lock_guard<std::mutex> lock(m_lock);
            std::map<PairKey, PairBlobs>::const_iterator it = m_cache.find(key);
            if(it != m_cache.end())
            {
                a_blobs = it->second;
                return true;
            }
        }

        NVidiaOpticalFlowDataWorker * pFlowWorker = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if(!m_pFlowWorker)
                m_pFlowWorker.reset(new NVidiaOpticalFlowDataWorker(m_cpVSAPI,
                    m_inputVideoInfo, m_getCost, m_gpu, true));
            pFlowWorker = m_pFlowWorker.get();
        }

        NVOFFlowData forwardData;
        NVOFFlowData backwardData;
        if(!pFlowWorker->getFlowData(a_cpFirstFrame, a_cpSecondFrame,
            forwardData, &backwardData, a_errorString))
            return false;

        PairBlobs newBlobs;
        newBlobs.forward = makeNVOFVectorBlob(a_forwardConfig, forwardData);
        newBlobs.backward = makeNVOFVectorBlob(a_backwardConfig, backwardData);
        a_blobs = newBlobs;

        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_cache[key] = std::move(newBlobs);

            const size_t maxCachedPairs = (size_t)m_tr * 8 + 8;
            while(m_cache.size() > maxCachedPairs)
                m_cache.erase(m_cache.begin());
        }

        return true;
    }

    bool getForwardBlob(int a_currentFrame, int a_referenceFrame,
        const VSFrameRef * a_cpCurrentFrame, const VSFrameRef * a_cpReferenceFrame,
        const mvtools::Config & a_config, std::vector<uint8_t> & a_blob,
        std::string & a_errorString)
    {
        const PairKey key{a_currentFrame, a_referenceFrame};
        {
            std::lock_guard<std::mutex> lock(m_lock);
            std::map<PairKey, std::vector<uint8_t>>::const_iterator it =
                m_forwardCache.find(key);
            if(it != m_forwardCache.end())
            {
                a_blob = it->second;
                return true;
            }
        }

        NVidiaOpticalFlowDataWorker * pFlowWorker = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if(!m_pFlowWorker)
                m_pFlowWorker.reset(new NVidiaOpticalFlowDataWorker(m_cpVSAPI,
                    m_inputVideoInfo, m_getCost, m_gpu, false));
            pFlowWorker = m_pFlowWorker.get();
        }

        NVOFFlowData forwardData;
        if(!pFlowWorker->getFlowData(a_cpCurrentFrame, a_cpReferenceFrame,
            forwardData, nullptr, a_errorString))
            return false;

        a_blob = makeNVOFVectorBlob(a_config, forwardData);

        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_forwardCache[key] = a_blob;

            const size_t maxCachedPairs = (size_t)m_tr * 16 + 16;
            while(m_forwardCache.size() > maxCachedPairs)
                m_forwardCache.erase(m_forwardCache.begin());
        }

        return true;
    }

    bool bidirectional() const
    {
        return m_bidirectional;
    }
private:
    std::vector<uint8_t> makeNVOFVectorBlob(const mvtools::Config & a_config,
        const NVOFFlowData & a_flowData) const
    {
        const size_t width = (size_t)m_inputVideoInfo.width;
        const float vectorScale = 1.0f / 32.0f;
        const float costScale = 1.0f / 255.0f;
        return mvtools::makeVectorBlob(a_config, true,
            [&](int a_x, int a_y) -> mvtools::Sample
            {
                const size_t index = (size_t)a_y * width + (size_t)a_x;
                const NV_OF_FLOW_VECTOR & vector = a_flowData.flow[index];
                return mvtools::Sample{
                    (double)((float)vector.flowx * vectorScale),
                    (double)((float)vector.flowy * vectorScale),
                    (double)((float)a_flowData.cost[index] * costScale)};
            });
    }

    const VSAPI * m_cpVSAPI{nullptr};
    VSVideoInfo m_inputVideoInfo{};
    bool m_getCost{false};
    int m_gpu{0};
    int m_tr{1};
    bool m_bidirectional{false};
    std::unique_ptr<NVidiaOpticalFlowDataWorker> m_pFlowWorker;
    std::map<PairKey, PairBlobs> m_cache;
    std::map<PairKey, std::vector<uint8_t>> m_forwardCache;
    std::mutex m_lock;
};

//==============================================================================
// Filter internal data structure

struct DataGetMVTools
{
    PtrNodeRef pInputNode{nullptr};
    PtrNodeRef pSourceNode{nullptr};
    const VSVideoInfo * cpInputVideoInfo{nullptr};
    const VSVideoInfo * cpSourceVideoInfo{nullptr};
    VSVideoInfo videoInfo{};
    std::shared_ptr<SharedGetMVTools> pShared;
    std::vector<mvtools::Config> mvtoolsConfigs;
    int outputIndex{0};
    int tr{1};
};

//==============================================================================
// Forward declarations

InitVSNodeFun initGetMVTools;
GetFrameFun getFrameGetMVTools;
FreeVSNodeFun freeGetMVTools;

//==============================================================================

int64_t getOptionalIntGetMVTools(const VSMap * a_pIn, const char * a_cpName,
    int64_t a_defaultValue, const VSAPI * a_cpVSAPI)
{
    int error = 0;
    int64_t value = a_cpVSAPI->propGetInt(a_pIn, a_cpName, 0, &error);
    return (error == 0) ? value : a_defaultValue;
}

//==============================================================================

bool validateIntRangeGetMVTools(int64_t a_value, const char * a_cpName,
    VSMap * a_pOut, const VSAPI * a_cpVSAPI)
{
    if((a_value >= std::numeric_limits<int>::min()) &&
        (a_value <= std::numeric_limits<int>::max()))
        return true;

    a_cpVSAPI->setError(a_pOut, (std::string("nvof.GetMvTools: ") +
        a_cpName + " is outside the supported integer range.").c_str());
    return false;
}

//==============================================================================

bool validateGeometryGetMVTools(int a_blockSizeX, int a_blockSizeY,
    int a_overlapX, int a_overlapY, int a_pel, VSMap * a_pOut,
    const VSAPI * a_cpVSAPI)
{
    if((a_blockSizeX <= 0) || (a_blockSizeY <= 0) ||
        (a_overlapX < 0) || (a_overlapY < 0) ||
        (a_overlapX >= a_blockSizeX) || (a_overlapY >= a_blockSizeY))
    {
        a_cpVSAPI->setError(a_pOut, "nvof.GetMvTools: "
            "block sizes must be positive and overlap must be non-negative and smaller than block size.");
        return false;
    }

    if((a_pel != 1) && (a_pel != 2) && (a_pel != 4))
    {
        a_cpVSAPI->setError(a_pOut, "nvof.GetMvTools: "
            "pel must be 1, 2, or 4.");
        return false;
    }

    return true;
}

//==============================================================================

void VS_CC createGetMVTools(const VSMap * a_pIn, VSMap * a_pOut,
    void * a_pUserData, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
    UNUSED(a_pUserData);

    std::unique_ptr<DataGetMVTools> pTemplateData(new DataGetMVTools);
    pTemplateData->pInputNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "clip", 0, 0), FreeNodeRef{a_cpVSAPI});
    pTemplateData->cpInputVideoInfo = a_cpVSAPI->getVideoInfo(pTemplateData->pInputNode.get());

    int error = 0;
    VSNodeRef * pSourceNode = a_cpVSAPI->propGetNode(a_pIn, "source", 0, &error);
    if(error == 0)
        pTemplateData->pSourceNode = PtrNodeRef(pSourceNode, FreeNodeRef{a_cpVSAPI});
    else
        pTemplateData->pSourceNode = PtrNodeRef(a_cpVSAPI->cloneNodeRef(pTemplateData->pInputNode.get()), FreeNodeRef{a_cpVSAPI});
    pTemplateData->cpSourceVideoInfo = a_cpVSAPI->getVideoInfo(pTemplateData->pSourceNode.get());

    std::string errorString;
    bool acceptableFormat = isConstantFormat(pTemplateData->cpInputVideoInfo) &&
        (pTemplateData->cpInputVideoInfo->numFrames != 0);
    if(!acceptableFormat)
    {
        a_cpVSAPI->setError(a_pOut, "nvof.GetMvTools: "
            "only constant format input with fixed frame number is supported.");
        return;
    }

    if(NVidiaOpticalFlow::bufferFormat(pTemplateData->cpInputVideoInfo->format,
        true, errorString) == NV_OF_BUFFER_FORMAT_UNDEFINED)
    {
        a_cpVSAPI->setError(a_pOut, (std::string("nvof.GetMvTools: "
            "Input format error. ") + errorString).c_str());
        return;
    }

    const VSFormat * cpSourceFormat = pTemplateData->cpSourceVideoInfo->format;
    acceptableFormat = isConstantFormat(pTemplateData->cpSourceVideoInfo) &&
        (pTemplateData->cpSourceVideoInfo->numFrames != 0) &&
        (cpSourceFormat->sampleType == stInteger) &&
        (cpSourceFormat->bitsPerSample > 0) &&
        (cpSourceFormat->bitsPerSample <= 16);
    if(!acceptableFormat)
    {
        a_cpVSAPI->setError(a_pOut, "nvof.GetMvTools: "
            "source clip must have constant integer format with 1 to 16 bits per sample.");
        return;
    }

    bool match = (pTemplateData->cpSourceVideoInfo->width == pTemplateData->cpInputVideoInfo->width) &&
        (pTemplateData->cpSourceVideoInfo->height == pTemplateData->cpInputVideoInfo->height) &&
        (pTemplateData->cpSourceVideoInfo->numFrames == pTemplateData->cpInputVideoInfo->numFrames);
    if(!match)
    {
        a_cpVSAPI->setError(a_pOut, "nvof.GetMvTools: "
            "clip and source must match in size and length.");
        return;
    }

    int64_t tr64 = getOptionalIntGetMVTools(a_pIn, "tr", 1, a_cpVSAPI);
    int64_t blockSizeX64 = getOptionalIntGetMVTools(a_pIn, "block_size", 8, a_cpVSAPI);
    int64_t blockSizeY64 = getOptionalIntGetMVTools(a_pIn, "block_size_v", blockSizeX64, a_cpVSAPI);
    int64_t overlapX64 = getOptionalIntGetMVTools(a_pIn, "overlap", 0, a_cpVSAPI);
    int64_t overlapY64 = getOptionalIntGetMVTools(a_pIn, "overlap_v", overlapX64, a_cpVSAPI);
    int64_t pel64 = getOptionalIntGetMVTools(a_pIn, "pel", 1, a_cpVSAPI);
    int64_t fallbackSAD = getOptionalIntGetMVTools(a_pIn, "fallback_sad", -1, a_cpVSAPI);
    int64_t clippedSAD = getOptionalIntGetMVTools(a_pIn, "clipped_sad", -1, a_cpVSAPI);
    int64_t getCost64 = getOptionalIntGetMVTools(a_pIn, "get_cost", 0, a_cpVSAPI);
    int64_t gpu64 = getOptionalIntGetMVTools(a_pIn, "gpu", 0, a_cpVSAPI);
    int64_t bidirectional64 = getOptionalIntGetMVTools(a_pIn, "bidirectional", 0, a_cpVSAPI);

    if(!validateIntRangeGetMVTools(tr64, "tr", a_pOut, a_cpVSAPI) ||
        !validateIntRangeGetMVTools(blockSizeX64, "block_size", a_pOut, a_cpVSAPI) ||
        !validateIntRangeGetMVTools(blockSizeY64, "block_size_v", a_pOut, a_cpVSAPI) ||
        !validateIntRangeGetMVTools(overlapX64, "overlap", a_pOut, a_cpVSAPI) ||
        !validateIntRangeGetMVTools(overlapY64, "overlap_v", a_pOut, a_cpVSAPI) ||
        !validateIntRangeGetMVTools(pel64, "pel", a_pOut, a_cpVSAPI) ||
        !validateIntRangeGetMVTools(gpu64, "gpu", a_pOut, a_cpVSAPI))
        return;

    const int tr = (int)tr64;
    const int blockSizeX = (int)blockSizeX64;
    const int blockSizeY = (int)blockSizeY64;
    const int overlapX = (int)overlapX64;
    const int overlapY = (int)overlapY64;
    const int pel = (int)pel64;
    if((tr < 1) || (tr > 3))
    {
        a_cpVSAPI->setError(a_pOut, "nvof.GetMvTools: "
            "tr must be between 1 and 3.");
        return;
    }
    if(!validateGeometryGetMVTools(blockSizeX, blockSizeY, overlapX, overlapY,
        pel, a_pOut, a_cpVSAPI))
        return;

    std::vector<mvtools::Config> mvtoolsConfigs;
    mvtoolsConfigs.reserve((size_t)tr * 2);
    for(int delta = 1; delta <= tr; ++delta)
    {
        for(int signIndex = 0; signIndex < 2; ++signIndex)
        {
            const int signedDelta = (signIndex == 0) ? delta : -delta;
            mvtools::AnalysisData analysisData{};
            if(!mvtools::buildAnalysisData(analysisData,
                *pTemplateData->cpSourceVideoInfo, signedDelta, blockSizeX,
                blockSizeY, overlapX, overlapY, pel, errorString))
            {
                a_cpVSAPI->setError(a_pOut, (std::string("nvof.GetMvTools: ") +
                    errorString).c_str());
                return;
            }

            mvtools::Config config{};
            if(!mvtools::buildConfig(config, analysisData, fallbackSAD,
                clippedSAD, errorString))
            {
                a_cpVSAPI->setError(a_pOut, (std::string("nvof.GetMvTools: ") +
                    errorString).c_str());
                return;
            }
            mvtoolsConfigs.push_back(config);
        }
    }

    pTemplateData->videoInfo = *pTemplateData->cpSourceVideoInfo;
    pTemplateData->videoInfo.format = a_cpVSAPI->getFormatPreset(pfGray8, a_pCore);
    pTemplateData->tr = tr;
    pTemplateData->mvtoolsConfigs = mvtoolsConfigs;
    pTemplateData->pShared = std::make_shared<SharedGetMVTools>(a_cpVSAPI,
        *pTemplateData->cpInputVideoInfo, getCost64 != 0, (int)gpu64, tr,
        bidirectional64 != 0);

    for(int outputIndex = 0; outputIndex < tr * 2; ++outputIndex)
    {
        DataGetMVTools * pInternalData = new DataGetMVTools;
        pInternalData->pInputNode = PtrNodeRef(a_cpVSAPI->cloneNodeRef(pTemplateData->pInputNode.get()), FreeNodeRef{a_cpVSAPI});
        pInternalData->pSourceNode = PtrNodeRef(a_cpVSAPI->cloneNodeRef(pTemplateData->pSourceNode.get()), FreeNodeRef{a_cpVSAPI});
        pInternalData->cpInputVideoInfo = pTemplateData->cpInputVideoInfo;
        pInternalData->cpSourceVideoInfo = pTemplateData->cpSourceVideoInfo;
        pInternalData->videoInfo = pTemplateData->videoInfo;
        pInternalData->pShared = pTemplateData->pShared;
        pInternalData->mvtoolsConfigs = pTemplateData->mvtoolsConfigs;
        pInternalData->outputIndex = outputIndex;
        pInternalData->tr = tr;
        a_cpVSAPI->createFilter(a_pIn, a_pOut, "GetMvTools", initGetMVTools,
            getFrameGetMVTools, freeGetMVTools, fmParallelRequests, 0,
            pInternalData, a_pCore);
    }
}

//==============================================================================

void VS_CC initGetMVTools(VSMap * a_pIn, VSMap * a_pOut,
    void ** a_ppInstanceData, VSNode * a_pNode, VSCore * a_pCore,
    const VSAPI * a_cpVSAPI)
{
    UNUSED(a_pIn);
    UNUSED(a_pOut);
    UNUSED(a_pCore);

    DataGetMVTools * pInternalData = (DataGetMVTools *) *a_ppInstanceData;
    a_cpVSAPI->setVideoInfo(&pInternalData->videoInfo, 1, a_pNode);
}

//==============================================================================

void VS_CC freeGetMVTools(void * a_pInstanceData, VSCore * a_pCore,
    const VSAPI * a_cpVSAPI)
{
    UNUSED(a_pCore);
    UNUSED(a_cpVSAPI);

    DataGetMVTools * pInternalData = (DataGetMVTools *)a_pInstanceData;
    delete pInternalData;
}

//==============================================================================

const VSFrameRef * VS_CC getFrameGetMVTools(int a_n, int a_activationReason,
    void ** a_ppInstanceData, void ** a_ppFrameData,
    VSFrameContext * a_pFrameCtx, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
    UNUSED(a_ppFrameData);

    DataGetMVTools * pInternalData = (DataGetMVTools *) *a_ppInstanceData;
    const int framesNumber = pInternalData->cpSourceVideoInfo->numFrames;
    a_n = clamp(a_n, 0, framesNumber - 1);

    const int outputIndex = pInternalData->outputIndex;
    const int distance = outputIndex / 2 + 1;
    const bool isBackward = (outputIndex & 1) == 0;
    const bool bidirectional = pInternalData->pShared->bidirectional();
    const int firstFrame = isBackward ? a_n : a_n - distance;
    const int secondFrame = isBackward ? a_n + distance : a_n;
    const int referenceFrame = a_n + (isBackward ? distance : -distance);
    const bool valid = bidirectional ?
        ((firstFrame >= 0) && (secondFrame < framesNumber)) :
        ((referenceFrame >= 0) && (referenceFrame < framesNumber));

    if(a_activationReason == arInitial)
    {
        if(valid && bidirectional)
        {
            a_cpVSAPI->requestFrameFilter(firstFrame, pInternalData->pInputNode.get(), a_pFrameCtx);
            a_cpVSAPI->requestFrameFilter(secondFrame, pInternalData->pInputNode.get(), a_pFrameCtx);
        }
        else if(valid)
        {
            a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pInputNode.get(), a_pFrameCtx);
            a_cpVSAPI->requestFrameFilter(referenceFrame, pInternalData->pInputNode.get(), a_pFrameCtx);
        }
        else
            a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pInputNode.get(), a_pFrameCtx);
        return nullptr;
    }
    else if(a_activationReason != arAllFramesReady)
        return nullptr;

    const VSFrameRef * cpPropFrame = valid ? nullptr :
        a_cpVSAPI->getFrameFilter(a_n, pInternalData->pInputNode.get(), a_pFrameCtx);

    VSFrameRef * pOutFrame = a_cpVSAPI->newVideoFrame(pInternalData->videoInfo.format,
        pInternalData->videoInfo.width, pInternalData->videoInfo.height,
        cpPropFrame, a_pCore);

    uint8_t * pDstRow = a_cpVSAPI->getWritePtr(pOutFrame, PLANE_Y);
    const int dstStride = a_cpVSAPI->getStride(pOutFrame, PLANE_Y);
    memset(pDstRow, 0, (size_t)dstStride * pInternalData->videoInfo.height);

    std::vector<uint8_t> vectorBlob;
    const mvtools::Config & config = pInternalData->mvtoolsConfigs[outputIndex];
    if(valid && bidirectional)
    {
        const VSFrameRef * cpFirstFrame = a_cpVSAPI->getFrameFilter(firstFrame,
            pInternalData->pInputNode.get(), a_pFrameCtx);
        const VSFrameRef * cpSecondFrame = a_cpVSAPI->getFrameFilter(secondFrame,
            pInternalData->pInputNode.get(), a_pFrameCtx);

        PairBlobs blobs;
        std::string errorString;
        const mvtools::Config & forwardConfig = pInternalData->mvtoolsConfigs[(distance - 1) * 2];
        const mvtools::Config & backwardConfig = pInternalData->mvtoolsConfigs[(distance - 1) * 2 + 1];
        if(!pInternalData->pShared->getPairBlobs(firstFrame, secondFrame,
            cpFirstFrame, cpSecondFrame, forwardConfig, backwardConfig, blobs,
            errorString))
        {
            a_cpVSAPI->setFilterError((std::string("nvof.GetMvTools: ") +
                errorString).c_str(), a_pFrameCtx);
            if(cpPropFrame)
                a_cpVSAPI->freeFrame(cpPropFrame);
            a_cpVSAPI->freeFrame(cpFirstFrame);
            a_cpVSAPI->freeFrame(cpSecondFrame);
            a_cpVSAPI->freeFrame(pOutFrame);
            return nullptr;
        }

        vectorBlob = isBackward ? std::move(blobs.forward) : std::move(blobs.backward);
        a_cpVSAPI->freeFrame(cpFirstFrame);
        a_cpVSAPI->freeFrame(cpSecondFrame);
    }
    else if(valid)
    {
        const VSFrameRef * cpCurrentFrame = a_cpVSAPI->getFrameFilter(a_n,
            pInternalData->pInputNode.get(), a_pFrameCtx);
        const VSFrameRef * cpReferenceFrame = a_cpVSAPI->getFrameFilter(referenceFrame,
            pInternalData->pInputNode.get(), a_pFrameCtx);

        std::string errorString;
        if(!pInternalData->pShared->getForwardBlob(a_n, referenceFrame,
            cpCurrentFrame, cpReferenceFrame, config, vectorBlob, errorString))
        {
            a_cpVSAPI->setFilterError((std::string("nvof.GetMvTools: ") +
                errorString).c_str(), a_pFrameCtx);
            if(cpPropFrame)
                a_cpVSAPI->freeFrame(cpPropFrame);
            a_cpVSAPI->freeFrame(cpCurrentFrame);
            a_cpVSAPI->freeFrame(cpReferenceFrame);
            a_cpVSAPI->freeFrame(pOutFrame);
            return nullptr;
        }

        a_cpVSAPI->freeFrame(cpCurrentFrame);
        a_cpVSAPI->freeFrame(cpReferenceFrame);
    }
    else
    {
        vectorBlob = mvtools::makeVectorBlob(config, false,
            [](int, int) -> mvtools::Sample { return mvtools::Sample{}; });
    }

    bool success = mvtools::writeDataProp(pOutFrame, mvtools::PROP_ANALYSIS_DATA,
        &config.analysisData, sizeof(config.analysisData), a_cpVSAPI) &&
        mvtools::writeDataProp(pOutFrame, mvtools::PROP_VECTORS,
            vectorBlob.data(), vectorBlob.size(), a_cpVSAPI);
    if(!success)
    {
        a_cpVSAPI->setFilterError("nvof.GetMvTools: "
            "failed to write MVTools frame properties.", a_pFrameCtx);
        if(cpPropFrame)
            a_cpVSAPI->freeFrame(cpPropFrame);
        a_cpVSAPI->freeFrame(pOutFrame);
        return nullptr;
    }

    if(cpPropFrame)
        a_cpVSAPI->freeFrame(cpPropFrame);

    return pOutFrame;
}

//==============================================================================
