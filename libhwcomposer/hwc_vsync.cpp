/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012, The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are
 * retained for attribution purposes only.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// WARNING : Excessive logging, if VSYNC_DEBUG enabled
#define VSYNC_DEBUG 0

#include <utils/Log.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <gralloc_priv.h>
#include <fb_priv.h>
#include <linux/msm_mdp.h>
#include "hwc_utils.h"
#include "external.h"
#include "string.h"

#define PAGE_SIZE 4096

namespace qhwc {

static void *vsync_loop(void *param)
{
	ALOGI("Inside vsync loop");
    const char* vsync_timestamp_fb0 = "/sys/class/graphics/fb0/vsync_event";
    const char* vsync_timestamp_fb1 = "/sys/class/graphics/fb1/vsync_event";

    ALOGI("checkpoint 1");
    hwc_context_t * ctx = reinterpret_cast<hwc_context_t *>(param);
    ALOGI("checkpoint 2");
    private_module_t* m = reinterpret_cast<private_module_t*>(
                ctx->mFbDev->common.module);
    char thread_name[64] = "hwcVsyncThread";
    ALOGI("checkpoint 3");
    prctl(PR_SET_NAME, (unsigned long) &thread_name, 0, 0, 0);
    ALOGI("checkpoint 4");
    setpriority(PRIO_PROCESS, 0,
                HAL_PRIORITY_URGENT_DISPLAY + ANDROID_PRIORITY_MORE_FAVORABLE);

    const int MAX_DATA = 64;
    const int MAX_RETRY_COUNT = 100;
    static char vdata[MAX_DATA];

    uint64_t cur_timestamp=0;
    ssize_t len = -1;
    int fb0_timestamp = -1; // fb0 file for primary
    int fb1_timestamp = -1; // fb1 file for secondary(HDMI)
    int ret = 0;
    bool fb1_vsync = false;
    int fb_read = -1; // file used for reading(can be fb0 of fb1)
    bool enabled = false;

    ALOGI("checkpoint 5");
    
    // Open the primary display vsync_event sysfs node
    fb0_timestamp = open(vsync_timestamp_fb0, O_RDONLY);
    ALOGI("checkpoint 6");
    if (fb0_timestamp < 0) {
        ALOGE("FATAL:%s:not able to open file:%s, %s",  __FUNCTION__,
               vsync_timestamp_fb0, strerror(errno));
        return NULL;
    }
        ALOGI("checkpoint 7");
    // Always set to primary display fd
    fb_read = fb0_timestamp;
        ALOGI("checkpoint 8");
    // Open the secondary display vsync_event sysfs node
    if(ctx->mMDP.hasOverlay) {
		    ALOGI("checkpoint 9");
        fb1_timestamp = open(vsync_timestamp_fb1, O_RDONLY);
            ALOGI("checkpoint 10");
        if (fb1_timestamp < 0) {
			    ALOGI("checkpoint 11");
            ALOGE("FATAL:%s:not able to open file:%s, %s",  __FUNCTION__,
                                        vsync_timestamp_fb1, strerror(errno));
            return NULL;
        }
    }


    /* Currently read vsync timestamp from drivers
       e.g. VSYNC=41800875994
    */
    ALOGI("checkpoint 12");
    hwc_procs* proc = (hwc_procs*)ctx->device.reserved_proc[0];

    do {
		    //dont' log. this is an infinite loop >_<
		    //ALOGI("checkpoint 13");
    usleep(16000);
        //dont' log. this is an infinite loop >_<
        //ALOGI("checkpoint 14");
    ctx->proc->vsync(ctx->proc, 0, systemTime());
    } while (true);

    ALOGI("checkpoint 15");
    if(fb0_timestamp > 0)
        close(fb0_timestamp);
        ALOGI("checkpoint 16");
    fb0_timestamp = -1;
        ALOGI("checkpoint 17");
    if(ctx->mMDP.hasOverlay) {
        if(fb1_timestamp > 0)
            close(fb1_timestamp);
        fb1_timestamp = -1;
    }
    return NULL;
}

void init_vsync_thread(hwc_context_t* ctx)
{
    pthread_t vsync_thread;
    ALOGI("Initializing VSYNC Thread");
    pthread_create(&vsync_thread, NULL, vsync_loop, (void*) ctx);
}

}; //namespace
