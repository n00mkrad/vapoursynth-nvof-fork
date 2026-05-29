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

#ifndef NVIDIA_OPTICAL_FLOW_H
#define NVIDIA_OPTICAL_FLOW_H

#include "nvidia-optical-flow-api.h"

class NVidiaOpticalFlow
{
public:
	NVidiaOpticalFlow(const VSAPI * a_cpVSAPI, const VSVideoInfo & a_sourceVideoInfo,
		bool a_chromaMotion = true, NV_OF_PERF_LEVEL a_perfLevel = NV_OF_PERF_LEVEL_SLOW,
		bool a_getCost = false, int a_gpuID = 0);
	virtual ~NVidiaOpticalFlow();

	static NV_OF_BUFFER_FORMAT bufferFormat(const VSFormat * a_cpFormat, bool chromaMotion,
		std::string & a_errorString);

	bool init(std::string & a_errorString);

	bool getFlow(const VSFrameRef * a_cpCurrentFrame, const VSFrameRef * a_cpDeltaFrame,
		VSFrameRef * a_pOutFrame, std::string & a_errorString);
private:
	void cleanup();
	void releaseAPI();
	void destroyContext();
	bool initNVOF(std::string & a_errorString);
	void destroyNVOF();
	bool createBuffers(std::string & a_errorString);
	void destroyBuffers();

	bool pushContext(std::string & a_errorString);
	bool popContext(std::string & a_errorString);
	bool loadLumaToBuffer(const VSFrameRef * a_cpFrame, NvOFGPUBufferHandle a_buffer,
		std::string & a_errorString);
	bool loadYUV420toBuffer(const VSFrameRef * a_cpFrame, NvOFGPUBufferHandle a_buffer,
		std::string & a_errorString);
	bool loadRGBtoBuffer(const VSFrameRef * a_cpFrame, NvOFGPUBufferHandle a_buffer,
		std::string & a_errorString);
	bool downloadFlow(VSFrameRef * a_cpFrame, std::string & a_errorString);

	const VSAPI * m_cpVSAPI{nullptr};
	VSVideoInfo m_sourceVideoInfo{};
	bool m_chromaMotion{true};
	NV_OF_PERF_LEVEL m_perfLevel{NV_OF_PERF_LEVEL_SLOW};
	bool m_getCost{false};
	int m_gpuID{0};
	NvOFHandle m_handle{nullptr};
	CUcontext m_context{nullptr};
	NV_OF_BUFFER_FORMAT m_sourceBufferFormat{NV_OF_BUFFER_FORMAT_UNDEFINED};
	NvOFGPUBufferHandle m_hSourceFrame{nullptr};
	NvOFGPUBufferHandle m_hDeltaFrame{nullptr};
	NvOFGPUBufferHandle m_hFlow{nullptr};
	NvOFGPUBufferHandle m_hCost{nullptr};
	bool m_apiInitialized{false};
	bool m_initialized{false};
};

#endif
