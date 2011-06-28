#ifndef PA_WIN_WDMKS_H
#define PA_WIN_WDMKS_H
/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * WDM/KS specific extensions
 *
 * Copyright (c) 1999-2007 Ross Bencina and Phil Burk
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however, 
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also 
 * requested that these non-binding requests be included along with the 
 * license above.
 */

/** @file
 @ingroup public_header
 @brief WDM Kernel Streaming-specific PortAudio API extension header file.
*/


#include "portaudio.h"

#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

    /* Setup flags */
    typedef enum PaWinWDMKSFlags
    {
        /* Makes WDMKS use the supplied latency figures instead of relying on the frame size reported
           by the WaveCyclic device. Use at own risk! */
        paWinWDMKSOverrideFramesize         = (1 << 0),

        /* Makes WDMKS not apply a timeout in processing thread. Normally if a USB unit is unplugged, it will stop seding
           packets, which will be detected by a timeout. Setting this flag prohibits that */
        paWinWDMKSDisableTimeoutInProcessingThread = (1 << 1),

    } PaWinWDMKSFlags;

    typedef struct PaWinWDMKSInfo{
        unsigned long size;             /**< sizeof(PaWinWDMKSInfo) */
        PaHostApiTypeId hostApiType;    /**< paWDMKS */
        unsigned long version;          /**< 1 */
        unsigned long flags;
    } PaWinWDMKSInfo;

    typedef enum PaWDMKSType
    {
        Type_kNotUsed,
        Type_kWaveCyclic,
        Type_kWaveRT,
        Type_kCnt,
    } PaWDMKSType;

    typedef enum PaWDMKSSubType
    {
        SubType_kNone,
        SubType_kNotification,
        SubType_kPolled,
        SubType_kCnt,
    } PaWDMKSSubType;

    typedef struct PaWinWDMKSDeviceInfo {
        wchar_t filterName[MAX_PATH];     /**< KS filter path in Unicode! */
        wchar_t topologyName[MAX_PATH];   /**< Topology filter path in Unicode! */
        PaWDMKSType streamingType;
        PaWDMKSSubType streamingSubType;
        int endpointPinId;                /**< Endpoint pin ID (on topology filter 
                                               if topologyName is not empty) */
        int muxNodeId;                    /**< Mux node on topology filter (or -1 if 
                                               not used) */
        unsigned channels;                /**< No of channels the device is opened with */
    } PaWinWDMKSDeviceInfo;

    typedef struct PaWDMKSSpecificStreamInfo {
        PaWinWDMKSDeviceInfo input;
        PaWinWDMKSDeviceInfo output;
    } PaWDMKSSpecificStreamInfo;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PA_WIN_DS_H */                                  
