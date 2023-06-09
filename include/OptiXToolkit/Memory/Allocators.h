//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#pragma once

#include <OptiXToolkit/Memory/CudaCheck.h>

#include <cuda.h>

#include <cstring>

namespace otk {

const unsigned int HOST_DEVICE = 0xFFFFFFFF;

/// Host allocator using malloc
class HostAllocator
{
  public:
    void* allocate( size_t numBytes, CUstream /*dummy*/ = 0 ) { return malloc( numBytes ); }
    void free( void* ptr, CUstream /*dummy*/ = 0 ) { ::free( ptr ); }
    void set( void* ptr, int val, size_t numBytes, CUstream /*dummy*/ = 0 ) { memset( ptr, val, numBytes ); }
    bool allocationIsHandle() const { return false; }
};

/// Pinned host allocator using cuMallocHost
class PinnedAllocator
{
  public:
    void* allocate( size_t numBytes, CUstream /*dummy*/ = 0 )
    {
        void* result;
        OTK_MEMORY_CUDA_CHECK( cuMemAllocHost( &result, numBytes ) );
        return result;
    }
    void free( void* ptr, CUstream /*dummy*/ = 0 ) { OTK_MEMORY_CUDA_CHECK( cuMemFreeHost( ptr ) ); }
    void set( void* ptr, int val, size_t numBytes, CUstream /*dummy*/ = 0 ) { memset( ptr, val, numBytes ); }
    bool allocationIsHandle() const { return false; }
};

/// Device allocator using cuMemAlloc
class DeviceAllocator
{
  public:
    DeviceAllocator()
    {
        // Record current CUDA context.
        OTK_MEMORY_CUDA_CHECK( cuCtxGetCurrent( &m_context ) );
    }

    void* allocate( size_t numBytes, CUstream /*dummy*/ = 0 )
    {
        checkCudaContext( m_context );
        if( numBytes == 0 )
            return nullptr;  // cuMemAlloc does not handle this.
        void* result;
        OTK_MEMORY_CUDA_CHECK( cuMemAlloc( reinterpret_cast<CUdeviceptr*>( &result ), numBytes ) );
        return result;
    }

    void free( void* ptr, CUstream /*dummy*/ = 0 )
    {
        checkCudaContext( m_context );
        OTK_MEMORY_CUDA_CHECK( cuMemFree( reinterpret_cast<CUdeviceptr>( ptr ) ) );
    }

    void set( void* ptr, int val, size_t numBytes, CUstream /*dummy*/ = 0 )
    {
        checkCudaContext( m_context );
        OTK_MEMORY_CUDA_CHECK( cuMemsetD8( reinterpret_cast<CUdeviceptr>( ptr ), static_cast<unsigned char>( val ), numBytes ) );
    }

    bool allocationIsHandle() const { return false; }

  private:
    CUcontext m_context;
};

/// Async device allocator using cuMemAllocAsync
class DeviceAsyncAllocator
{
  public:
    DeviceAsyncAllocator()
    {
        // Record current CUDA context.
        OTK_MEMORY_CUDA_CHECK( cuCtxGetCurrent( &m_context ) );
    }

    void* allocate( size_t numBytes, CUstream stream = 0 )
    {
        checkCudaContext( m_context );
        if( numBytes == 0 )
            return nullptr;  // cuMemAlloc does not handle this.
        void* result;
#if OTK_USE_CUDA_MEMORY_POOLS
        OTK_MEMORY_CUDA_CHECK( cuMemAllocAsync( reinterpret_cast<CUdeviceptr*>( &result ), numBytes, stream ) );
#else
        OTK_MEMORY_CUDA_CHECK( cuMemAlloc( reinterpret_cast<CUdeviceptr*>( &result ), numBytes ) );
#endif
        return result;
    }

    void free( void* ptr, CUstream stream = 0 )
    {
        checkCudaContext( m_context );
#if OTK_USE_CUDA_MEMORY_POOLS
        OTK_MEMORY_CUDA_CHECK( cuMemFreeAsync( reinterpret_cast<CUdeviceptr>( ptr ), stream ) );
#else
        OTK_MEMORY_CUDA_CHECK( cuMemFree( reinterpret_cast<CUdeviceptr>( ptr ) ) );
#endif
    }

    void set( void* ptr, int val, size_t numBytes, CUstream stream = 0 )
    {
        checkCudaContext( m_context );
        OTK_MEMORY_CUDA_CHECK( cuMemsetD8Async( reinterpret_cast<CUdeviceptr>( ptr ), static_cast<unsigned char>( val ), numBytes, stream ) );
    }

    bool allocationIsHandle() const { return false; }

  private:
    CUcontext m_context;
};

/// Texture tile allocator using cuMemCreate
class TextureTileAllocator
{
  public:
    TextureTileAllocator()
        : m_allocationProp( makeAllocationProp() )
    {
        // Record current CUDA context.
        OTK_MEMORY_CUDA_CHECK( cuCtxGetCurrent( &m_context ) );
    }

    void* allocate( size_t numBytes, CUstream /*dummy*/ = 0 )
    {
        checkCudaContext( m_context );
        CUmemGenericAllocationHandle handle;
        OTK_MEMORY_CUDA_CHECK( cuMemCreate( &handle, numBytes, &m_allocationProp, 0U ) );
        return reinterpret_cast<void*>( handle );
    }

    void free( void* handle, CUstream /*dummy*/ = 0 )
    {
        checkCudaContext( m_context );
        OTK_MEMORY_CUDA_CHECK( cuMemRelease( reinterpret_cast<CUmemGenericAllocationHandle>( handle ) ) );
    }

    bool allocationIsHandle() const { return true; }  // The allocation is not a pointer that can be incremented

    static CUmemAllocationProp makeAllocationProp()
    {
        CUdevice device;
        OTK_MEMORY_CUDA_CHECK( cuCtxGetDevice( &device ) );

        CUmemAllocationProp prop{};
        prop.type             = CU_MEM_ALLOCATION_TYPE_PINNED;
        prop.location         = {CU_MEM_LOCATION_TYPE_DEVICE, static_cast<int>( device )};
        prop.allocFlags.usage = CU_MEM_CREATE_USAGE_TILE_POOL;
        return prop;
    }

    static size_t getRecommendedAllocationSize()
    {
        size_t              size;
        CUmemAllocationProp prop( makeAllocationProp() );
        OTK_MEMORY_CUDA_CHECK( cuMemGetAllocationGranularity( &size, &prop, CU_MEM_ALLOC_GRANULARITY_RECOMMENDED ) );
        return size ? size : 8 << 20;  // get the recommended size, or 8MB if it returns 0
    }

  private:
    CUcontext           m_context;
    CUmemAllocationProp m_allocationProp{};
};

}  // namespace otk
