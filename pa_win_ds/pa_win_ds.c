/*
 * $Id$
 * Portable Audio I/O Library DirectSound implementation
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

#include "dsound_wrapper.h"

#define PRINT(x) { printf x; fflush(stdout); }
#define ERR_RPT(x) PRINT(x)
#define DBUG(x)  /* PRINT(x) */
#define DBUGX(x) /* PRINT(x) */

/* prototypes for functions declared in this file */

PaError PaWinDs_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex );
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           PaDeviceIndex inputDevice,
                           int numInputChannels,
                           PaSampleFormat inputSampleFormat,
                           unsigned long inputLatency,
                           void *inputStreamInfo,
                           PaDeviceIndex outputDevice,
                           int numOutputChannels,
                           PaSampleFormat outputSampleFormat,
                           unsigned long outputLatency,
                           void *outputStreamInfo,
                           double sampleRate,
                           unsigned long framesPerCallback,
                           PaStreamFlags streamFlags,
                           PortAudioCallback *callback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTimestamp GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, void *buffer, unsigned long frames );
static unsigned long GetStreamReadAvailable( PaStream* stream );
static unsigned long GetStreamWriteAvailable( PaStream* stream );

/************************************************* DX Prototypes **********/
static BOOL CALLBACK Pa_EnumOutputProc(LPGUID lpGUID,
                                 LPCTSTR lpszDesc,
                                 LPCTSTR lpszDrvName,
                                 LPVOID lpContext );
static BOOL CALLBACK Pa_CountDevProc(LPGUID lpGUID,
                                     LPCTSTR lpszDesc,
                                     LPCTSTR lpszDrvName,
                                     LPVOID lpContext );

/* PaWinDsHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct PaWinDsDeviceInfo
{
    GUID                             GUID;
    GUID                            *lpGUID;
    double                           sampleRates[3];
} PaWinDsDeviceInfo;

typedef struct
{
    PaUtilHostApiRepresentation commonHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationContext *allocations;

    /* implementation specific data goes here */
    PaWinDsDeviceInfo       *winDsDeviceInfos;
    PaError                  enumerationError;

}
PaWinDsHostApiRepresentation;


/************************************************************************************
** Just count devices so we know how much memory to allocate.
*/
static BOOL CALLBACK Pa_CountDevProc(LPGUID lpGUID,
                                     LPCTSTR lpszDesc,
                                     LPCTSTR lpszDrvName,
                                     LPVOID lpContext )
{
    int *counterPtr = (int *)lpContext;
    *counterPtr += 1;
    return TRUE;
}

