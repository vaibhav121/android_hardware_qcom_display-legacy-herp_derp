/*
 * Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cutils/log.h>
#include <utils/RefBase.h>
#include <fcntl.h>
#include <dlfcn.h>
#include "gralloc_priv.h"
#include "alloc_controller.h"
#include "memalloc.h"
#include "ionalloc.h"
#ifdef USE_PMEM_ADSP
#include "pmemalloc.h"
#endif
#include "gr.h"
#include "comptype.h"

#ifdef VENUS_COLOR_FORMAT
#include <media/msm_media_info.h>
#else
#define VENUS_Y_STRIDE(args...) 0
#define VENUS_Y_SCANLINES(args...) 0
#define VENUS_BUFFER_SIZE(args...) 0
#endif

using namespace gralloc;
using namespace qdutils;
using android::sp;

const int GRALLOC_HEAP_MASK  =  GRALLOC_USAGE_PRIVATE_ADSP_HEAP      |
                                GRALLOC_USAGE_PRIVATE_UI_CONTIG_HEAP |
                                GRALLOC_USAGE_PRIVATE_SYSTEM_HEAP    |
                                GRALLOC_USAGE_PRIVATE_IOMMU_HEAP     |
                                GRALLOC_USAGE_PRIVATE_MM_HEAP        |
                                GRALLOC_USAGE_PRIVATE_WRITEBACK_HEAP |
                                GRALLOC_USAGE_PRIVATE_CAMERA_HEAP;

ANDROID_SINGLETON_STATIC_INSTANCE(AdrenoMemInfo);

//Common functions
static bool canFallback(int usage, bool triedSystem)
{
    // Fallback to system heap when alloc fails unless
    // 1. Alloc from system heap was already tried
    // 2. The heap type is requsted explicitly
    // 3. The heap type is protected
    // 4. The buffer is meant for external display only

    if(triedSystem)
        return false;
    if(usage & (GRALLOC_HEAP_MASK | GRALLOC_USAGE_PROTECTED))
        return false;
    if(usage & (GRALLOC_HEAP_MASK | GRALLOC_USAGE_PRIVATE_EXTERNAL_ONLY))
        return false;
    //Return true by default
    return true;
}

static bool useUncached(int usage)
{
    // System heaps cannot be uncached
    if(usage & GRALLOC_USAGE_PRIVATE_SYSTEM_HEAP)
        return false;
    if (usage & GRALLOC_USAGE_PRIVATE_UNCACHED)
        return true;
    return false;
}

//-------------- AdrenoMemInfo-----------------------//
AdrenoMemInfo::AdrenoMemInfo()
{
    libadreno_utils = ::dlopen("libadreno_utils.so", RTLD_NOW);
    if (libadreno_utils) {
        *(void **)&LINK_adreno_compute_padding = ::dlsym(libadreno_utils,
                                           "compute_surface_padding");
    }
}

AdrenoMemInfo::~AdrenoMemInfo()
{
    if (libadreno_utils) {
        ::dlclose(libadreno_utils);
    }
}

int AdrenoMemInfo::getStride(int width, int format)
{
    int stride = ALIGN(width, 32);
    // Currently surface padding is only computed for RGB* surfaces.
    if (format < 0x7) {
        int bpp = 4;
        switch(format)
        {
            case HAL_PIXEL_FORMAT_RGB_888:
                bpp = 3;
                break;
            case HAL_PIXEL_FORMAT_RGB_565:
            case HAL_PIXEL_FORMAT_RGBA_5551:
            case HAL_PIXEL_FORMAT_RGBA_4444:
                bpp = 2;
                break;
            default: break;
        }
        if ((libadreno_utils) && (LINK_adreno_compute_padding)) {
            int surface_tile_height = 1;   // Linear surface
            int raster_mode         = 0;   // Adreno unknown raster mode.
            int padding_threshold   = 512; // Threshold for padding surfaces.
            // the function below expects the width to be a multiple of
            // 32 pixels, hence we pass stride instead of width.
            stride = LINK_adreno_compute_padding(stride, bpp,
                                      surface_tile_height, raster_mode,
                                      padding_threshold);
        }
    } else {
        switch (format)
        {
            case HAL_PIXEL_FORMAT_YCrCb_420_SP_ADRENO:
            case HAL_PIXEL_FORMAT_RAW_SENSOR:
                stride = ALIGN(width, 32);
                break;
            case HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED:
                stride = ALIGN(width, 128);
                break;
            case HAL_PIXEL_FORMAT_NV12_ENCODEABLE:
            case HAL_PIXEL_FORMAT_YCbCr_420_SP:
            case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            case HAL_PIXEL_FORMAT_YV12:
            case HAL_PIXEL_FORMAT_YCbCr_422_SP:
            case HAL_PIXEL_FORMAT_YCrCb_422_SP:
                stride = ALIGN(width, 16);
                break;
            case HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS:
                stride = VENUS_Y_STRIDE(COLOR_FMT_NV12, width);
                break;
            case HAL_PIXEL_FORMAT_BLOB:
                stride = width;
                break;
            default: break;
        }
    }
    return stride;
}

//-------------- IAllocController-----------------------//
sp<IAllocController> IAllocController::sController = NULL;
sp<IAllocController> IAllocController::getInstance(void)
{
    if(sController == NULL) {
        sController = new IonController();
    }
    return sController;
}


//-------------- IonController-----------------------//
IonController::IonController()
{
    mIonAlloc = new IonAlloc();
#ifdef USE_PMEM_ADSP
    mPmemAlloc = new PmemAdspAlloc();
#endif
}

int IonController::allocate(alloc_data& data, int usage)
{
    int ionFlags = 0;
    int ret;
    bool noncontig = false;

    data.uncached = useUncached(usage);
    data.allocType = 0;

#ifdef USE_PMEM_ADSP
    if (usage & GRALLOC_USAGE_PRIVATE_ADSP_HEAP) {
        data.allocType |= private_handle_t::PRIV_FLAGS_USES_PMEM_ADSP;
        ret = mPmemAlloc->alloc_buffer(data);
        return ret;
    }
#endif

    if(usage & GRALLOC_USAGE_PRIVATE_UI_CONTIG_HEAP)
        ionFlags |= ION_HEAP(ION_SF_HEAP_ID);

    if(usage & GRALLOC_USAGE_PRIVATE_SYSTEM_HEAP) {
        ionFlags |= ION_HEAP(ION_SYSTEM_HEAP_ID);
        noncontig = true;
    }

    if(usage & GRALLOC_USAGE_PRIVATE_IOMMU_HEAP) {
        ionFlags |= ION_HEAP(ION_IOMMU_HEAP_ID);
        noncontig = true;
    }

    if(usage & GRALLOC_USAGE_PRIVATE_MM_HEAP)
        ionFlags |= ION_HEAP(ION_CP_MM_HEAP_ID);

    if(usage & GRALLOC_USAGE_PRIVATE_WRITEBACK_HEAP)
        ionFlags |= ION_HEAP(ION_CP_WB_HEAP_ID);

    if(usage & GRALLOC_USAGE_PRIVATE_CAMERA_HEAP)
        ionFlags |= ION_HEAP(ION_CAMERA_HEAP_ID);

    if(usage & GRALLOC_USAGE_PROTECTED && !noncontig)
        ionFlags |= ION_SECURE;

    if(usage & GRALLOC_USAGE_PRIVATE_DO_NOT_MAP)
        data.allocType  |=  private_handle_t::PRIV_FLAGS_NOT_MAPPED;
    else
        data.allocType  &=  ~(private_handle_t::PRIV_FLAGS_NOT_MAPPED);

    // if no flags are set, default to
    // SF + IOMMU heaps, so that bypass can work
    // we can fall back to system heap if
    // we run out.
    if(!ionFlags)
        ionFlags = ION_HEAP(ION_SF_HEAP_ID) | ION_HEAP(ION_IOMMU_HEAP_ID);

    data.flags = ionFlags;
    ret = mIonAlloc->alloc_buffer(data);

    // Fallback
    if(ret < 0 && canFallback(usage,
                              (ionFlags & ION_SYSTEM_HEAP_ID)))
    {
        ALOGW("Falling back to system heap");
        data.flags = ION_HEAP(ION_SYSTEM_HEAP_ID);
        noncontig = true;
        ret = mIonAlloc->alloc_buffer(data);
    }

    if(ret >= 0 ) {
        data.allocType |= private_handle_t::PRIV_FLAGS_USES_ION;
        if(noncontig)
            data.allocType |= private_handle_t::PRIV_FLAGS_NONCONTIGUOUS_MEM;
        if(ionFlags & ION_SECURE)
            data.allocType |= private_handle_t::PRIV_FLAGS_SECURE_BUFFER;
    }

    return ret;
}

sp<IMemAlloc> IonController::getAllocator(int flags)
{
    sp<IMemAlloc> memalloc = NULL;
    if (flags & private_handle_t::PRIV_FLAGS_USES_ION) {
        memalloc = mIonAlloc;
#ifdef USE_PMEM_ADSP
    } else if (flags & private_handle_t::PRIV_FLAGS_USES_PMEM_ADSP) {
        memalloc = mPmemAlloc;
#endif
    } else {
        ALOGE("%s: Invalid flags passed: 0x%x", __FUNCTION__, flags);
    }

    return memalloc;
}

size_t getBufferSizeAndDimensions(int width, int height, int format,
                                  int& alignedw, int &alignedh)
{
    size_t size;

    alignedw = AdrenoMemInfo::getInstance().getStride(width, format);
    alignedh = ALIGN(height, 32);
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            size = alignedw * alignedh * 4;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            size = alignedw * alignedh * 3;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RGBA_5551:
        case HAL_PIXEL_FORMAT_RGBA_4444:
        case HAL_PIXEL_FORMAT_RAW_SENSOR:
            size = alignedw * alignedh * 2;
            break;

            // adreno formats
        case HAL_PIXEL_FORMAT_YCrCb_420_SP_ADRENO:  // NV21
            size  = ALIGN(alignedw*alignedh, 4096);
            size += ALIGN(2 * ALIGN(width/2, 32) * ALIGN(height/2, 32), 4096);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED:   // NV12
            // The chroma plane is subsampled,
            // but the pitch in bytes is unchanged
            // The GPU needs 4K alignment, but the video decoder needs 8K
            size  = ALIGN( alignedw * alignedh, 8192);
            size += ALIGN( alignedw * ALIGN(height/2, 32), 8192);
            break;
        case HAL_PIXEL_FORMAT_NV12_ENCODEABLE:
        case HAL_PIXEL_FORMAT_YV12:
            if ((format == HAL_PIXEL_FORMAT_YV12) && ((width&1) || (height&1))) {
                ALOGE("w or h is odd for the YV12 format");
                return -EINVAL;
            }
            alignedh = height;
            if (HAL_PIXEL_FORMAT_NV12_ENCODEABLE == format) {
                // The encoder requires a 2K aligned chroma offset.
                size = ALIGN(alignedw*alignedh, 2048) +
                    (ALIGN(alignedw/2, 16) * (alignedh/2))*2;
            } else {
                size = alignedw*alignedh +
                    (ALIGN(alignedw/2, 16) * (alignedh/2))*2;
            }
            size = ALIGN(size, 4096);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            alignedh = height;
            size = ALIGN((alignedw*alignedh) + (alignedw* alignedh)/2, 4096);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
        case HAL_PIXEL_FORMAT_YCrCb_422_SP:
            if(width & 1) {
                ALOGE("width is odd for the YUV422_SP format");
                return -EINVAL;
            }
            alignedh = height;
            size = ALIGN(alignedw * alignedh * 2, 4096);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS:
            alignedh = VENUS_Y_SCANLINES(COLOR_FMT_NV12, height);
            size = VENUS_BUFFER_SIZE(COLOR_FMT_NV12, width, height);
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            if(height != 1) {
                ALOGE("%s: Buffers with format HAL_PIXEL_FORMAT_BLOB \
                      must have height==1 ", __FUNCTION__);
                return -EINVAL;
            }
            alignedh = height;
            alignedw = width;
            size = width;
            break;
        default:
            ALOGE("unrecognized pixel format: 0x%x", format);
            return -EINVAL;
    }

    return size;
}

// Allocate buffer from width, height and format into a
// private_handle_t. It is the responsibility of the caller
// to free the buffer using the free_buffer function
int alloc_buffer(private_handle_t **pHnd, int w, int h, int format, int usage)
{
    alloc_data data;
    int alignedw, alignedh;
    android::sp<gralloc::IAllocController> sAlloc =
        gralloc::IAllocController::getInstance();
    data.base = 0;
    data.fd = -1;
    data.offset = 0;
    data.size = getBufferSizeAndDimensions(w, h, format, alignedw, alignedh);
    data.align = getpagesize();
    data.uncached = useUncached(usage);
    int allocFlags = usage;

    int err = sAlloc->allocate(data, allocFlags);
    if (0 != err) {
        ALOGE("%s: allocate failed", __FUNCTION__);
        return -ENOMEM;
    }

    private_handle_t* hnd = new private_handle_t(data.fd, data.size,
                                                 data.allocType, 0, format,
                                                 alignedw, alignedh);
    hnd->base = (int) data.base;
    hnd->offset = data.offset;
    hnd->gpuaddr = 0;
    *pHnd = hnd;
    return 0;
}

void free_buffer(private_handle_t *hnd)
{
    android::sp<gralloc::IAllocController> sAlloc =
        gralloc::IAllocController::getInstance();
    if (hnd && hnd->fd > 0) {
        sp<IMemAlloc> memalloc = sAlloc->getAllocator(hnd->flags);
        memalloc->free_buffer((void*)hnd->base, hnd->size, hnd->offset, hnd->fd);
    }
    if(hnd)
        delete hnd;

}
