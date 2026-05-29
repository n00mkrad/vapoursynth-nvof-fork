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

#include "nvidia-optical-flow.h"

#include "cuda-tools.h"

#include <cassert>

//==============================================================================

NVidiaOpticalFlow::NVidiaOpticalFlow(const VSAPI * a_cpVSAPI,
	const VSVideoInfo & a_sourceVideoInfo, bool a_chromaMotion,
	NV_OF_PERF_LEVEL a_perfLevel, bool a_getCost, int a_gpuID):
	  m_cpVSAPI(a_cpVSAPI)
	, m_sourceVideoInfo(a_sourceVideoInfo)
	, m_chromaMotion(a_chromaMotion)
	, m_perfLevel(a_perfLevel)
	, m_getCost(a_getCost)
	, m_gpuID(a_gpuID)
{
	assert(m_cpVSAPI);
}

//==============================================================================

NVidiaOpticalFlow::~NVidiaOpticalFlow()
{
	cleanup();
}

//==============================================================================

NV_OF_BUFFER_FORMAT NVidiaOpticalFlow::bufferFormat(const VSFormat * a_cpFormat,
	bool a_chromaMotion, std::string & a_errorString)
{
	const int planes = a_cpFormat->numPlanes;
	const VSColorFamily family = (VSColorFamily)a_cpFormat->colorFamily;
	const int ssW = a_cpFormat->subSamplingW;
	const int ssH = a_cpFormat->subSamplingH;

	if((a_cpFormat->bitsPerSample != 8) || ((planes != 1) && (planes != 3)))
	{
		a_errorString = "Only 8 bit per sample formats with 1 or 3 planes "
			"are supported.";
		return NV_OF_BUFFER_FORMAT_UNDEFINED;
	}

	if(planes == 1)
		return NV_OF_BUFFER_FORMAT_GRAYSCALE8;

	if(((family == cmYUV) || (family == cmYCoCg)) && (!a_chromaMotion))
		return NV_OF_BUFFER_FORMAT_GRAYSCALE8;

	if(((family == cmYUV) || (family == cmYCoCg)) && (ssH == 1) && (ssW == 1))
		return NV_OF_BUFFER_FORMAT_NV12;

	if((ssH != 0) || (ssW != 0))
	{
		a_errorString = "Subsampling only supported for YUV420P8 format.";
		return NV_OF_BUFFER_FORMAT_UNDEFINED;
	}

	return NV_OF_BUFFER_FORMAT_ABGR8;
}

//==============================================================================

bool NVidiaOpticalFlow::init(std::string & a_errorString)
{
	if(m_initialized)
		return true;

	int planes = m_sourceVideoInfo.format->numPlanes;
	if((planes != 1) && (planes != 3))
	{
		a_errorString = "Only 1 and 3 planes are supported.";
		return false;
	}

	if(!NVidiaOpticalFlowAPI::create(a_errorString))
		return false;
	m_apiInitialized = true;

	CUdevice device;
	CUresult cuResult = cuDeviceGet(&device, m_gpuID);
	if(cuResult != CUDA_SUCCESS)
	{
		a_errorString = NVidiaOpticalFlowAPI::getErrorDescription(cuResult);
		cleanup();
		return false;
	}

	#if CUDA_VERSION >= 13000
		cuResult = cuCtxCreate(&m_context, nullptr, 0, device);
	#else
		cuResult = cuCtxCreate(&m_context, 0, device);
	#endif
	if(cuResult != CUDA_SUCCESS)
	{
		a_errorString = NVidiaOpticalFlowAPI::getErrorDescription(cuResult);
		cleanup();
		return false;
	}

	if(!initNVOF(a_errorString))
	{
		cleanup();
		return false;
	}

	if(!createBuffers(a_errorString))
	{
		cleanup();
		return false;
	}

	m_initialized = true;

	return true;
}

//==============================================================================