/************************************************************************************
** Extract capabilities info from each device.
*/
static BOOL CALLBACK Pa_EnumOutputProc(LPGUID lpGUID,
                                 LPCTSTR lpszDesc,
                                 LPCTSTR lpszDrvName,
                                 LPVOID lpContext )
{
    HRESULT                       hr;
    DSCAPS                        caps;
    LPDIRECTSOUND                 lpDirectSound;
    PaWinDsHostApiRepresentation *winDsHostApi  = (PaWinDsHostApiRepresentation *) lpContext;
    PaUtilHostApiRepresentation  *hostApi = &winDsHostApi->commonHostApiRep;
    int                           index = hostApi->deviceCount;
    PaDeviceInfo                 *deviceInfo = hostApi->deviceInfos[index];
    PaWinDsDeviceInfo            *winDsDeviceInfo = &winDsHostApi->winDsDeviceInfos[index];


    /* Copy GUID to static array. Set pointer. */
    if( lpGUID == NULL )
    {
        winDsDeviceInfo->lpGUID = NULL;
    }
    else
    {
        winDsDeviceInfo->lpGUID = &winDsDeviceInfo->GUID;
        memcpy( &winDsDeviceInfo->GUID, lpGUID, sizeof(GUID) );
    }

    /* Allocate room for descriptive name. */
    if( lpszDesc != NULL )
    {
        char *deviceName;
        int len = strlen(lpszDesc);
        deviceName = (char*)PaUtil_ContextAllocateMemory( winDsHostApi->allocations, len + 1 );
        if( !deviceName )
        {
            winDsHostApi->enumerationError = paInsufficientMemory;
            return FALSE;
        }
        memcpy( (void *) deviceName, lpszDesc, len+1 );
        deviceInfo->name = deviceName;
    }

   /********** Output ******************************/
    if( lpGUID == NULL ) hostApi->defaultOutputDeviceIndex = index;

    /* Create interfaces for each object. */
    hr = DirectSoundCreate(  lpGUID, &lpDirectSound,   NULL );
    if( hr != DS_OK )
    {
        deviceInfo->maxOutputChannels = 0;
        DBUG(("Cannot create dsound for %s. Result = 0x%x\n", lpszDesc, hr ));
    }
    else
    {
        /* Query device characteristics. */
        caps.dwSize = sizeof(caps);
        IDirectSound_GetCaps( lpDirectSound, &caps );
        deviceInfo->maxOutputChannels = ( caps.dwFlags & DSCAPS_PRIMARYSTEREO ) ? 2 : 1;
        /* Get sample rates. */
        winDsDeviceInfo->sampleRates[0] = (double) caps.dwMinSecondarySampleRate;
        winDsDeviceInfo->sampleRates[1] = (double) caps.dwMaxSecondarySampleRate;
        if( caps.dwFlags & DSCAPS_CONTINUOUSRATE ) deviceInfo->numSampleRates = -1;
        else if( caps.dwMinSecondarySampleRate == caps.dwMaxSecondarySampleRate )
        {
            if( caps.dwMinSecondarySampleRate == 0 )
            {
                /*
                ** On my Thinkpad 380Z, DirectSoundV6 returns min-max=0 !!
                ** But it supports continuous sampling.
                ** So fake range of rates, and hope it really supports it.
                */
                winDsDeviceInfo->sampleRates[0] = 11025.0f;
                winDsDeviceInfo->sampleRates[1] = 48000.0f;
                deviceInfo->numSampleRates = -1; /* continuous range */

                DBUG(("PA - Reported rates both zero. Setting to fake values for device #%d\n", sDeviceIndex ));
            }
            else
            {
                deviceInfo->numSampleRates = 1;
            }
        }
        else if( (caps.dwMinSecondarySampleRate < 1000.0) && (caps.dwMaxSecondarySampleRate > 50000.0) )
        {
            /* The EWS88MT drivers lie, lie, lie. The say they only support two rates, 100 & 100000.
            ** But we know that they really support a range of rates!
            ** So when we see a ridiculous set of rates, assume it is a range.
            */
            deviceInfo->numSampleRates = -1;
            DBUG(("PA - Sample rate range used instead of two odd values for device #%d\n", sDeviceIndex ));
        }
        else deviceInfo->numSampleRates = 2;
        IDirectSound_Release( lpDirectSound );
    }
    deviceInfo->sampleRates = winDsDeviceInfo->sampleRates;
    deviceInfo->nativeSampleFormats = paInt16;
    hostApi->deviceCount++;
    return( TRUE );
}


