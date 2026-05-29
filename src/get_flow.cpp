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
#include "nvidia-optical-flow.h"

#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>

//==============================================================================
// Dedicated optical flow worker

class NVidiaOpticalFlowWorker
{
public:
	NVidiaOpticalFlowWorker(const VSAPI * a_cpVSAPI, const VSVideoInfo & a_sourceVideoInfo,
		bool a_chromaMotion, NV_OF_PERF_LEVEL a_perfLevel, bool a_getCost,
		int a_gpuID):
		  m_cpVSAPI(a_cpVSAPI)
		, m_sourceVideoInfo(a_sourceVideoInfo)
		, m_chromaMotion(a_chromaMotion)
		, m_perfLevel(a_perfLevel)
		, m_getCost(a_getCost)
		, m_gpuID(a_gpuID)
	{
		m_thread = std::thread(&NVidiaOpticalFlowWorker::threadMain, this);
	}

	~NVidiaOpticalFlowWorker()
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_stop = true;
		}
		m_condition.notify_one();
		if(m_thread.joinable())
			m_thread.join();
	}

	bool getFlow(const VSFrameRef * a_cpCurrentFrame, const VSFrameRef * a_cpDeltaFrame,
		VSFrameRef * a_pOutFrame, std::string & a_errorString)
	{
		std::unique_lock<std::mutex> lock(m_lock);
		m_cpCurrentFrame = a_cpCurrentFrame;
		m_cpDeltaFrame = a_cpDeltaFrame;
		m_pOutFrame = a_pOutFrame;
		m_success = false;
		m_errorString.clear();
		m_responseReady = false;
		m_requestPending = true;
		m_condition.notify_one();
		m_condition.wait(lock, [this] { return m_responseReady; });
		a_errorString = m_errorString;
		return m_success;
	}
