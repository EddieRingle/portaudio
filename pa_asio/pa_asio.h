#ifndef PA_ASIO_H
#define PA_ASIO_H
/*
 *
 * PortAudio Portable Real-Time Audio Library
 * ASIO specific extensions
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


#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


/** Retrieve legal latency settings for the specificed device, in samples.

 @param device The global index of the device about which the query is being made.
 @param minLatency A pointer to the location which will recieve the minimum latency value.
 @param maxLatency A pointer to the location which will recieve the maximum latency value.
 @param minLatency A pointer to the location which will recieve the preferred latency value.
 @param granularity A pointer to the location which will recieve the granularity. This value 
 determines which values between minLatency and maxLatency are available. ie the step size,
 if granularity is -1 then available latency settings are powers of two.

 @see ASIOGetBufferSize in the ASIO SDK.

 @todo This function should have a better name, any suggestions?
*/
PaError PaAsio_GetAvailableLatencyValues( PaDeviceIndex device,
		long *minLatency, long *maxLatency, long *preferredLatency, long *granularity );



#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PA_ASIO_H */