/************************************************************************************
** Extract capabilities info from each device.
*/
static BOOL CALLBACK Pa_EnumInputProc(LPGUID lpGUID,
                                 LPCTSTR lpszDesc,
                                 LPCTSTR lpszDrvName,
                                 LPVOID lpContext )
{
    HRESULT                       hr;
    DSCCAPS                        caps;
    LPDIRECTSOUNDCAPTURE          lpDirectSoundCapture;
    PaWinDsHostApiRepresentation *winDsHostApi  = (PaWinDsHostApiRepresentation *) lpContext;
    PaUtilHostApiRepresentation  *hostApi = &winDsHostApi->commonHostApiRep;
    int                           index = hostApi->deviceCount;
    PaDeviceInfo                 *deviceInfo = hostApi->deviceInfos[index];
    PaWinDsDeviceInfo            *winDsDeviceInfo = &winDsHostApi->winDsDeviceInfos[index];


    /* Copy GUID to static array. Set pointer. */
    if( lpGUID == NULL )
    {
        winDsDeviceInfo->lpGUID = NULL;
    }
    else
    {
        winDsDeviceInfo->lpGUID = &winDsDeviceInfo->GUID;
        memcpy( &winDsDeviceInfo->GUID, lpGUID, sizeof(GUID) );
    }

    /* Allocate room for descriptive name. */
    if( lpszDesc != NULL )
    {
        char *deviceName;
        int len = strlen(lpszDesc);
        deviceName = (char*)PaUtil_ContextAllocateMemory( winDsHostApi->allocations, len + 1 );
        if( !deviceName )
        {
            winDsHostApi->enumerationError = paInsufficientMemory;
            return FALSE;
        }
        memcpy( (void *) deviceName, lpszDesc, len+1 );
        deviceInfo->name = deviceName;
    }

    /********** Input ******************************/
    if( lpGUID == NULL ) hostApi->defaultInputDeviceIndex = index;

    hr = DirectSoundCaptureCreate(  lpGUID, &lpDirectSoundCapture,   NULL );
    if( hr != DS_OK )
    {
        deviceInfo->maxInputChannels = 0;
        DBUG(("Cannot create Capture for %s. Result = 0x%x\n", lpszDesc, hr ));
    }
    else
    {
        /* Query device characteristics. */
        caps.dwSize = sizeof(caps);
        IDirectSoundCapture_GetCaps( lpDirectSoundCapture, &caps );
        /* printf("caps.dwFormats = 0x%x\n", caps.dwFormats ); */
        deviceInfo->maxInputChannels = caps.dwChannels;
        /* Determine sample rates from flags. */
        if( caps.dwChannels == 2 )
        {
            int index = 0;
            if( caps.dwFormats & WAVE_FORMAT_1S16) winDsDeviceInfo->sampleRates[index++] = 11025.0;
            if( caps.dwFormats & WAVE_FORMAT_2S16) winDsDeviceInfo->sampleRates[index++] = 22050.0;
            if( caps.dwFormats & WAVE_FORMAT_4S16) winDsDeviceInfo->sampleRates[index++] = 44100.0;
            deviceInfo->numSampleRates = index;
        }
        else if( caps.dwChannels == 1 )
        {
            int index = 0;
            if( caps.dwFormats & WAVE_FORMAT_1M16) winDsDeviceInfo->sampleRates[index++] = 11025.0;
            if( caps.dwFormats & WAVE_FORMAT_2M16) winDsDeviceInfo->sampleRates[index++] = 22050.0;
            if( caps.dwFormats & WAVE_FORMAT_4M16) winDsDeviceInfo->sampleRates[index++] = 44100.0;
            deviceInfo->numSampleRates = index;
        }
        else deviceInfo->numSampleRates = 0;
        IDirectSoundCapture_Release( lpDirectSoundCapture );
        }    deviceInfo->sampleRates = winDsDeviceInfo->sampleRates;
    deviceInfo->nativeSampleFormats = paInt16;
    hostApi->deviceCount++;
    return( TRUE );
}


