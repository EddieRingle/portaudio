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

#include <stdio.h>
#include <string.h> /* strlen() */

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "dsound_wrapper.h"

/* TODO
O- fix "patest_stop.c"
O- Handle buffer underflow, overflow, etc.
*/

#define PRINT(x) { printf x; fflush(stdout); }
#define ERR_RPT(x) PRINT(x)
#define DBUG(x)  /* PRINT(x) */
#define DBUGX(x) /* PRINT(x) */

#define PA_USE_HIGH_LATENCY   (0)
#if PA_USE_HIGH_LATENCY
#define PA_WIN_9X_LATENCY     (500)
#define PA_WIN_NT_LATENCY     (600)
#else
#define PA_WIN_9X_LATENCY     (140)
#define PA_WIN_NT_LATENCY     (280)
#endif

#define PA_WIN_WDM_LATENCY       (120)

#define SECONDS_PER_MSEC      (0.001)
#define MSEC_PER_SECOND       (1000)

/* prototypes for functions declared in this file */

PaError PaWinDs_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex );
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

/************************************************* DX Prototypes **********/
static BOOL CALLBACK Pa_EnumOutputProc(LPGUID lpGUID,
                                 LPCTSTR lpszDesc,
                                 LPCTSTR lpszDrvName,
                                 LPVOID lpContext );
static BOOL CALLBACK Pa_CountDevProc(LPGUID lpGUID,
                                     LPCTSTR lpszDesc,
                                     LPCTSTR lpszDrvName,
                                     LPVOID lpContext );

/************************************************************************************/
/********************** Structures **************************************************/
/************************************************************************************/
/* PaWinDsHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct PaWinDsDeviceInfo
{
    GUID                             GUID;
    GUID                            *lpGUID;
    double                           sampleRates[3];
} PaWinDsDeviceInfo;

typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface    callbackStreamInterface;
    PaUtilStreamInterface    blockingStreamInterface;

    PaUtilAllocationGroup   *allocations;

    /* implementation specific data goes here */
    PaWinDsDeviceInfo       *winDsDeviceInfos;
    PaError                  enumerationError;

} PaWinDsHostApiRepresentation;

/* PaWinDsStream - a stream data structure specifically for this implementation */

typedef struct PaWinDsStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

/* DirectSound specific data. */
    DSoundWrapper    directSoundWrapper;
    MMRESULT         timerID;
    BOOL             ifInsideCallback;  /* Test for reentrancy. */
    int              framesPerDSBuffer;
    double           framesWritten;
    double           secondsPerHostByte; /* Used to optimize latency calculation for outTime */

    double           outputLatency;

