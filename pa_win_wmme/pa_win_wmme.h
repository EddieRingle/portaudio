#ifndef PA_WIN_WMME_H
#define PA_WIN_WMME_H

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/*
 *
 * PortAudio Portable Real-Time Audio Library
 * MME specific extensions
 *
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */


#include "portaudio.h"

#define PaWinMmeUseLowLevelLatencyParameters            (0x01)
#define PaWinMmeUseMultipleDevices                      (0x02)  /* use mme specific multiple device feature */

/* by default, the mme implementation boosts the process priority class to
    HIGH_PRIORITY_CLASS. This flag disables that priority boost */
#define PaWinMmeNoHighPriorityProcessClass              (0x03)

/* by default, the mme implementation drops the processing thread's priority
    to THREAD_PRIORITY_NORMAL and sleeps the thread if the CPU load exceeds 100% */
#define PaWinMmeDontThrottleOverloadedProcessingThread  (0x04)

/* by default, the mme implementation sets the processing thread's priority to
    THREAD_PRIORITY_HIGHEST. This flag sets the priority to
    THREAD_PRIORITY_TIME_CRITICAL instead. Note that this has the potential
    to freeze the machine, especially when used in combination with
    PaWinMmeDontThrottleOverloadedProcessingThread */
#define PaWinMmeUseTimeCriticalThreadPriority           (0x05)

typedef struct PaWinMmeDeviceAndChannelCount{
    PaDeviceIndex device;
    int channelCount;
}PaWinMmeDeviceAndChannelCount;


typedef struct PaWinMmeStreamInfo{
    unsigned long size;             /* sizeof(PaWinMmeStreamInfo) */
    PaHostApiTypeId hostApiType;    /* paMME */
    unsigned long version;          /* 1 */

    unsigned long flags;

    /* low-level latency setting support
        These settings control the number and size of host buffers in order
        to set latency. They will be used instead of the generic parameters
        to Pa_OpenStream() if flags contains the PaWinMmeUseLowLevelLatencyParameters
        flag.
    */
    unsigned long framesPerBuffer;
    unsigned long numBuffers;  

    /* multiple devices per direction support
        If flags contains the PaWinMmeUseMultipleDevices flag,
        this functionality will be used, otherwise the device parameter to
        Pa_OpenStream() will be used instead.
        If devices are specified here, the corresponding device parameter
        to Pa_OpenStream() should be set to paUseHostApiSpecificDeviceSpecification,
        otherwise an paInvalidDevice error will result.
        The total number of channels accross all specified devices
        must agree with the corresponding channelCount parameter to
        Pa_OpenStream() otherwise a paInvalidChannelCount error will result.
    */
    PaWinMmeDeviceAndChannelCount *devices;
    unsigned long deviceCount;

}PaWinMmeStreamInfo;


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PA_WIN_WMME_H */