private:
	void threadMain()
	{
		std::unique_ptr<NVidiaOpticalFlow> pFlow(new NVidiaOpticalFlow(m_cpVSAPI,
			m_sourceVideoInfo, m_chromaMotion, m_perfLevel, m_getCost, m_gpuID));
		while(true)
		{
			std::unique_lock<std::mutex> lock(m_lock);
			m_condition.wait(lock, [this] { return m_requestPending || m_stop; });
			if(m_stop && !m_requestPending)
				break;
			const VSFrameRef * cpCurrentFrame = m_cpCurrentFrame;
			const VSFrameRef * cpDeltaFrame = m_cpDeltaFrame;
			VSFrameRef * pOutFrame = m_pOutFrame;
			m_requestPending = false;
			lock.unlock();

			std::string errorString;
			bool success = false;
			{
				std::lock_guard<std::mutex> nvofLock(nvofGlobalLock());
				success = pFlow->init(errorString);
				if(success)
					success = pFlow->getFlow(cpCurrentFrame, cpDeltaFrame, pOutFrame,
						errorString);
				else
					errorString = std::string("failed to initialize the flow calculator. ") +
						errorString;
			}

			lock.lock();
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
	VSVideoInfo m_sourceVideoInfo{};
	bool m_chromaMotion{true};
	NV_OF_PERF_LEVEL m_perfLevel{NV_OF_PERF_LEVEL_SLOW};
	bool m_getCost{false};
	int m_gpuID{0};
	std::thread m_thread;
	std::mutex m_lock;
	std::condition_variable m_condition;
	const VSFrameRef * m_cpCurrentFrame{nullptr};
	const VSFrameRef * m_cpDeltaFrame{nullptr};
	VSFrameRef * m_pOutFrame{nullptr};
	bool m_requestPending{false};
	bool m_responseReady{false};
	bool m_stop{false};
	bool m_success{false};
	std::string m_errorString;
};

//==============================================================================
// Filter internal data structure

struct DataGetFlow
{
	PtrNodeRef pInputNode{nullptr};
	const VSVideoInfo * cpSourceVideoInfo{nullptr};
	VSVideoInfo videoInfo{};
	int64_t delta{1};
	int64_t chromaMotion{1};
	NV_OF_PERF_LEVEL perfLevel{NV_OF_PERF_LEVEL_UNDEFINED};
	bool getCost{false};
	int64_t gpu{0};
	std::unique_ptr<NVidiaOpticalFlowWorker> pFlowWorker;
	std::mutex flowLock;
};

//==============================================================================
// Forward declarations

InitVSNodeFun initGetFlow;
GetFrameFun getFrameGetFlow;
FreeVSNodeFun freeGetFlow;

//==============================================================================

void VS_CC createGetFlow(const VSMap * a_pIn, VSMap * a_pOut, void * a_pUserData,
	VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pUserData);

	std::string errorString;
	std::unique_ptr<DataGetFlow> pInternalData(new DataGetFlow);

	pInternalData->pInputNode = PtrNodeRef(a_cpVSAPI->propGetNode(a_pIn, "clip", 0, 0), FreeNodeRef{a_cpVSAPI});
	pInternalData->cpSourceVideoInfo = a_cpVSAPI->getVideoInfo(pInternalData->pInputNode.get());

	//--------------------------------------------------------------------------
	// Format check

	bool acceptableFormat = isConstantFormat(pInternalData->cpSourceVideoInfo) &&
		(pInternalData->cpSourceVideoInfo->numFrames != 0);
	if(!acceptableFormat)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.GetFlow: "
			"only constant format input with fixed frame number is supported.");
		return;
	}

	//--------------------------------------------------------------------------
	// delta

	int error = 0;
	pInternalData->delta = a_cpVSAPI->propGetInt(a_pIn, "delta", 0, &error);
	if(error != 0)
	{
		a_cpVSAPI->setError(a_pOut, "nvof.GetFlow: "
			"failed to initialize the \"delta\" argument.");
		return;
	}

	//--------------------------------------------------------------------------
	// chromaMotion

	pInternalData->chromaMotion = a_cpVSAPI->propGetInt(a_pIn, "chroma_motion", 0, &error);
	if(error != 0)
	{
		pInternalData->chromaMotion = 1;
		error = 0;
	}

	if(NVidiaOpticalFlow::bufferFormat(pInternalData->cpSourceVideoInfo->format,
		pInternalData->chromaMotion, errorString) == NV_OF_BUFFER_FORMAT_UNDEFINED)
	{
		a_cpVSAPI->setError(a_pOut, (std::string("nvof.GetFlow: "
			"Source format error. ") + errorString).c_str());
		return;
	}

	//--------------------------------------------------------------------------
	// speed

	int64_t speed = a_cpVSAPI->propGetInt(a_pIn, "speed", 0, &error);
	if(error != 0)
	{
		speed = 0;
		error = 0;
	}

	static std::map<int64_t, NV_OF_PERF_LEVEL> perfLevels = {
		{0, NV_OF_PERF_LEVEL_SLOW},
		{1, NV_OF_PERF_LEVEL_MEDIUM},
		{2, NV_OF_PERF_LEVEL_FAST},
	};

	std::map<int64_t, NV_OF_PERF_LEVEL>::const_iterator it =
		perfLevels.find(speed);
	if(it == perfLevels.end())
	{
		a_cpVSAPI->setError(a_pOut, "nvof.GetFlow: "
			"Invalid value for the \"speed\" argument.\n"
			"Acceptable values:\n"
			"0 - slow, best quality\n"
			"1 - medium, medium quality\n"
			"2 - fast, low quality\n"
			);
		return;
	}

	pInternalData->perfLevel = it->second;

	//--------------------------------------------------------------------------
	// getCost

	pInternalData->getCost = a_cpVSAPI->propGetInt(a_pIn, "get_cost", 0, &error);
	if(error != 0)
	{
		pInternalData->getCost = false;
		error = 0;
	}

	//--------------------------------------------------------------------------
	// gpu

	pInternalData->gpu = a_cpVSAPI->propGetInt(a_pIn, "gpu", 0, &error);
	if(error != 0)
	{
		pInternalData->gpu = 0;
		error = 0;
	}

	//--------------------------------------------------------------------------
	// output video info

	pInternalData->videoInfo = *pInternalData->cpSourceVideoInfo;
	pInternalData->videoInfo.format = a_cpVSAPI->getFormatPreset(pfYUV444PS, a_pCore);

	//--------------------------------------------------------------------------

	a_cpVSAPI->createFilter(a_pIn, a_pOut, "GetFlow", initGetFlow,
		getFrameGetFlow, freeGetFlow, fmParallelRequests, 0, pInternalData.release(), a_pCore);
}