/* FIXME - move all below to PaUtilStreamRepresentation */
    double           sampleRate; 
    volatile int     isStarted;
    volatile int     isActive;
    volatile int     stopProcessing; /* stop thread once existing buffers have been returned */
    volatile int     abortProcessing; /* stop thread immediately */
} PaWinDsStream;


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
    PaUtilHostApiRepresentation  *hostApi = &winDsHostApi->inheritedHostApiRep;
    int                           index = hostApi->deviceCount;
    PaDeviceInfo                 *deviceInfo = hostApi->deviceInfos[index];
    PaWinDsDeviceInfo            *winDsDeviceInfo = &winDsHostApi->winDsDeviceInfos[index];
    int                           deviceOK = TRUE;

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


   /********** Output ******************************/

    /* Create interfaces for each object. */
    hr = dswDSoundEntryPoints.DirectSoundCreate(  lpGUID, &lpDirectSound,   NULL );
    if( hr != DS_OK )
    {
        deviceInfo->maxOutputChannels = 0;
        DBUG(("Cannot create dsound for %s. Result = 0x%x\n", lpszDesc, hr ));
        deviceOK = FALSE;
    }
    else
    {
        /* Query device characteristics. */
        caps.dwSize = sizeof(caps);
        IDirectSound_GetCaps( lpDirectSound, &caps );

#ifndef PA_NO_WMME
        if( caps.dwFlags & DSCAPS_EMULDRIVER )
        {
            /* If WMME supported, then reject Emulated drivers because they are lousy. */
            deviceOK = FALSE;
        }
#endif

        if( deviceOK )
        {
            /* Mono or stereo device? */
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
        }

        IDirectSound_Release( lpDirectSound );
    }

    if( deviceOK )
    {
        deviceInfo->sampleRates = winDsDeviceInfo->sampleRates;
        deviceInfo->nativeSampleFormats = paInt16;

        if( lpGUID == NULL ) hostApi->defaultOutputDeviceIndex = index;

        /* Allocate room for descriptive name. */
        if( lpszDesc != NULL )
        {
            char *deviceName;
            int len = strlen(lpszDesc);
            deviceName = (char*)PaUtil_GroupAllocateMemory( winDsHostApi->allocations, len + 1 );
            if( !deviceName )
            {
                winDsHostApi->enumerationError = paInsufficientMemory;
                return FALSE;
            }
            memcpy( (void *) deviceName, lpszDesc, len+1 );
            deviceInfo->name = deviceName;
        }

        hostApi->deviceCount++;
    }

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
    PaUtilHostApiRepresentation  *hostApi = &winDsHostApi->inheritedHostApiRep;
    int                           index = hostApi->deviceCount;
    PaDeviceInfo                 *deviceInfo = hostApi->deviceInfos[index];
    PaWinDsDeviceInfo            *winDsDeviceInfo = &winDsHostApi->winDsDeviceInfos[index];
    int                           deviceOK = TRUE;

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

    /********** Input ******************************/

    hr = dswDSoundEntryPoints.DirectSoundCaptureCreate(  lpGUID, &lpDirectSoundCapture,   NULL );
    if( hr != DS_OK )
    {
        deviceInfo->maxInputChannels = 0;
        DBUG(("Cannot create Capture for %s. Result = 0x%x\n", lpszDesc, hr ));
        deviceOK = FALSE;
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
        else
        {
            deviceInfo->numSampleRates = 0;
            deviceOK = FALSE;
        }
        IDirectSoundCapture_Release( lpDirectSoundCapture );
    }

    if( deviceOK )
    {
        deviceInfo->sampleRates = winDsDeviceInfo->sampleRates;
        deviceInfo->nativeSampleFormats = paInt16;

        /* Allocate room for descriptive name. */
        if( lpszDesc != NULL )
        {
            char *deviceName;
            int len = strlen(lpszDesc);
            deviceName = (char*)PaUtil_GroupAllocateMemory( winDsHostApi->allocations, len + 1 );
            if( !deviceName )
            {
                winDsHostApi->enumerationError = paInsufficientMemory;
                return FALSE;
            }
            memcpy( (void *) deviceName, lpszDesc, len+1 );
            deviceInfo->name = deviceName;
        }

        if( lpGUID == NULL ) hostApi->defaultInputDeviceIndex = index;

        hostApi->deviceCount++;
    }

    return TRUE;
}


