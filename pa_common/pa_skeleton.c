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
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

/*
    NOTE TO IMPLEMENTORS:

    This file is provided as a starting point for implementing support for
    a new host API. IMPLEMENT ME comments are used to indicate functionality
    which much be customised for each implementation.
*/


/* prototypes for functions declared in this file */

PaError PaSkeleton_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex );
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
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
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PortAudioCallback *callback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamInputLatency( PaStream *stream );
static PaTime GetStreamOutputLatency( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, void *buffer, unsigned long frames );
static unsigned long GetStreamReadAvailable( PaStream* stream );
static unsigned long GetStreamWriteAvailable( PaStream* stream );


/* PaSkeletonHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    /* implementation specific data goes here */
}
PaSkeletonHostApiRepresentation;  /* IMPLEMENT ME: rename this */


PaError PaSkeleton_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaSkeletonHostApiRepresentation *skeletonHostApi;
    PaDeviceInfo *deviceInfoArray;

    skeletonHostApi = (PaSkeletonHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaSkeletonHostApiRepresentation) );
    if( !skeletonHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    skeletonHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !skeletonHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &skeletonHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;            /* IMPLEMENT ME: change to correct type id */
    (*hostApi)->info.name = "skeleton implementation";  /* IMPLEMENT ME: change to correct name */
    
    (*hostApi)->deviceCount = 0;  
    (*hostApi)->defaultInputDeviceIndex = paNoDevice;  /* IMPLEMENT ME */
    (*hostApi)->defaultOutputDeviceIndex = paNoDevice; /* IMPLEMENT ME */

    deviceCount = 0; /* IMPLEMENT ME */
    
    if( deviceCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
                skeletonHostApi->allocations, sizeof(PaDeviceInfo*) * deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
                skeletonHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i=0; i < deviceCount; ++i )
        {
            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = hostApiIndex;
            deviceInfo->name = 0; /* IMPLEMENT ME: allocate block and copy name eg:
                deviceName = (char*)PaUtil_GroupAllocateMemory( skeletonHostApi->allocations, strlen(srcName) + 1 );
                if( !deviceName )
                {
                    result = paInsufficientMemory;
                    goto error;
                }
                strcpy( deviceName, srcName );
                deviceInfo->name = deviceName;
            */

            /*
                IMPLEMENT ME:
                    - populate other device info fields
            */
            
            (*hostApi)->deviceInfos[i] = deviceInfo;
            ++(*hostApi)->deviceCount;
        }
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;

    PaUtil_InitializeStreamInterface( &skeletonHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamInputLatency, GetStreamOutputLatency, GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite, PaUtil_DummyGetAvailable, PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &skeletonHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamInputLatency, GetStreamOutputLatency, GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( skeletonHostApi )
    {
        if( skeletonHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( skeletonHostApi->allocations );
            PaUtil_DestroyAllocationGroup( skeletonHostApi->allocations );
        }
                
        PaUtil_FreeMemory( skeletonHostApi );
    }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaSkeletonHostApiRepresentation *skeletonHostApi = (PaSkeletonHostApiRepresentation*)hostApi;

    /*
        IMPLEMENT ME:
            - clean up any resourced not handled by the allocation group
    */

    if( skeletonHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( skeletonHostApi->allocations );
        PaUtil_DestroyAllocationGroup( skeletonHostApi->allocations );
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

/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
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
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PortAudioCallback *callback,
                           void *userData )
{
    PaError result = paNoError;
    PaSkeletonHostApiRepresentation *skeletonHostApi = (PaSkeletonHostApiRepresentation*)hostApi;
    PaSkeletonStream *stream = 0;
    unsigned long framesPerHostBuffer = framesPerBuffer; /* these may not be equivalent for all implementations */
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;

    /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

    if( (inputDevice == paUseHostApiSpecificDeviceSpecification)
            || (outputDevice == paUseHostApiSpecificDeviceSpecification) )
        return paInvalidDevice;                    

    /* check that input device can support numInputChannels */
    if( (inputDevice != paNoDevice) &&
            (numInputChannels > hostApi->deviceInfos[ inputDevice ]->maxInputChannels) )
        return paInvalidChannelCount;


    /* check that output device can support numInputChannels */
    if( (outputDevice != paNoDevice) &&
            (numOutputChannels > hostApi->deviceInfos[ outputDevice ]->maxOutputChannels) )
        return paInvalidChannelCount;

    /*
        IMPLEMENT ME:

        ( the following two checks are taken care of by PaUtil_InitializeBufferProcessor() FIXME - checks needed? )

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

            - validate inputLatency and outputLatency parameters,
                use default values where necessary
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


    /* we assume a fixed host buffer size in this example, but the buffer processor
        can also support bounded and unknown host buffer sized by passing 
        paUtilBoundedHostBufferSize or paUtilUnknownHostBufferSize instead of
        paUtilFixedHostBufferSize below. */
        
    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              numInputChannels, inputSampleFormat, hostInputSampleFormat,
              numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerBuffer,
              framesPerHostBuffer, paUtilFixedHostBufferSize,
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
    PaTime outTime = 0; /* IMPLEMENT ME */
    int callbackResult;
    unsigned long framesProcessed;
    
    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
    
    /*
        IMPLEMENT ME:
            - generate timing information
            - handle buffer slips
    */

    /*
        If you need to byte swap or shift inputBuffer to convert it into a
        portaudio format, do it here.
    */



    PaUtil_BeginBufferProcessing( &stream->bufferProcessor, outTime );

    /*
        depending on whether the host buffers are interleaved, non-interleaved
        or a mixture, you will want to call PaUtil_SetInterleaved*Channels(),
        PaUtil_SetNonInterleaved*Channel() or PaUtil_Set*Channel() here.
    */
    
    PaUtil_SetInputFrameCount( &stream->bufferProcessor, 0 /* default to host buffer size */ );
    PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor,
            0, /* first channel of inputBuffer is channel 0 */
            inputBuffer,
            0 ); /* 0 - use numInputChannels passed to init buffer processor */

    PaUtil_SetOutputFrameCount( &stream->bufferProcessor, 0 /* default to host buffer size */ );
    PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
            0, /* first channel of outputBuffer is channel 0 */
            outputBuffer,
            0 ); /* 0 - use numOutputChannels passed to init buffer processor */

    framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );

    
    /*
        If you need to byte swap or shift outputBuffer to convert it to
        host format, do it here.
    */

    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );


    if( callbackResult == paContinue )
    {
        /* nothing special to do */
    }
    else if( callbackResult == paAbort )
    {
        /* IMPLEMENT ME - finish playback immediately  */
    }
    else
    {
        /* User callback has asked us to stop with paComplete or other non-zero value */

        /* IMPLEMENT ME - finish playback once currently queued audio has completed  */
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


static PaError IsStreamStopped( PaStream *s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return 0;
}


static PaError IsStreamActive( PaStream *s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return 0;
}


static PaTime GetStreamInputLatency( PaStream *s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static PaTime GetStreamOutputLatency( PaStream *s )
{
    PaSkeletonStream *stream = (PaSkeletonStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static PaTime GetStreamTime( PaStream *s )
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



