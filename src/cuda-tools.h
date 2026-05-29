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

#ifndef CUDA_TOOLS_H
#define CUDA_TOOLS_H

#include <cuda.h>

#include <vector>

template <typename T>
struct CUDAHostAllocator
{
	using value_type = T;

	CUDAHostAllocator() = default;

	template <typename U>
	CUDAHostAllocator(const CUDAHostAllocator<U> &)
	{}

	T * allocate(std::size_t n)
	{
		void * pointer = nullptr;
		cuMemHostAlloc(&pointer, n * sizeof(T), CU_MEMHOSTALLOC_PORTABLE);
		return static_cast<T *>(pointer);
	}

	void deallocate(T * p, std::size_t n)
	{
		(void)n;
		cuMemFreeHost(p);
	}
};

template <typename T, typename U>
bool operator==(const CUDAHostAllocator<T> &, const CUDAHostAllocator<U> &)
{
	return true;
}

template <typename T, typename U>
bool operator!=(const CUDAHostAllocator<T> &, const CUDAHostAllocator<U> &)
{
	return false;
}

template<typename T>
using CUDAHostVector = std::vector<T, CUDAHostAllocator<T> >;

#endif