bool NVidiaOpticalFlow::getFlow(const VSFrameRef * a_cpCurrentFrame,
	const VSFrameRef * a_cpDeltaFrame, VSFrameRef * a_pOutFrame,
	std::string & a_errorString)
{
	const VSFormat * cpCurrentFormat = m_cpVSAPI->getFrameFormat(a_cpCurrentFrame);
	const VSFormat * cpDeltaFormat = m_cpVSAPI->getFrameFormat(a_cpDeltaFrame);
	if((m_sourceVideoInfo.format != cpCurrentFormat) || (cpCurrentFormat != cpDeltaFormat))
	{
		a_errorString = "Input frame format changed.";
		return false;
	}

	if(m_sourceBufferFormat == NV_OF_BUFFER_FORMAT_NV12)
	{
		if(!loadYUV420toBuffer(a_cpCurrentFrame, m_hSourceFrame, a_errorString))
			return false;
		if(!loadYUV420toBuffer(a_cpDeltaFrame, m_hDeltaFrame, a_errorString))
			return false;
	}
	else if(m_sourceBufferFormat == NV_OF_BUFFER_FORMAT_GRAYSCALE8)
	{
		if(!loadLumaToBuffer(a_cpCurrentFrame, m_hSourceFrame, a_errorString))
			return false;
		if(!loadLumaToBuffer(a_cpDeltaFrame, m_hDeltaFrame, a_errorString))
			return false;
	}
	else if(m_sourceBufferFormat == NV_OF_BUFFER_FORMAT_ABGR8)
	{
		if(!loadRGBtoBuffer(a_cpCurrentFrame, m_hSourceFrame, a_errorString))
			return false;
		if(!loadRGBtoBuffer(a_cpDeltaFrame, m_hDeltaFrame, a_errorString))
			return false;
	}
	else
	{
		a_errorString = "Invalid source buffer format.";
		return false;
	}
	
	NV_OF_EXECUTE_INPUT_PARAMS inputParams{0};
	inputParams.inputFrame = m_hSourceFrame;
	inputParams.referenceFrame = m_hDeltaFrame;
	inputParams.externalHints = nullptr;
	inputParams.disableTemporalHints = NV_OF_TRUE;
	inputParams.padding = 0;
	inputParams.hPrivData = nullptr;
	inputParams.padding2 = 0;
	inputParams.numRois = 0;
	inputParams.roiData = nullptr;

	NV_OF_EXECUTE_OUTPUT_PARAMS outputParams{0};
	outputParams.outputBuffer = m_hFlow;
	outputParams.outputCostBuffer = m_getCost ? m_hCost : nullptr;
	outputParams.hPrivData = nullptr;
	outputParams.bwdOutputBuffer = nullptr;
	outputParams.bwdOutputCostBuffer = nullptr;
	outputParams.globalFlowBuffer = nullptr;

	if(!pushContext(a_errorString))
		return false;
	NV_OF_STATUS result = NVidiaOpticalFlowAPI::getAPI()->nvOFExecute(m_handle,
		&inputParams, &outputParams);
	if(!popContext(a_errorString))
		return false;
	if(result != NV_OF_SUCCESS)
	{
		a_errorString = std::string("Failed to execute optical flow. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(result);
		return false;
	}

	return downloadFlow(a_pOutFrame, a_errorString);
}

//==============================================================================

void NVidiaOpticalFlow::cleanup()
{
	destroyBuffers();
	destroyNVOF();
	destroyContext();
	releaseAPI();
	m_initialized = false;
}

//==============================================================================

void NVidiaOpticalFlow::releaseAPI()
{
	if(!m_apiInitialized)
		return;

	std::string destroyError;
	NVidiaOpticalFlowAPI::destroy(destroyError);
	m_apiInitialized = false;
}

//==============================================================================

void NVidiaOpticalFlow::destroyContext()
{
	if(!m_context)
		return;

	cuCtxDestroy(m_context);
	m_context = nullptr;
}

//==============================================================================

bool NVidiaOpticalFlow::initNVOF(std::string & a_errorString)
{
	m_sourceBufferFormat = bufferFormat(m_sourceVideoInfo.format,
		m_chromaMotion, a_errorString);
	if(m_sourceBufferFormat == NV_OF_BUFFER_FORMAT_UNDEFINED)
		return false;

	NV_OF_STATUS result = NVidiaOpticalFlowAPI::getAPI()->nvCreateOpticalFlowCuda(
		m_context, &m_handle);
	if(result != NV_OF_SUCCESS)
	{
		a_errorString = "NVidiaOpticalFlow: Failed to create CUDA flow. ";
		a_errorString += NVidiaOpticalFlowAPI::getErrorDescription(result);
		return false;
	}

	assert(m_handle);

	NV_OF_INIT_PARAMS params{0};
	params.width = (uint32_t)m_sourceVideoInfo.width;
	params.height = (uint32_t)m_sourceVideoInfo.height;
	params.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_1;
	params.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
	params.mode = NV_OF_MODE_OPTICALFLOW;
	params.perfLevel = m_perfLevel;
	params.enableExternalHints = NV_OF_FALSE;
	params.enableOutputCost = m_getCost ? NV_OF_TRUE : NV_OF_FALSE;
	params.hPrivData = 0;
	params.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
	params.enableRoi = NV_OF_FALSE;
	params.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
	params.enableGlobalFlow = NV_OF_FALSE;
	params.inputBufferFormat = m_sourceBufferFormat;

	result = NVidiaOpticalFlowAPI::getAPI()->nvOFInit(m_handle, &params);
	if(result != NV_OF_SUCCESS)
	{
		a_errorString = "NVidiaOpticalFlow: Failed to initialize CUDA flow. ";
		a_errorString += NVidiaOpticalFlowAPI::getErrorDescription(result);
		destroyNVOF();
		return false;
	}

	return true;
}

//==============================================================================

void NVidiaOpticalFlow::destroyNVOF()
{
	if(!m_handle)
		return;

	NVidiaOpticalFlowAPI::getAPI()->nvOFDestroy(m_handle);
	m_handle = nullptr;
}

//==============================================================================

bool NVidiaOpticalFlow::createBuffers(std::string & a_errorString)
{
	if(!m_handle)
	{
		a_errorString = "Failed to create CUDA buffers. "
			"NVidiaOpticalFlow is not initialized.";
		return false;
	}

	NV_OF_BUFFER_DESCRIPTOR descriptor{0};
	descriptor.width = (uint32_t)m_sourceVideoInfo.width;
	descriptor.height = (uint32_t)m_sourceVideoInfo.height;
	descriptor.bufferUsage = NV_OF_BUFFER_USAGE_UNDEFINED;
	descriptor.bufferFormat = NV_OF_BUFFER_FORMAT_UNDEFINED;

	struct BufferInitInfo
	{
		NvOFGPUBufferHandle * pHandle{nullptr};
		NV_OF_BUFFER_USAGE usage{NV_OF_BUFFER_USAGE_UNDEFINED};
		NV_OF_BUFFER_FORMAT format{NV_OF_BUFFER_FORMAT_UNDEFINED};
	};

	BufferInitInfo buffersToInit[] = {
		{&m_hSourceFrame, NV_OF_BUFFER_USAGE_INPUT, m_sourceBufferFormat},
		{&m_hDeltaFrame, NV_OF_BUFFER_USAGE_INPUT, m_sourceBufferFormat},
		{&m_hFlow, NV_OF_BUFFER_USAGE_OUTPUT, NV_OF_BUFFER_FORMAT_SHORT2},
		{&m_hCost, NV_OF_BUFFER_USAGE_COST, NV_OF_BUFFER_FORMAT_UINT8},
	};

	for(const BufferInitInfo & info : buffersToInit)
	{
		if((info.usage == NV_OF_BUFFER_USAGE_COST) && (!m_getCost))
			continue;

		descriptor.bufferUsage = info.usage;
		descriptor.bufferFormat = info.format;
		NV_OF_STATUS result = NVidiaOpticalFlowAPI::getAPI()->nvOFCreateGPUBufferCuda(
			m_handle, &descriptor, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, info.pHandle);
		if(result != NV_OF_SUCCESS)
		{
			a_errorString = std::string("Failed to create a CUDA buffer. ") +
				NVidiaOpticalFlowAPI::getErrorDescription(result);
			destroyBuffers();
			return false;
		}
	}

	return true;
}

//==============================================================================

void NVidiaOpticalFlow::destroyBuffers()
{
	NvOFGPUBufferHandle * buffers[] = {&m_hSourceFrame, &m_hDeltaFrame,
		&m_hFlow, &m_hCost};
	for(NvOFGPUBufferHandle * pHandle : buffers)
	{
		if(!(*pHandle))
			continue;
		NVidiaOpticalFlowAPI::getAPI()->nvOFDestroyGPUBufferCuda(*pHandle);
		*pHandle = nullptr;
	}
}

//==============================================================================

bool NVidiaOpticalFlow::pushContext(std::string & a_errorString)
{
	CUresult result = cuCtxPushCurrent(m_context);
	if(result == CUDA_SUCCESS)
		return true;

	a_errorString = std::string("Failed to push CUDA context. ") +
		NVidiaOpticalFlowAPI::getErrorDescription(result);
	return false;
}

//==============================================================================

bool NVidiaOpticalFlow::popContext(std::string & a_errorString)
{
	CUresult result = cuCtxPopCurrent(nullptr);
	if(result == CUDA_SUCCESS)
		return true;

	a_errorString = std::string("Failed to pop CUDA context. ") +
		NVidiaOpticalFlowAPI::getErrorDescription(result);
	return false;
}

//==============================================================================

bool NVidiaOpticalFlow::loadLumaToBuffer(const VSFrameRef * a_cpFrame,
	NvOFGPUBufferHandle a_buffer, std::string & a_errorString)
{
	const size_t width = m_sourceVideoInfo.width;
	const size_t height = m_sourceVideoInfo.height;
	const int srcStride = m_cpVSAPI->getStride(a_cpFrame, PLANE_Y);
	const uint8_t * cpSrc = m_cpVSAPI->getReadPtr(a_cpFrame, PLANE_Y);

	if(!pushContext(a_errorString))
		return false;

	CUdeviceptr devicePtr = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetCUdeviceptr(a_buffer);

	NV_OF_CUDA_BUFFER_STRIDE_INFO strideInfo{};
	NV_OF_STATUS nvResult = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetStrideInfo(
		a_buffer, &strideInfo);
	if(nvResult != NV_OF_SUCCESS)
	{
		popContext(a_errorString);
		a_errorString = std::string("Failed to get input buffer stride info. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(nvResult);
		return false;
	}

	CUDA_MEMCPY2D cuCopy2d{0};
	cuCopy2d.WidthInBytes = width;
	cuCopy2d.srcMemoryType = CU_MEMORYTYPE_HOST;
	cuCopy2d.srcHost = cpSrc;
	cuCopy2d.srcPitch = srcStride;
	cuCopy2d.dstMemoryType = CU_MEMORYTYPE_DEVICE;
	cuCopy2d.dstDevice = devicePtr;
	cuCopy2d.dstPitch = strideInfo.strideInfo[0].strideXInBytes;
	cuCopy2d.Height = height;

	CUresult cuResult = cuMemcpy2D(&cuCopy2d);
	if(cuResult != CUDA_SUCCESS)
	{
		popContext(a_errorString);
		a_errorString = std::string("Failed to upload luma frame to CUDA buffer. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(cuResult);
		return false;
	}

	return popContext(a_errorString);
}

//==============================================================================

bool NVidiaOpticalFlow::loadYUV420toBuffer(const VSFrameRef * a_cpFrame,
	NvOFGPUBufferHandle a_buffer, std::string & a_errorString)
{
	const size_t width = m_sourceVideoInfo.width;
	const size_t height = m_sourceVideoInfo.height;
	const size_t tempBufferYSize = width * height;
	const size_t tempBufferUVSize = width * height / 2;
	const size_t tempBufferSize = tempBufferYSize + tempBufferUVSize;

	CUDAHostVector<uint8_t> tempBuffer(tempBufferSize, 0);

	const int yStride = m_cpVSAPI->getStride(a_cpFrame, PLANE_Y);
	const uint8_t * cpYRow = m_cpVSAPI->getReadPtr(a_cpFrame, PLANE_Y);
	uint8_t * pDest = tempBuffer.data();

	for(size_t h = 0; h < height; ++h)
	{
		memcpy(pDest, cpYRow, width);
		cpYRow += yStride;
		pDest += width;
	}

	const int uStride = m_cpVSAPI->getStride(a_cpFrame, PLANE_U);
	const uint8_t * cpURow = m_cpVSAPI->getReadPtr(a_cpFrame, PLANE_U);
	const int vStride = m_cpVSAPI->getStride(a_cpFrame, PLANE_V);
	const uint8_t * cpVRow = m_cpVSAPI->getReadPtr(a_cpFrame, PLANE_V);

	for(size_t h = 0; h < (height / 2); ++h)
	{
		for(size_t w = 0; w < (width / 2); ++w)
		{
			*pDest = cpURow[w];
			pDest++;
			*pDest = cpVRow[w];
			pDest++;
		}
		cpURow += uStride;
		cpVRow += vStride;
	}

	if(!pushContext(a_errorString))
		return false;

	CUdeviceptr devicePtr = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetCUdeviceptr(a_buffer);

	NV_OF_CUDA_BUFFER_STRIDE_INFO strideInfo{};
	NV_OF_STATUS nvResult = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetStrideInfo(
		a_buffer, &strideInfo);
	if(nvResult != NV_OF_SUCCESS)
	{
		popContext(a_errorString);
		a_errorString = std::string("Failed to get input buffer stride info. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(nvResult);
		return false;
	}

	CUDA_MEMCPY2D cuCopy2d{0};
	cuCopy2d.WidthInBytes = width;
	cuCopy2d.srcMemoryType = CU_MEMORYTYPE_HOST;
	cuCopy2d.srcHost = tempBuffer.data();
	cuCopy2d.srcPitch = width;
	cuCopy2d.dstMemoryType = CU_MEMORYTYPE_DEVICE;
	cuCopy2d.dstDevice = devicePtr;
	cuCopy2d.dstPitch = strideInfo.strideInfo[0].strideXInBytes;
	cuCopy2d.Height = height + height / 2;

	CUresult cuResult = cuMemcpy2D(&cuCopy2d);
	if(cuResult != CUDA_SUCCESS)
	{
		popContext(a_errorString);
		a_errorString = std::string("Failed to upload YUV420 frame to CUDA buffer. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(cuResult);
		return false;
	}

	return popContext(a_errorString);
}

//==============================================================================

bool NVidiaOpticalFlow::loadRGBtoBuffer(const VSFrameRef * a_cpFrame,
	NvOFGPUBufferHandle a_buffer, std::string & a_errorString)
{
	const size_t width = m_sourceVideoInfo.width;
	const size_t height = m_sourceVideoInfo.height;
	const size_t widthInBytes = width * sizeof(RGBA32);
	
	CUDAHostVector<RGBA32> tempBuffer(width * height);

	const int srcStride = m_cpVSAPI->getStride(a_cpFrame, PLANE_R);
	const uint8_t * cpRRow = m_cpVSAPI->getReadPtr(a_cpFrame, PLANE_R);
	const uint8_t * cpGRow = m_cpVSAPI->getReadPtr(a_cpFrame, PLANE_G);
	const uint8_t * cpBRow = m_cpVSAPI->getReadPtr(a_cpFrame, PLANE_B);

	RGBA32 * pDest = tempBuffer.data();

	for(size_t h = 0; h < height; ++h)
	{
		for(size_t w = 0; w < width; ++w)
		{
			*pDest = RGBA32{cpRRow[w], cpGRow[w], cpBRow[w], 0xFF};
			pDest++;
		}
		cpRRow += srcStride;
		cpGRow += srcStride;
		cpBRow += srcStride;
	}

	if(!pushContext(a_errorString))
		return false;

	CUdeviceptr devicePtr = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetCUdeviceptr(a_buffer);

	NV_OF_CUDA_BUFFER_STRIDE_INFO strideInfo{};
	NV_OF_STATUS nvResult = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetStrideInfo(
		a_buffer, &strideInfo);
	if(nvResult != NV_OF_SUCCESS)
	{
		popContext(a_errorString);
		a_errorString = std::string("Failed to get input buffer stride info. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(nvResult);
		return false;
	}

	CUDA_MEMCPY2D cuCopy2d{0};
	cuCopy2d.WidthInBytes = widthInBytes;
	cuCopy2d.srcMemoryType = CU_MEMORYTYPE_HOST;
	cuCopy2d.srcHost = tempBuffer.data();
	cuCopy2d.srcPitch = widthInBytes;
	cuCopy2d.dstMemoryType = CU_MEMORYTYPE_DEVICE;
	cuCopy2d.dstDevice = devicePtr;
	cuCopy2d.dstPitch = strideInfo.strideInfo[0].strideXInBytes;
	cuCopy2d.Height = height;

	CUresult cuResult = cuMemcpy2D(&cuCopy2d);
	if(cuResult != CUDA_SUCCESS)
	{
		popContext(a_errorString);
		a_errorString = std::string("Failed to upload RGB frame to CUDA buffer. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(cuResult);
		return false;
	}

	return popContext(a_errorString);
}

//==============================================================================

bool NVidiaOpticalFlow::downloadFlow(VSFrameRef * a_pFrame, std::string & a_errorString)
{
	if(!pushContext(a_errorString))
		return false;

	const size_t width = m_sourceVideoInfo.width;
	const size_t height = m_sourceVideoInfo.height;

	size_t flowTempBufferSize = width * height;
	size_t flowTempBufferStride = width * sizeof(NV_OF_FLOW_VECTOR);
	CUDAHostVector<NV_OF_FLOW_VECTOR> flowTempBuffer(flowTempBufferSize, {0});
	CUDAHostVector<uint8_t> costTempBuffer(m_getCost ? width * height : 0, 0);

	CUdeviceptr flowDevicePtr = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetCUdeviceptr(m_hFlow);

	NV_OF_CUDA_BUFFER_STRIDE_INFO strideInfo{};
	NV_OF_STATUS nvResult = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetStrideInfo(
		m_hFlow, &strideInfo);
	if(nvResult != NV_OF_SUCCESS)
	{
		popContext(a_errorString);
		a_errorString = std::string("Failed to get flow buffer stride info. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(nvResult);
		return false;
	}

	CUDA_MEMCPY2D cuCopy2d{0};
	cuCopy2d.WidthInBytes = flowTempBufferStride;
	cuCopy2d.dstMemoryType = CU_MEMORYTYPE_HOST;
	cuCopy2d.dstHost = flowTempBuffer.data();
	cuCopy2d.dstPitch = flowTempBufferStride;
	cuCopy2d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
	cuCopy2d.srcDevice = flowDevicePtr;
	cuCopy2d.srcPitch = strideInfo.strideInfo[0].strideXInBytes;
	cuCopy2d.Height = height;

	CUresult cuResult = cuMemcpy2D(&cuCopy2d);
	if(cuResult != CUDA_SUCCESS)
	{
		popContext(a_errorString);
		a_errorString = std::string("Failed to download flow from CUDA buffer. ") +
			NVidiaOpticalFlowAPI::getErrorDescription(cuResult);
		return false;
	}

	if(m_getCost)
	{
		CUdeviceptr costDevicePtr = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetCUdeviceptr(m_hCost);
		nvResult = NVidiaOpticalFlowAPI::getAPI()->nvOFGPUBufferGetStrideInfo(
			m_hCost, &strideInfo);
		if(nvResult != NV_OF_SUCCESS)
		{
			popContext(a_errorString);
			a_errorString = std::string("Failed to get cost buffer stride info. ") +
				NVidiaOpticalFlowAPI::getErrorDescription(nvResult);
			return false;
		}

		cuCopy2d.WidthInBytes = width;
		cuCopy2d.dstHost = costTempBuffer.data();
		cuCopy2d.dstPitch = width;
		cuCopy2d.srcDevice = costDevicePtr;
		cuCopy2d.srcPitch = strideInfo.strideInfo[0].strideXInBytes;

		cuResult = cuMemcpy2D(&cuCopy2d);
		if(cuResult != CUDA_SUCCESS)
		{
			popContext(a_errorString);
			a_errorString = std::string("Failed to download cost from CUDA buffer. ") +
				NVidiaOpticalFlowAPI::getErrorDescription(cuResult);
			return false;
		}
	}

	if(!popContext(a_errorString))
		return false;

	const float COST_NORM = 255.0f;

	const int costStride = m_cpVSAPI->getStride(a_pFrame, PLANE_COST);
	uint8_t * pDestRow = m_cpVSAPI->getWritePtr(a_pFrame, PLANE_COST);
	ZeroMemory(pDestRow, costStride * height);
	if(m_getCost)
	{
		const uint8_t * pSrcRow = costTempBuffer.data();
		for(size_t h = 0; h < height; ++h)
		{
			float * pFloatRow = (float *)pDestRow;
			for(size_t w = 0; w < width; ++w)
				pFloatRow[w] = (float)pSrcRow[w] / COST_NORM;
			pDestRow += costStride;
			pSrcRow += width;
		}
	}

	const float VEC_LEN_DENOM = float(1 << 5);

	const int vecXStride = m_cpVSAPI->getStride(a_pFrame, PLANE_VEC_X);
	const int vecYStride = m_cpVSAPI->getStride(a_pFrame, PLANE_VEC_Y);
	uint8_t * pVecXRow = m_cpVSAPI->getWritePtr(a_pFrame, PLANE_VEC_X);
	uint8_t * pVecYRow = m_cpVSAPI->getWritePtr(a_pFrame, PLANE_VEC_Y);
	const NV_OF_FLOW_VECTOR * cpSrcRow = flowTempBuffer.data();
	for(size_t h = 0; h < height; ++h)
	{
		float * pFloatVecXRow = (float *)pVecXRow;
		float * pFloatVecYRow = (float *)pVecYRow;
		for(size_t w = 0; w < width; ++w)
		{
			pFloatVecXRow[w] = (float)cpSrcRow[w].flowx / VEC_LEN_DENOM;
			pFloatVecYRow[w] = (float)cpSrcRow[w].flowy / VEC_LEN_DENOM;
		}
		pVecXRow += vecXStride;
		pVecYRow += vecYStride;
		cpSrcRow += width;
	}

	return true;
}

//==============================================================================
