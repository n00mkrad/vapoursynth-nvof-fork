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

#ifndef NVIDIA_OPTICAL_FLOW_API_H
#define NVIDIA_OPTICAL_FLOW_API_H

#include "common.h"

#pragma warning(push)
#pragma warning( disable : 26812 )
#include "nvOpticalFlowCuda.h"
#pragma warning(pop)

#include <string>
#include <mutex>

class NVidiaOpticalFlowAPI
{
public:
	static bool create(std::string & a_errorString);
	static bool destroy(std::string & a_errorString);
	static const NV_OF_CUDA_API_FUNCTION_LIST * getAPI();
	static std::string apiVersionString(uint16_t a_apiVersion);
	static std::string getErrorDescription(NV_OF_STATUS a_error);
	static std::string getErrorDescription(CUresult a_error);
private:
	static HMODULE m_hLibNVOF;
	static NV_OF_CUDA_API_FUNCTION_LIST m_API;
	static size_t m_referencesNumber;
	static std::mutex m_lock;

	static bool initNvofLibrary(std::string & a_errorString);
};

#endif
