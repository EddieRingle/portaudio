#ifndef PA_HOSTAPI_H
#define PA_HOSTAPI_H
/*
 *
 * Portable Audio I/O Library
 * host api representation
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
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
 */

#include "portaudio.h"

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


typedef struct PaUtilPrivatePaFrontHostApiInfo {
/* **for the use of pa_front.c only**
    don't use fields in this structure, they my change at any time
    use functions defined in pa_util.h if you think you need functionality
    which can be derived from here
*/

    unsigned long baseDeviceIndex;
}PaUtilPrivatePaFrontHostApiInfo;


/*
 PaUtilHostApiRepresentation must be implemented by each host api implementation.

*/

typedef struct PaUtilHostApiRepresentation {
    PaUtilPrivatePaFrontHostApiInfo privatePaFrontInfo;
    PaHostApiInfo info;
        
    int deviceCount;
    PaDeviceInfo** deviceInfos;
    int defaultInputDeviceIndex;
    int defaultOutputDeviceIndex;

    /*
        (*Terminate)() is guaranteed to be called with a valid <hostApi>
        parameter, which was previously returned from the same implementation's
        initializer.
    */
    void (*Terminate)( struct PaUtilHostApiRepresentation *hostApi );

    /*
        The following guarantees are made about parameters to (*OpenStream)():

            PaHostApiRepresentation *hostApi
                - is valid for this implementation

            PaStream** stream
                - is non-null

            - at least one of inputDevice & outputDevice is valid (not paNoDevice)

            - if inputDevice & outputDevice are both valid, they both use
                the same host api

            PaDeviceIndex inputDevice
                - is within range (0 to Pa_CountDevices-1)

            int numInputChannels
                - if inputDevice is valid, numInputChannels is > 0
                - upper bound is NOT validated against device capabilities
                - will be zero if inputDevice is paNoDevice

            PaSampleFormat inputSampleFormat
                - is one of the sample formats defined in portaudio.h

            void *inputStreamInfo
                - if supplied its hostApi field matches the input device's host Api
                - will be NULL if input device is paNoDevice
                
            PaDeviceIndex outputDevice
                - is within range (0 to Pa_CountDevices-1)

            int numOutputChannels
                - if inputDevice is valid, numInputChannels is > 0
                - upper bound is NOT validated against device capabilities
                - will be zero if outputDevice is paNoDevice

            PaSampleFormat outputSampleFormat
                - is one of the sample formats defined in portaudio.h
        
            void *outputStreamInfo
                - if supplied its hostApi field matches the output device's host Api
                - will be NULL if output device is paNoDevice
                
            double sampleRate
                - is not an 'absurd' rate (less than 1000. or greater than 200000.)
                - sampleRate is NOT validated against device capabilities

            PaStreamFlags streamFlags
                - unused platform neutral flags are zero


        The following validations MUST be performed by (*OpenStream)():

            - check that input device can support numInputChannels
            
            - check that input device can support inputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - if inputStreamInfo is supplied, validate its contents,
                or return an error if no inputStreamInfo is expected

            - check that output device can support numOutputChannels
            
            - check that output device can support outputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - if outputStreamInfo is supplied, validate its contents,
                or return an error if no outputStreamInfo is expected

            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported

            - check that the device supports sampleRate

            - alter sampleRate to a close allowable rate if necessary

            - validate inputLatency and outputLatency

            - validate any platform specific flags, if flags are supplied they
                must be valid.
    */
    PaError (*OpenStream)( struct PaUtilHostApiRepresentation *hostApi,
                       PaStream** stream,
                       PaDeviceIndex inputDevice,
                       int numInputChannels,
                       PaSampleFormat inputSampleFormat,
                       unsigned long inputLatency,
                       PaHostApiSpecificStreamInfo *inputStreamInfo,
                       PaDeviceIndex outputDevice,
                       int numOutputChannels,
                       PaSampleFormat outputSampleFormat,
                       unsigned long outputLatency,
                       PaHostApiSpecificStreamInfo *outputStreamInfo,
                       double sampleRate,
                       unsigned long framesPerCallback,
                       PaStreamFlags streamFlags,
                       PortAudioCallback *callback,
                       void *userData );
} PaUtilHostApiRepresentation;


/*
    every host api implementation must supply a host api initializer in the
    following form.
*/
typedef PaError PaUtilHostApiInitializer( PaUtilHostApiRepresentation**, PaHostApiIndex );


/*
 paHostApiInitializers is a NULL-terminated array of host api initializers
 for the host apis which will be initialized when Pa_Initialize() is called.
 each platform has a file which defines hostApiInitializers for that platform.
 see pa_win_init.c for example.
*/
extern PaUtilHostApiInitializer *paHostApiInitializers[];


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_HOSTAPI_H */