/***********************************************************************************/
PaError PaWinDs_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaWinDsHostApiRepresentation *winDsHostApi;
    PaDeviceInfo *deviceInfoArray;

    DSW_InitializeDSoundEntryPoints();

    winDsHostApi = (PaWinDsHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaWinDsHostApiRepresentation) );
    if( !winDsHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    winDsHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !winDsHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &winDsHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paDirectSound;
    (*hostApi)->info.name = "Windows DirectSound";
    
    (*hostApi)->deviceCount = 0;  
    (*hostApi)->defaultInputDeviceIndex = paNoDevice;
    (*hostApi)->defaultOutputDeviceIndex = paNoDevice;

    deviceCount = 0;
/* DSound - enumerate devices to count them. */
    dswDSoundEntryPoints.DirectSoundEnumerate( (LPDSENUMCALLBACK)Pa_CountDevProc, &deviceCount );
    dswDSoundEntryPoints.DirectSoundCaptureEnumerate( (LPDSENUMCALLBACK)Pa_CountDevProc, &deviceCount );

    if( deviceCount > 0 )
    {
        /* allocate array for pointers to PaDeviceInfo structs */
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
                winDsHostApi->allocations, sizeof(PaDeviceInfo*) * deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all PaDeviceInfo structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
                winDsHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all DSound specific info structs in a contiguous block */
        winDsHostApi->winDsDeviceInfos = (PaWinDsDeviceInfo*)PaUtil_GroupAllocateMemory(
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
        dswDSoundEntryPoints.DirectSoundEnumerate( (LPDSENUMCALLBACK)Pa_EnumOutputProc, (void *)winDsHostApi );
        if( winDsHostApi->enumerationError != paNoError ) return winDsHostApi->enumerationError;

        winDsHostApi->enumerationError = 0;
        dswDSoundEntryPoints.DirectSoundCaptureEnumerate( (LPDSENUMCALLBACK)Pa_EnumInputProc, (void *)winDsHostApi );
        if( winDsHostApi->enumerationError != paNoError ) return winDsHostApi->enumerationError;
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;

    PaUtil_InitializeStreamInterface( &winDsHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamInputLatency, GetStreamOutputLatency, GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite, PaUtil_DummyGetAvailable, PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &winDsHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamInputLatency, GetStreamOutputLatency, GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( winDsHostApi )
    {
        if( winDsHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( winDsHostApi->allocations );
            PaUtil_DestroyAllocationGroup( winDsHostApi->allocations );
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
            - clean up any resourced not handled by the allocation group
    */

    if( winDsHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( winDsHostApi->allocations );
        PaUtil_DestroyAllocationGroup( winDsHostApi->allocations );
    }

    PaUtil_FreeMemory( winDsHostApi );

    DSW_TerminateDSoundEntryPoints();
}


/* Set minimal latency based on whether NT or Win95.
 * NT has higher latency.
 */
static int PaWinDS_GetMinSystemLatency( void )
{
    int minLatencyMsec;
    /* Set minimal latency based on whether NT or other OS.
     * NT has higher latency.
     */
    OSVERSIONINFO osvi;
	osvi.dwOSVersionInfoSize = sizeof( osvi );
	GetVersionEx( &osvi );
    DBUG(("PA - PlatformId = 0x%x\n", osvi.dwPlatformId ));
    DBUG(("PA - MajorVersion = 0x%x\n", osvi.dwMajorVersion ));
    DBUG(("PA - MinorVersion = 0x%x\n", osvi.dwMinorVersion ));
    /* Check for NT */
	if( (osvi.dwMajorVersion == 4) && (osvi.dwPlatformId == 2) )
	{
		minLatencyMsec = PA_WIN_NT_LATENCY;
	}
	else if(osvi.dwMajorVersion >= 5)
	{
		minLatencyMsec = PA_WIN_WDM_LATENCY;
	}
	else
	{
		minLatencyMsec = PA_WIN_9X_LATENCY;
	}
    return minLatencyMsec;
}

/*************************************************************************
** Determine minimum number of buffers required for this host based
** on minimum latency. Latency can be optionally set by user by setting
** an environment variable. For example, to set latency to 200 msec, put:
**
**    set PA_MIN_LATENCY_MSEC=200
**
** in the AUTOEXEC.BAT file and reboot.
** If the environment variable is not set, then the latency will be determined
** based on the OS. Windows NT has higher latency than Win95.
*/
#define PA_LATENCY_ENV_NAME  ("PA_MIN_LATENCY_MSEC")
#define PA_ENV_BUF_SIZE  (32)

static int PaWinDs_GetMinLatencyFrames( double sampleRate )
{
    char      envbuf[PA_ENV_BUF_SIZE];
    DWORD     hresult;
    int       minLatencyMsec = 0;

    /* Let user determine minimal latency by setting environment variable. */
    hresult = GetEnvironmentVariable( PA_LATENCY_ENV_NAME, envbuf, PA_ENV_BUF_SIZE );
    if( (hresult > 0) && (hresult < PA_ENV_BUF_SIZE) )
    {
        minLatencyMsec = atoi( envbuf );
    }
    else
    {
        minLatencyMsec = PaWinDS_GetMinSystemLatency();
#if PA_USE_HIGH_LATENCY
        PRINT(("PA - Minimum Latency set to %d msec!\n", minLatencyMsec ));
#endif

    }

    return (int) (minLatencyMsec * sampleRate * SECONDS_PER_MSEC);
}

/***********************************************************************************/
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
    PaWinDsHostApiRepresentation *winDsHostApi = (PaWinDsHostApiRepresentation*)hostApi;
    PaWinDsStream *stream = 0;
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
              sampleRate, streamFlags, framesPerBuffer,
              framesPerBuffer, /* ignored in paUtilVariableHostBufferSizePartialUsageAllowed mode. */
        /* This next mode is required because DS can split the host buffer when it wraps around. */
              paUtilVariableHostBufferSizePartialUsageAllowed,
              callback, userData );
    if( result != paNoError )
        goto error;

/* DirectSound specific initialization */
    {
        HRESULT          hr;
        int              bytesPerDirectSoundBuffer;
        DSoundWrapper   *dsw;
        int              userLatencyFrames;
        int              minLatencyFrames;

        stream->timerID = 0;
        dsw = &stream->directSoundWrapper;
        DSW_Init( dsw );

    /* Get system minimum latency. */
        minLatencyFrames = PaWinDs_GetMinLatencyFrames( sampleRate );

    /* Let user override latency by passing latency parameter. */
        userLatencyFrames = (inputLatency > outputLatency) ? inputLatency : outputLatency;
        if( userLatencyFrames > 0 ) minLatencyFrames = userLatencyFrames;

    /* Calculate stream->framesPerDSBuffer depending on framesPerBuffer */
        if( framesPerBuffer == 0 )
        {
        /* App support variable framesPerBuffer */
            stream->framesPerDSBuffer = minLatencyFrames;

            stream->outputLatency = (double)(minLatencyFrames - 1) / sampleRate;
        }
        else
        {
        /* Round up to number of buffers needed to guarantee that latency. */
            int numUserBuffers = (minLatencyFrames + framesPerBuffer - 1) / framesPerBuffer;
            if( numUserBuffers < 1 ) numUserBuffers = 1;
            numUserBuffers += 1; /* So we have latency worth of buffers ahead of current buffer. */
            stream->framesPerDSBuffer = framesPerBuffer * numUserBuffers;

            stream->outputLatency = (double)(framesPerBuffer * (numUserBuffers-1)) / sampleRate;
        }

        {
            // REVIEW: this calculation seems incorrect to me - rossb.
            int msecLatency = (int) ((stream->framesPerDSBuffer * MSEC_PER_SECOND) / sampleRate);
            PRINT(("PortAudio on DirectSound - Latency = %d frames, %d msec\n", stream->framesPerDSBuffer, msecLatency ));
        }


        /* ------------------ OUTPUT */
        if( (outputDevice >= 0) && (numOutputChannels > 0) )
        {
            PaDeviceInfo *deviceInfo = hostApi->deviceInfos[ outputDevice ];
            DBUG(("PaHost_OpenStream: deviceID = 0x%x\n", outputDevice));

            bytesPerDirectSoundBuffer = stream->framesPerDSBuffer * numOutputChannels * sizeof(short);
            if( bytesPerDirectSoundBuffer < DSBSIZE_MIN )
            {
                result = paBufferTooSmall;
                goto error;
            }
            else if( bytesPerDirectSoundBuffer > DSBSIZE_MAX )
            {
                result = paBufferTooBig;
                goto error;
            }


            hr = dswDSoundEntryPoints.DirectSoundCreate( winDsHostApi->winDsDeviceInfos[outputDevice].lpGUID,
                &dsw->dsw_pDirectSound,   NULL );
            if( hr != DS_OK )
            {
                ERR_RPT(("PortAudio: DirectSoundCreate() failed!\n"));
                result = paHostError;
                goto error;
            }
            hr = DSW_InitOutputBuffer( dsw,
                                       (unsigned long) (sampleRate + 0.5),
                                       numOutputChannels, bytesPerDirectSoundBuffer );
            DBUG(("DSW_InitOutputBuffer() returns %x\n", hr));
            if( hr != DS_OK )
            {
                result = paHostError;
                goto error;
            }
            /* Calculate value used in latency calculation to avoid real-time divides. */
            stream->secondsPerHostByte = 1.0 /
                (stream->bufferProcessor.bytesPerHostOutputSample *
                numOutputChannels * sampleRate);
        }

        /* ------------------ INPUT */
        if( (inputDevice >= 0) && (numInputChannels > 0) )
        {
            PaDeviceInfo *deviceInfo = hostApi->deviceInfos[ inputDevice ];
            
            bytesPerDirectSoundBuffer = stream->framesPerDSBuffer * numInputChannels * sizeof(short);
            if( bytesPerDirectSoundBuffer < DSBSIZE_MIN )
            {
                result = paBufferTooSmall;
                goto error;
            }
            else if( bytesPerDirectSoundBuffer > DSBSIZE_MAX )
            {
                result = paBufferTooBig;
                goto error;
            }

            hr = dswDSoundEntryPoints.DirectSoundCaptureCreate( winDsHostApi->winDsDeviceInfos[inputDevice].lpGUID,
                &dsw->dsw_pDirectSoundCapture,   NULL );
            if( hr != DS_OK )
            {
                ERR_RPT(("PortAudio: DirectSoundCaptureCreate() failed!\n"));
                result = paHostError;
                goto error;
            }
            hr = DSW_InitInputBuffer( dsw,
                                      (unsigned long) (sampleRate + 0.5),
                                      numInputChannels, bytesPerDirectSoundBuffer );
            DBUG(("DSW_InitInputBuffer() returns %x\n", hr));
            if( hr != DS_OK )
            {
                ERR_RPT(("PortAudio: DSW_InitInputBuffer() returns %x\n", hr));
                result = paHostError;
                goto error;
            }
        }

    }

    stream->sampleRate = sampleRate;

    *s = (PaStream*)stream;

    return result;

error:
    if( stream )
        PaUtil_FreeMemory( stream );

    return result;
}


/***********************************************************************************/
static PaError Pa_TimeSlice( PaWinDsStream *stream )
{
    PaError           result = 0;
    DSoundWrapper    *dsw;
    long              numFrames = 0;
    long              bytesEmpty = 0;
    long              bytesFilled = 0;
    long              bytesToXfer = 0;
    long              framesToXfer = 0;
    long              numInFramesReady = 0;
    long              numOutFramesReady = 0;
    long              bytesProcessed;
    HRESULT           hresult;
    double            outputLatency = 0;
    double            outTime;
/* Input */
    LPBYTE            lpInBuf1 = NULL;
    LPBYTE            lpInBuf2 = NULL;
    DWORD             dwInSize1 = 0;
    DWORD             dwInSize2 = 0;
/* Output */
    LPBYTE            lpOutBuf1 = NULL;
    LPBYTE            lpOutBuf2 = NULL;
    DWORD             dwOutSize1 = 0;
    DWORD             dwOutSize2 = 0;

    dsw = &stream->directSoundWrapper;

    /* How much input data is available? */
    if( stream->bufferProcessor.numInputChannels > 0 )
    {
        DSW_QueryInputFilled( dsw, &bytesFilled );
        framesToXfer = numInFramesReady = bytesFilled / dsw->dsw_BytesPerInputFrame;
        outputLatency = ((double)bytesFilled) * stream->secondsPerHostByte;
    }

    /* How much output room is available? */
    if( stream->bufferProcessor.numOutputChannels > 0 )
    {
        DSW_QueryOutputSpace( dsw, &bytesEmpty );
        framesToXfer = numOutFramesReady = bytesEmpty / dsw->dsw_BytesPerOutputFrame;
    }

    if( (numInFramesReady > 0) && (numOutFramesReady > 0) )
    {
        framesToXfer = (numOutFramesReady < numInFramesReady) ? numOutFramesReady : numInFramesReady;
    }

    if( framesToXfer > 0 )
    {

        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

    /* The outTime parameter should indicates the time at which
        the first sample of the output buffer is heard at the DACs. */
        outTime = PaUtil_GetTime() + outputLatency;


        PaUtil_BeginBufferProcessing( &stream->bufferProcessor, outTime );

    /* Input */
        if( stream->bufferProcessor.numInputChannels > 0 )
        {
            bytesToXfer = framesToXfer * dsw->dsw_BytesPerInputFrame;
            hresult = IDirectSoundCaptureBuffer_Lock ( dsw->dsw_InputBuffer,
                dsw->dsw_ReadOffset, bytesToXfer,
                (void **) &lpInBuf1, &dwInSize1,
                (void **) &lpInBuf2, &dwInSize2, 0);
            if (hresult != DS_OK)
            {
                ERR_RPT(("DirectSound IDirectSoundCaptureBuffer_Lock failed, hresult = 0x%x\n",hresult));
                result = paHostError;
                goto error2;
            }

            numFrames = dwInSize1 / dsw->dsw_BytesPerInputFrame;
            PaUtil_SetInputFrameCount( &stream->bufferProcessor, numFrames );
            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0, lpInBuf1, 0 );
        /* Is input split into two regions. */
            if( dwInSize2 > 0 )
            {
                numFrames = dwInSize2 / dsw->dsw_BytesPerInputFrame;
                PaUtil_Set2ndInputFrameCount( &stream->bufferProcessor, numFrames );
                PaUtil_Set2ndInterleavedInputChannels( &stream->bufferProcessor, 0, lpInBuf2, 0 );
            }
        }

    /* Output */
        if( stream->bufferProcessor.numOutputChannels > 0 )
        {
            bytesToXfer = framesToXfer * dsw->dsw_BytesPerOutputFrame;
            hresult = IDirectSoundBuffer_Lock ( dsw->dsw_OutputBuffer,
                dsw->dsw_WriteOffset, bytesToXfer,
                (void **) &lpOutBuf1, &dwOutSize1,
                (void **) &lpOutBuf2, &dwOutSize2, 0);
            if (hresult != DS_OK)
            {
                ERR_RPT(("DirectSound IDirectSoundBuffer_Lock failed, hresult = 0x%x\n",hresult));
                result = paHostError;
                goto error1;
            }

            numFrames = dwOutSize1 / dsw->dsw_BytesPerOutputFrame;
            PaUtil_SetOutputFrameCount( &stream->bufferProcessor, numFrames );
            PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0, lpOutBuf1, 0 );

        /* Is output split into two regions. */
            if( dwOutSize2 > 0 )
            {
                numFrames = dwOutSize2 / dsw->dsw_BytesPerOutputFrame;
                PaUtil_Set2ndOutputFrameCount( &stream->bufferProcessor, numFrames );
                PaUtil_Set2ndInterleavedOutputChannels( &stream->bufferProcessor, 0, lpOutBuf2, 0 );
            }
        }
        
        numFrames = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &result );
        stream->framesWritten += numFrames;
        
        if( stream->bufferProcessor.numOutputChannels > 0 )
        {
        /* Update our buffer offset and unlock sound buffer */
            bytesProcessed = numFrames * dsw->dsw_BytesPerOutputFrame;
            dsw->dsw_WriteOffset = (dsw->dsw_WriteOffset + bytesProcessed) % dsw->dsw_OutputSize;
            IDirectSoundBuffer_Unlock( dsw->dsw_OutputBuffer, lpOutBuf1, dwOutSize1, lpOutBuf2, dwOutSize2);
            dsw->dsw_FramesWritten += numFrames;
        }

error1:
        if( stream->bufferProcessor.numInputChannels > 0 )
        {
        /* Update our buffer offset and unlock sound buffer */
            bytesProcessed = numFrames * dsw->dsw_BytesPerInputFrame;
            dsw->dsw_ReadOffset = (dsw->dsw_ReadOffset + bytesProcessed) % dsw->dsw_InputSize;
            IDirectSoundCaptureBuffer_Unlock( dsw->dsw_InputBuffer, lpInBuf1, dwInSize1, lpInBuf2, dwInSize2);
        }
error2:

        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, numFrames );

    }
    
    return result;
}
/*******************************************************************/
static void CALLBACK Pa_TimerCallback(UINT uID, UINT uMsg, DWORD dwUser, DWORD dw1, DWORD dw2)
{
    PaWinDsStream *stream;

    stream = (PaWinDsStream *) dwUser;
    if( stream == NULL ) return;

    if( stream->isActive )
    {
        if( stream->abortProcessing )
        {
            stream->isActive = 0;
        }
        else if( stream->stopProcessing )
        {
            DSoundWrapper   *dsw = &stream->directSoundWrapper;
            if( stream->bufferProcessor.numOutputChannels > 0 )
            {
                DSW_ZeroEmptySpace( dsw );
                /* clear isActive when all sound played */
                if( dsw->dsw_FramesPlayed >= stream->framesWritten )
                {
                    stream->isActive = 0;
                }
            }
            else
            {
                stream->isActive = 0;
            }
        }
        else
        {
            if( Pa_TimeSlice( stream ) != 0)  /* Call time slice independant of timing method. */
            {
                stream->stopProcessing = 1;
            }
        }
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

    DSW_Term( &stream->directSoundWrapper );

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

    return result;
}

/***********************************************************************************/
static PaError StartStream( PaStream *s )
{
    PaError          result = paNoError;
    PaWinDsStream   *stream = (PaWinDsStream*)s;
    HRESULT          hr;

    if( stream->bufferProcessor.numInputChannels > 0 )
    {
        hr = DSW_StartInput( &stream->directSoundWrapper );
        DBUG(("StartStream: DSW_StartInput returned = 0x%X.\n", hr));
        if( hr != DS_OK )
        {
            result = paHostError;
            goto error;
        }
    }

    stream->framesWritten = 0;

    stream->abortProcessing = 0;
    stream->stopProcessing = 0;
    stream->isActive = 1;

    if( stream->bufferProcessor.numOutputChannels > 0 )
    {
        /* Give user callback a chance to pre-fill buffer. */
        result = Pa_TimeSlice( stream );
        if( result != paNoError ) return result; // FIXME - what if finished?

        hr = DSW_StartOutput( &stream->directSoundWrapper );
        DBUG(("PaHost_StartOutput: DSW_StartOutput returned = 0x%X.\n", hr));
        if( hr != DS_OK )
        {
            result = paHostError;
            goto error;
        }
    }


    /* Create timer that will wake us up so we can fill the DSound buffer. */
    {
        int resolution;
        int framesPerWakeup = stream->framesPerDSBuffer / 4;
        int msecPerWakeup = MSEC_PER_SECOND * framesPerWakeup / (int) stream->sampleRate;
        if( msecPerWakeup < 10 ) msecPerWakeup = 10;
        else if( msecPerWakeup > 100 ) msecPerWakeup = 100;
        resolution = msecPerWakeup/4;
        stream->timerID = timeSetEvent( msecPerWakeup, resolution, (LPTIMECALLBACK) Pa_TimerCallback,
                                             (DWORD) stream, TIME_PERIODIC );
    }
    if( stream->timerID == 0 )
    {
        stream->isActive = 0;
        result = paHostError;
        goto error;
    }

    stream->isStarted = TRUE;

error:
    return result;
}


/***********************************************************************************/
static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinDsStream *stream = (PaWinDsStream*)s;
    HRESULT          hr;
    int timeoutMsec;

    stream->stopProcessing = 1;
    /* Set timeout at 20% beyond maximum time we might wait. */
    timeoutMsec = (int) (1200.0 * stream->framesPerDSBuffer / stream->sampleRate);
    while( stream->isActive && (timeoutMsec > 0)  )
    {
        Sleep(10);
        timeoutMsec -= 10;
    }
    if( stream->timerID != 0 )
    {
        timeKillEvent(stream->timerID);  /* Stop callback timer. */
        stream->timerID = 0;
    }


    if( stream->bufferProcessor.numOutputChannels > 0 )
    {
        hr = DSW_StopOutput( &stream->directSoundWrapper );
    }

    if( stream->bufferProcessor.numInputChannels > 0 )
    {
        hr = DSW_StopInput( &stream->directSoundWrapper );
    }

    stream->isStarted = FALSE;

    return result;
}


/***********************************************************************************/
static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinDsStream *stream = (PaWinDsStream*)s;

    stream->abortProcessing = 1;
    return StopStream( s );
}


/***********************************************************************************/
static PaError IsStreamStopped( PaStream *s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    return !stream->isStarted;
}


/***********************************************************************************/
static PaError IsStreamActive( PaStream *s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    return stream->isActive;
}


/***********************************************************************************/
static PaTime GetStreamInputLatency( PaStream *s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}

/***********************************************************************************/
static PaTime GetStreamOutputLatency( PaStream *s )
{
    PaWinDsStream *stream = (PaWinDsStream*)s;

    return stream->outputLatency;
}

/***********************************************************************************/
static PaTime GetStreamTime( PaStream *s )
{
/*
    new behavior for GetStreamTime is to return a stream based seconds clock
    used for the outTime parameter to the callback.
    FIXME: delete this comment when the other unnecessary related code has
    been cleaned from this file.

    PaWinDsStream *stream = (PaWinDsStream*)s;
    DSoundWrapper   *dsw;
    dsw = &stream->directSoundWrapper;
    return dsw->dsw_FramesPlayed;
*/
    return PaUtil_GetTime();
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