//==============================================================================

void VS_CC initGetFlow(VSMap * a_pIn, VSMap * a_pOut, void ** a_ppInstanceData,
	VSNode * a_pNode, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pIn);
	UNUSED(a_pOut);
	UNUSED(a_pCore);

	DataGetFlow * pInternalData = (DataGetFlow *) *a_ppInstanceData;
	a_cpVSAPI->setVideoInfo(&pInternalData->videoInfo, 1, a_pNode);
}

//==============================================================================

// Free all allocated data on filter destruction
void VS_CC freeGetFlow(void * a_pInstanceData, VSCore * a_pCore,
	const VSAPI * a_cpVSAPI)
{
	UNUSED(a_pCore);

	DataGetFlow * pInternalData = (DataGetFlow *)a_pInstanceData;
	delete pInternalData;
}

//==============================================================================

const VSFrameRef * VS_CC getFrameGetFlow(int a_n, int a_activationReason,
	void ** a_ppInstanceData, void ** a_ppFrameData,
	VSFrameContext * a_pFrameCtx, VSCore * a_pCore, const VSAPI * a_cpVSAPI)
{
	UNUSED(a_ppFrameData);

	DataGetFlow * pInternalData = (DataGetFlow *) *a_ppInstanceData;
	int framesNumber = pInternalData->cpSourceVideoInfo->numFrames;

	a_n = clamp(a_n, 0, framesNumber - 1);
	int n2 = clamp(a_n + (int)pInternalData->delta, 0, framesNumber - 1);

	if(a_activationReason == arInitial)
	{
		a_cpVSAPI->requestFrameFilter(a_n, pInternalData->pInputNode.get(), a_pFrameCtx);
		a_cpVSAPI->requestFrameFilter(n2, pInternalData->pInputNode.get(), a_pFrameCtx);
		return nullptr;
	}
	else if(a_activationReason != arAllFramesReady)
		return nullptr;

	const VSFormat * cpFormat = pInternalData->videoInfo.format;

	const VSFrameRef * cpCurrentFrame = a_cpVSAPI->getFrameFilter(a_n,
		pInternalData->pInputNode.get(), a_pFrameCtx);
	const VSFrameRef * cpDeltaFrame = a_cpVSAPI->getFrameFilter(n2,
		pInternalData->pInputNode.get(), a_pFrameCtx);

	VSFrameRef * pOutFrame = a_cpVSAPI->newVideoFrame(cpFormat,
		pInternalData->videoInfo.width, pInternalData->videoInfo.height,
		nullptr, a_pCore);

	std::string errorString;

	{
		std::lock_guard<std::mutex> lock(pInternalData->flowLock);
		if(!pInternalData->pFlowWorker)
		{
			pInternalData->pFlowWorker.reset(new NVidiaOpticalFlowWorker(a_cpVSAPI,
				*pInternalData->cpSourceVideoInfo,
				(bool)pInternalData->chromaMotion,
				pInternalData->perfLevel,
				pInternalData->getCost,
				(int)pInternalData->gpu));
		}

		if(!pInternalData->pFlowWorker->getFlow(cpCurrentFrame, cpDeltaFrame, pOutFrame,
			errorString))
		{
			a_cpVSAPI->setFilterError((std::string("nvof.GetFlow: ") +
				errorString).c_str(), a_pFrameCtx);
			a_cpVSAPI->freeFrame(cpCurrentFrame);
			a_cpVSAPI->freeFrame(cpDeltaFrame);
			a_cpVSAPI->freeFrame(pOutFrame);
			return nullptr;
		}
	}

	a_cpVSAPI->freeFrame(cpCurrentFrame);
	a_cpVSAPI->freeFrame(cpDeltaFrame);

	return pOutFrame;
}

//==============================================================================