/***********************************************************************************/
PaError PaWinDs_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaWinDsHostApiRepresentation *winDsHostApi;
    PaDeviceInfo *deviceInfoArray;

    winDsHostApi = (PaWinDsHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaWinDsHostApiRepresentation) );
    if( !winDsHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    winDsHostApi->allocations = PaUtil_CreateAllocationContext();
    if( !winDsHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &winDsHostApi->commonHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;
    (*hostApi)->info.name = "Windows DirectSound";
    
    (*hostApi)->deviceCount = 0;  
    (*hostApi)->defaultInputDeviceIndex = paNoDevice;
    (*hostApi)->defaultOutputDeviceIndex = paNoDevice;

    deviceCount = 0;
/* DSound - enumerate devices to count them. */
    DirectSoundEnumerate( (LPDSENUMCALLBACK)Pa_CountDevProc, &deviceCount );
    DirectSoundCaptureEnumerate( (LPDSENUMCALLBACK)Pa_CountDevProc, &deviceCount );
    
    if( deviceCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_ContextAllocateMemory(
                winDsHostApi->allocations, sizeof(PaDeviceInfo*) * deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_ContextAllocateMemory(
                winDsHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all DSound specific info structs in a contiguous block */
        winDsHostApi->winDsDeviceInfos = (PaWinDsDeviceInfo*)PaUtil_ContextAllocateMemory(
                winDsHostApi->allocations, sizeof(PaWinDsDeviceInfo) * deviceCount );
        if( !winDsHostApi->winDsDeviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i=0; i < deviceCount; ++i )
        {
            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = hostApiIndex;
            deviceInfo->name = 0; 
            (*hostApi)->deviceInfos[i] = deviceInfo;
        }

    /* DSound - Enumerate again to fill in structures. */
        winDsHostApi->enumerationError = 0;
        DirectSoundEnumerate( (LPDSENUMCALLBACK)Pa_EnumOutputProc, (void *)winDsHostApi );
        if( winDsHostApi->enumerationError != paNoError ) return winDsHostApi->enumerationError;

        winDsHostApi->enumerationError = 0;
        DirectSoundCaptureEnumerate( (LPDSENUMCALLBACK)Pa_EnumInputProc, (void *)winDsHostApi );
        if( winDsHostApi->enumerationError != paNoError ) return winDsHostApi->enumerationError;
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;

    PaUtil_InitializeStreamInterface( &winDsHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive, GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite, PaUtil_DummyGetAvailable, PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &winDsHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( winDsHostApi )
    {
        if( winDsHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( winDsHostApi->allocations );
            PaUtil_DestroyAllocationContext( winDsHostApi->allocations );
        }
                
        PaUtil_FreeMemory( winDsHostApi );
    }
    return result;
}


/***********************************************************************************/
static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaWinDsHostApiRepresentation *winDsHostApi = (PaWinDsHostApiRepresentation*)hostApi;

    /*
        IMPLEMENT ME:
            - clean up any resourced not handled by the allocation context
    */

    if( winDsHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( winDsHostApi->allocations );
        PaUtil_DestroyAllocationContext( winDsHostApi->allocations );
    }

    PaUtil_FreeMemory( winDsHostApi );
}


/* PaWinDsStream - a stream data structure specifically for this implementation */

typedef struct PaWinDsStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    /* IMPLEMENT ME:
            - implementation specific data goes here
    */
    unsigned long framesPerHostCallback; /* just an example */
}
PaWinDsStream;


/***********************************************************************************/
/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           PaDeviceIndex inputDevice,
                           int numInputChannels,
                           PaSampleFormat inputSampleFormat,
                           unsigned long inputLatency,
                           void *inputStreamInfo,
                           PaDeviceIndex outputDevice,
                           int numOutputChannels,
                           PaSampleFormat outputSampleFormat,
                           unsigned long outputLatency,
                           void *outputStreamInfo,
                           double sampleRate,
                           unsigned long framesPerCallback,
                           PaStreamFlags streamFlags,
                           PortAudioCallback *callback,
                           void *userData )
{
    PaError result = paNoError;
    PaWinDsHostApiRepresentation *winDsHostApi = (PaWinDsHostApiRepresentation*)hostApi;
    PaWinDsStream *stream = 0;
    unsigned long framesPerHostBuffer = framesPerCallback; /* these may not be equivalent for all implementations */
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


    stream = (PaWinDsStream*)PaUtil_AllocateMemory( sizeof(PaWinDsStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    if( callback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &winDsHostApi->callbackStreamInterface, callback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &winDsHostApi->blockingStreamInterface, callback, userData );
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
              sampleRate, streamFlags, framesPerCallback, framesPerHostBuffer,
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


/***********************************************************************************
    ExampleHostProcessingLoop() illustrates the kind of processing which may
    occur in a host implementation.
 
*/
static void ExampleHostProcessingLoop( void *inputBuffer, void *outputBuffer, void *userData )
{
    PaWinDsStream *stream = (PaWinDsStream*)userData;
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



/***********************************************************************************
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /*
        IMPLEMENT ME:
            - additional stream closing + cleanup
    */

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

    return result;
}


/***********************************************************************************/
static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return result;
}


/***********************************************************************************/
static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return result;
}


/***********************************************************************************/
static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return result;
}


/***********************************************************************************/
static PaError IsStreamStopped( PaStream *s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return 0;
}


/***********************************************************************************/
static PaError IsStreamActive( PaStream *s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior */

    return 0;
}


/***********************************************************************************/
static PaTimestamp GetStreamTime( PaStream *s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


/***********************************************************************************/
static double GetStreamCpuLoad( PaStream* s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}



/***********************************************************************************
    As separate stream interfaces are used for blocking and callback
    streams, the following functions can be guaranteed to only be called
    for blocking streams.
*/

static PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


/***********************************************************************************/
static PaError WriteStream( PaStream* s,
                            void *buffer,
                            unsigned long frames )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


/***********************************************************************************/
static unsigned long GetStreamReadAvailable( PaStream* s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


/***********************************************************************************/
static unsigned long GetStreamWriteAvailable( PaStream* s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}



