/*
 * $Id$
 * Portable Audio I/O Library skeleton implementation
 * demonstrates how to use the common functions to implement support
 * for a host API
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

#include <string.h> /* strlen() */

#include "pa_util.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

/* prototypes for functions declared in this file */

PaError PaSkeleton_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** stream,
                           PaDeviceIndex inputDevice,
                           int numInputChannels,
                           PaSampleFormat inputSampleFormat,
                           void *inputDriverInfo,
                           PaDeviceIndex outputDevice,
                           int numOutputChannels,
                           PaSampleFormat outputSampleFormat,
                           void *outputDriverInfo,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           unsigned long numberOfBuffers,
                           PaStreamFlags streamFlags,
                           PortAudioCallback *callback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamActive( PaStream *stream );
static PaTimestamp GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, void *buffer, unsigned long frames );
static unsigned long GetStreamReadAvailable( PaStream* stream );
static unsigned long GetStreamWriteAvailable( PaStream* stream );


/* PaSkeletonHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation commonHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    /* implementation specific data goes here */
}
PaSkeletonHostApiRepresentation;  /* IMPLEMENT ME: rename this */


PaError PaSkeleton_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaSkeletonHostApiRepresentation *skeletonHostApi = 0;
    PaDeviceInfo *deviceInfoArray = 0;

    skeletonHostApi = (PaSkeletonHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaSkeletonHostApiRepresentation) );
    if( !skeletonHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &skeletonHostApi->commonHostApiRep;
    (*hostApi)->deviceInfos = 0;    /* initialize to allow allocation test in error: */

    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;
    (*hostApi)->info.name = "skeleton implementation";

    deviceCount = 0;

    (*hostApi)->deviceCount = deviceCount;
    (*hostApi)->defaultInputDeviceIndex = paNoDevice;  /* IMPLEMENT ME */
    (*hostApi)->defaultOutputDeviceIndex = paNoDevice; /* IMPLEMENT ME */

    if( deviceCount > 0 )
    {

        (*hostApi)->deviceInfos =
            (PaDeviceInfo**)PaUtil_AllocateMemory( sizeof(PaDeviceInfo*) * deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block
            (*hostApi)->deviceInfos[0] points to start of block
         */
        (*hostApi)->deviceInfos[0] = 0;
        deviceInfoArray = (PaDeviceInfo*)PaUtil_AllocateMemory( sizeof(PaDeviceInfo) * deviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* initializes buffers ptrs to zero so they can be deallocated on error */
        for( i=0; i < deviceCount; ++i )
        {
            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            (*hostApi)->deviceInfos[i] = deviceInfo;

            deviceInfo->name = 0;
            /* IMPLEMENT ME: initialize other buffer ptrs to zero */
        }

        for( i=0; i < deviceCount; ++i )
        {
            PaDeviceInfo *deviceInfo = (*hostApi)->deviceInfos[i];

            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = index;

            deviceInfo->name = 0; /* IMPLEMENT ME: allocate block and copy name eg:
                            deviceName = PaUtil_AllocateMemory( strlen(srcName) + 1 );
                            strcpy( deviceName, srcName );
                            deviceInfo->name = deviceName;
                        */

            /*
                IMPLEMENT ME:
                    - populate other device info fields
            */
        }
    }
    else
    {
        (*hostApi)->deviceInfos = 0;
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;

    PaUtil_InitializeStreamInterface( &skeletonHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamActive, GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite, PaUtil_DummyGetAvailable, PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &skeletonHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( skeletonHostApi )
    {
        if( deviceCount > 0 )
        {
            if( skeletonHostApi->commonHostApiRep.deviceInfos )
            {
                if( skeletonHostApi->commonHostApiRep.deviceInfos[0] )
                {
                    for( i=0; i < deviceCount; ++i )
                    {
                        if( skeletonHostApi->commonHostApiRep.deviceInfos[0]->name )
                            PaUtil_FreeMemory( (char*)skeletonHostApi->commonHostApiRep.deviceInfos[0]->name );
                        /* IMPLEMENT ME: free other device info buffers if allocated */
                    }

                    PaUtil_FreeMemory( skeletonHostApi->commonHostApiRep.deviceInfos[0] );
                }

                PaUtil_FreeMemory( skeletonHostApi->commonHostApiRep.deviceInfos );
            }
        }
        PaUtil_FreeMemory( skeletonHostApi );
    }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    int i;
    PaSkeletonHostApiRepresentation *skeletonHostApi = (PaSkeletonHostApiRepresentation*)hostApi;

    if( hostApi->deviceCount > 0 )
    {
        /*
            IMPLEMENT ME:
                - free device info strings and arrays if they have been allocated
        */

        PaUtil_FreeMemory( hostApi->deviceInfos[0] );
        PaUtil_FreeMemory( hostApi->deviceInfos );
    }

    PaUtil_FreeMemory( skeletonHostApi );
}


/* PaSkeletonStream - a stream data structure specifically for this implementation */

typedef struct PaSkeletonStream
{ /* IMPLEMENT ME: rename this */
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    /* IMPLEMENT ME:
            - implementation specific data goes here
    */
    unsigned long framesPerHostCallback; /* just an example */
}
PaSkeletonStream;


static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           PaDeviceIndex inputDevice,
                           int numInputChannels,
                           PaSampleFormat inputSampleFormat,
                           void *inputStreamInfo,
                           PaDeviceIndex outputDevice,
                           int numOutputChannels,
                           PaSampleFormat outputSampleFormat,
                           void *outputStreamInfo,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           unsigned long numberOfBuffers,
                           PaStreamFlags streamFlags,
                           PortAudioCallback *callback,
                           void *userData )
{
    PaError result = paNoError;
    PaSkeletonHostApiRepresentation *skeletonHostApi = (PaSkeletonHostApiRepresentation*)hostApi;
    PaSkeletonStream *stream = 0;
    unsigned long framesPerHostBuffer = framesPerBuffer; /* these may not be equivalent for all implementations */
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;

    /*
        IMPLEMENT ME:
            - check that input device can support numInputChannels

            - check that output device can support numOutputChannels

        ( the following two checks are taken care of by PaUtil_InitializeBufferProcessor() )

            - check that input device can support inputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - check that output device can support outputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported

            - check that the device supports sampleRate

            - alter sampleRate to a close allowable rate if possible / necessary

            - validate framesPerBuffer and numberOfBuffers
    */


    /* validate inputStreamInfo */
    if( inputStreamInfo )
        return paIncompatibleStreamInfo; /* this implementation doesn't use custom stream info */

    /* validate outputStreamInfo */
    if( outputStreamInfo )
        return paIncompatibleStreamInfo; /* this implementation doesn't use custom stream info */

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */


    stream = (PaSkeletonStream*)PaUtil_AllocateMemory( sizeof(PaSkeletonStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    if( callback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &skeletonHostApi->callbackStreamInterface, callback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &skeletonHostApi->blockingStreamInterface, callback, userData );
    }


    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    
    /* IMPLEMENT ME - establish which  host formats are available */
    hostInputSampleFormat =
        PaUtil_SelectClosestAvailableFormat( paInt16 /* native formats */, inputSampleFormat );

    /* IMPLEMENT ME - establish which  host formats are available */
    hostOutputSampleFormat =
        PaUtil_SelectClosestAvailableFormat( paInt16 /* native formats */, outputSampleFormat );
        

    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              numInputChannels, inputSampleFormat, hostInputSampleFormat,
              numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerBuffer, framesPerHostBuffer,
              callback, userData );
    if( result != paNoError )
        goto error;

    /*
        IMPLEMENT ME:
            - additional stream setup + opening
    */

    stream->framesPerHostCallback = framesPerHostBuffer;

    *s = (PaStream*)stream;

    return result;

error:
    if( stream )
        PaUtil_FreeMemory( stream );

    return result;
}

/*
    ExampleHostProcessingLoop() illustrates the kind of processing which may
    occur in a host implementation.
 
*/
static void ExampleHostProcessingLoop( void *inputBuffer, void *outputBuffer, void *userData )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)userData;
    PaTimestamp outTime = 0; /* IMPLEMENT ME */
    int callbackResult;

    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer, stream->framesPerHostCallback );
    
    /*
        IMPLEMENT ME:
            - generate timing information
            - handle buffer slips
    */

    /*
        If you need to byte swap inputBuffer, you can do it here using
        routines in pa_byteswappers.h
    */

    /*
        depending on whether the host buffers are interleaved, non-interleaved
        or a mixture, you will want to call PaUtil_ProcessInterleavedBuffers(),
        PaUtil_ProcessNonInterleavedBuffers() or PaUtil_ProcessBuffers() here.
    */
    callbackResult = PaUtil_ProcessInterleavedBuffers( &stream->bufferProcessor, inputBuffer, outputBuffer, outTime );

    /*
        If you need to byte swap outputBuffer, you can do it here using
        routines in pa_byteswappers.h
    */

    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer );
    
    if( callbackResult != 0 )
    {
        /* IMPLEMENT ME - stop the stream */
    }
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /*
        IMPLEMENT ME:
            - additional stream closing + cleanup
    */

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

    return result;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return result;
}


static PaError IsStreamActive( PaStream *s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return 0;
}


static PaTimestamp GetStreamTime( PaStream *s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}


/*
    As separate stream interfaces are used for blocking and callback
    streams, the following functions can be guaranteed to only be called
    for blocking streams.
*/

static PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            void *buffer,
                            unsigned long frames )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static unsigned long GetStreamReadAvailable( PaStream* s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static unsigned long GetStreamWriteAvailable( PaStream* s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}



