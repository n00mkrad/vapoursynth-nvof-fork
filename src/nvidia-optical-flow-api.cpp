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

#include "nvidia-optical-flow-api.h"

#include <cassert>
#include <map>

#ifdef _WIN32
	const wchar_t NVOF_LIBRARY_NAME[] =
	#ifdef _WIN64
		L"nvofapi64.dll";
	#else
		L"nvofapi.dll";
	#endif
#else
	const char NVOF_LIBRARY_NAME[] = "libnvidia-opticalflow.so.1";
#endif

using PFNvOFGetMaxSupportedApiVersion = NV_OF_STATUS (NVOFAPI *)(uint32_t *);
using PFNvOFAPICreateInstanceCuda = NV_OF_STATUS (NVOFAPI *)(uint32_t,
	NV_OF_CUDA_API_FUNCTION_LIST *);

//=============================================================================

HMODULE NVidiaOpticalFlowAPI::m_hLibNVOF = nullptr;
NV_OF_CUDA_API_FUNCTION_LIST NVidiaOpticalFlowAPI::m_API = { nullptr };
size_t NVidiaOpticalFlowAPI::m_referencesNumber = 0;
std::mutex NVidiaOpticalFlowAPI::m_lock;

//=============================================================================

bool NVidiaOpticalFlowAPI::create(std::string & a_errorString)
{
	std::lock_guard<std::mutex> lock(m_lock);
	
	if(m_referencesNumber > 0)
	{
		assert(m_hLibNVOF);
		assert(m_API.nvCreateOpticalFlowCuda);
		m_referencesNumber++;
		return true;
	}

	m_hLibNVOF = loadLibrary(NVOF_LIBRARY_NAME);
	if(m_hLibNVOF == nullptr)
	{
		a_errorString = "Failed to load the NVIDIA Optical Flow library";
		return false;
	}

	if(!initNvofLibrary(a_errorString))
	{
		freeLibrary(m_hLibNVOF);
		return false;
	}

	CUresult cuResult = cuInit(0);
	if(cuResult != CUDA_SUCCESS)
	{
		a_errorString = getErrorDescription(cuResult);
		freeLibrary(m_hLibNVOF);
		return false;
	}

	m_referencesNumber++;

	return true;
}

//=============================================================================

bool NVidiaOpticalFlowAPI::destroy(std::string & a_errorString)
{
	std::lock_guard<std::mutex> lock(m_lock);
	if(m_referencesNumber == 0)
	{
		a_errorString = "Cannot destroy API. There are no active API references.";
		return false;
	}

	m_referencesNumber--;

	if(m_referencesNumber > 0)
		return true;

	freeLibrary(m_hLibNVOF);

	return true;
}

const NV_OF_CUDA_API_FUNCTION_LIST * NVidiaOpticalFlowAPI::getAPI()
{
	return &m_API;
}

//=============================================================================

std::string NVidiaOpticalFlowAPI::apiVersionString(uint16_t a_apiVersion)
{
	std::string versionString;
	versionString += std::to_string(a_apiVersion >> 4);
	versionString += ".";
	versionString += std::to_string(a_apiVersion & ((1 << 4) - 1));
	return versionString;
}

//=============================================================================

std::string NVidiaOpticalFlowAPI::getErrorDescription(NV_OF_STATUS a_error)
{
	static std::map<NV_OF_STATUS, std::string> descriptions = {
		{NV_OF_SUCCESS, "Success."},
		{NV_OF_ERR_OF_NOT_AVAILABLE, "HW Optical flow functionality is not supported."},
		{NV_OF_ERR_UNSUPPORTED_DEVICE, "Unsupported device."},
		{NV_OF_ERR_DEVICE_DOES_NOT_EXIST, "Device is no longer available."},
		{NV_OF_ERR_INVALID_PTR, "Invalid pointer passed to NVOF API."},
		{NV_OF_ERR_INVALID_PARAM, "Invalid parameter passed to NVOF API."},
		{NV_OF_ERR_INVALID_CALL, "NVOF API call made in wrong sequence."},
		{NV_OF_ERR_INVALID_VERSION, "Invalid struct version used."},
		{NV_OF_ERR_OUT_OF_MEMORY, "Out of memory."},
		{NV_OF_ERR_NOT_INITIALIZED, "Optical flow session is not initialized."},
		{NV_OF_ERR_UNSUPPORTED_FEATURE, "Feature not supported."},
		{NV_OF_ERR_GENERIC, "Unknown internal error."},
	};

	std::map<NV_OF_STATUS, std::string>::const_iterator it = descriptions.find(a_error);
	if(it != descriptions.end())
		return it->second;

	return "Invalid error code.";
}

//=============================================================================

std::string NVidiaOpticalFlowAPI::getErrorDescription(CUresult a_error)
{
	std::string description;
	const char * pString = nullptr;
	cuGetErrorName(a_error, &pString);
	description += pString;
	description += ": ";
	cuGetErrorString(a_error, &pString);
	description += pString;
	return description;
}

//=============================================================================

bool NVidiaOpticalFlowAPI::initNvofLibrary(std::string & a_errorString)
{
	PFNvOFGetMaxSupportedApiVersion getSupportedApiVersion = (PFNvOFGetMaxSupportedApiVersion)
		getFunctionAddress(m_hLibNVOF, "NvOFGetMaxSupportedApiVersion");
	if(getSupportedApiVersion == nullptr)
	{
		a_errorString = "No function \"NvOFGetMaxSupportedApiVersion\" found "
			"in the NVIDIA Optical Flow library";
		return false;
	}

	uint32_t nvofVersion = 0;
	NV_OF_STATUS nvResult = getSupportedApiVersion(&nvofVersion);
	if(nvResult != NV_OF_SUCCESS)
	{
		a_errorString = "Failed to get the supported NVOF API version. " +
			getErrorDescription(nvResult);
		return false;
	}

	if(nvofVersion < NV_OF_API_VERSION)
	{
		a_errorString = std::string("Supported NVOF API version (") +
			apiVersionString(nvofVersion) + ") is lower than requested (" +
			apiVersionString(NV_OF_API_VERSION) + ").";
		return false;
	}

	PFNvOFAPICreateInstanceCuda createAPI = (PFNvOFAPICreateInstanceCuda)
		getFunctionAddress(m_hLibNVOF, "NvOFAPICreateInstanceCuda");
	if(createAPI == nullptr)
	{
		a_errorString = "No function \"NvOFAPICreateInstanceCuda\" found "
			"in the NVIDIA Optical Flow library";
		return false;
	}

	nvResult = createAPI(NV_OF_API_VERSION, &m_API);
	if(nvResult != NV_OF_SUCCESS)
	{
		a_errorString = "Failed to initialize NVIDIA Optical Flow API. " +
			getErrorDescription(nvResult);
		return false;
	}

	assert(m_API.nvOFGetCaps);

	return true;
}

//=============================================================================
