/*
 * $Id$
 * pa_win_wmme.c
 * Implementation of PortAudio for Windows MultiMedia Extensions (WMME)
 *
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 *
 * Authors: Ross Bencina and Phil Burk
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
/*
  All memory allocations and frees are marked with MEM for quick review.
*/

/* Modification History:
 PLB = Phil Burk
 JM = Julien Maillard
 RDB = Ross Bencina
 PLB20010402 - sDevicePtrs now allocates based on sizeof(pointer)
 PLB20010413 - check for excessive numbers of channels
 PLB20010422 - apply Mike Berry's changes for CodeWarrior on PC
               including condition including of memory.h,
               and explicit typecasting on memory allocation
 PLB20010802 - use GlobalAlloc for sDevicesPtr instead of PaHost_AllocFastMemory
 PLB20010816 - pass process instead of thread to SetPriorityClass()
 PLB20010927 - use number of frames instead of real-time for CPULoad calculation.
 JM20020118 - prevent hung thread when buffers underflow.
 PLB20020321 - detect Win XP versus NT, 9x; fix DBUG typo; removed init of CurrentCount
 RDB20020411 - various renaming cleanups, factored streamData alloc and cpu usage init
 RDB20020417 - stopped counting WAVE_MAPPER when there were no real devices
               refactoring, renaming and fixed a few edge case bugs
 RDB20020531 - converted to V19 framework
*/

/*
TODO:

    - implement buffer size and number of buffers code

    - implement timecode param to callback

    - add default buffer size/number code from old implementation
    - add bufferslip management
    - add multidevice multichannel support
    - add thread throttling on overload
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>
#include <mmsystem.h>
#include <process.h>
#include <assert.h>
/* PLB20010422 - "memory.h" doesn't work on CodeWarrior for PC. Thanks Mike Berry for the mod. */
#ifndef __MWERKS__
#include <malloc.h>
#include <memory.h>
#endif /* __MWERKS__ */

#include "portaudio.h"
#include "pa_trace.h"
#include "pa_util.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_win_wmme.h"

/************************************************* Constants ********/

/* Switches for debugging. */
#define PA_SIMULATE_UNDERFLOW_    (0)  /* Set to one to force an underflow of the output buffer. */

#define PA_USE_HIGH_LATENCY_      (0)  /* For debugging glitches. */

#if PA_USE_HIGH_LATENCY_
 #define PA_MIN_MSEC_PER_HOST_BUFFER_  (100)
 #define PA_MAX_MSEC_PER_HOST_BUFFER_  (300) /* Do not exceed unless user buffer exceeds */
 #define PA_MIN_NUM_HOST_BUFFERS_      (4)
 #define PA_MAX_NUM_HOST_BUFFERS_      (16)  /* OK to exceed if necessary */
 #define PA_WIN_9X_LATENCY_            (400)
#else
 #define PA_MIN_MSEC_PER_HOST_BUFFER_  (10)
 #define PA_MAX_MSEC_PER_HOST_BUFFER_  (100) /* Do not exceed unless user buffer exceeds */
 #define PA_MIN_NUM_HOST_BUFFERS_      (3)
 #define PA_MAX_NUM_HOST_BUFFERS_      (16)  /* OK to exceed if necessary */
 #define PA_WIN_9X_LATENCY_            (200)
#endif

#define PA_MIN_TIMEOUT_MSEC_                 (1000)

/*
** Use higher latency for NT because it is even worse at real-time
** operation than Win9x.
*/
#define PA_WIN_NT_LATENCY_        (PA_WIN_9X_LATENCY_ * 2)
#define PA_WIN_WDM_LATENCY_       (PA_WIN_9X_LATENCY_)


static const char constInputMapperSuffix_[] = " - Input";
static const char constOutputMapperSuffix_[] = " - Output";


typedef struct PaWinMmeStream PaWinMmeStream;     /* forward reference */

/* prototypes for functions declared in this file */

PaError PaWinMme_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** stream,
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

static PaError UpdateStreamTime( PaWinMmeStream *stream );


/* PaWinMmeHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation commonHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    int numInputDevices, numOutputDevices;
}
PaWinMmeHostApiRepresentation;


static void InitializeDeviceCount( PaWinMmeHostApiRepresentation *hostApi )
{
    hostApi->numInputDevices = waveInGetNumDevs();
    if( hostApi->numInputDevices > 0 )
    {
        hostApi->numInputDevices += 1; /* add one extra for the WAVE_MAPPER */
        hostApi->commonHostApiRep.defaultInputDeviceIndex = 0;
    }
    else
    {
        hostApi->commonHostApiRep.defaultInputDeviceIndex = paNoDevice;
    }

    hostApi->numOutputDevices = waveOutGetNumDevs();
    if( hostApi->numOutputDevices > 0 )
    {
        hostApi->numOutputDevices += 1; /* add one extra for the WAVE_MAPPER */
        hostApi->commonHostApiRep.defaultOutputDeviceIndex = hostApi->numInputDevices;
    }
    else
    {
        hostApi->commonHostApiRep.defaultOutputDeviceIndex = paNoDevice;
    }

    hostApi->commonHostApiRep.deviceCount =
        hostApi->numInputDevices + hostApi->numOutputDevices;
}

/*************************************************************************
 * Returns recommended device ID.
 * On the PC, the recommended device can be specified by the user by
 * setting an environment variable. For example, to use device #1.
 *
 *    set PA_RECOMMENDED_OUTPUT_DEVICE=1
 *
 * The user should first determine the available device ID by using
 * the supplied application "pa_devs".
 */
#define PA_ENV_BUF_SIZE_  (32)
#define PA_REC_IN_DEV_ENV_NAME_  ("PA_RECOMMENDED_INPUT_DEVICE")
#define PA_REC_OUT_DEV_ENV_NAME_  ("PA_RECOMMENDED_OUTPUT_DEVICE")
static PaDeviceIndex GetEnvDefaultDeviceID( char *envName )
{
    PaDeviceIndex recommendedIndex = paNoDevice;
    DWORD   hresult;
    char    envbuf[PA_ENV_BUF_SIZE_];

    /* Let user determine default device by setting environment variable. */
    hresult = GetEnvironmentVariable( envName, envbuf, PA_ENV_BUF_SIZE_ );
    if( (hresult > 0) && (hresult < PA_ENV_BUF_SIZE_) )
    {
        recommendedIndex = atoi( envbuf );
    }
    return recommendedIndex;
}

static void InitializeDefaultDeviceIdsFromEnv( PaWinMmeHostApiRepresentation *hostApi )
{
    PaDeviceIndex device;

    /* input */
    device = GetEnvDefaultDeviceID( PA_REC_IN_DEV_ENV_NAME_ );
    if( device != paNoDevice &&
            ( device >= 0 && device < hostApi->commonHostApiRep.deviceCount ) &&
            hostApi->commonHostApiRep.deviceInfos[ device ]->maxInputChannels > 0 )
    {
        hostApi->commonHostApiRep.defaultInputDeviceIndex = device;
    }

    /* output */
    device = GetEnvDefaultDeviceID( PA_REC_OUT_DEV_ENV_NAME_ );
    if( device != paNoDevice &&
            ( device >= 0 && device < hostApi->commonHostApiRep.deviceCount ) &&
            hostApi->commonHostApiRep.deviceInfos[ device ]->maxOutputChannels > 0 )
    {
        hostApi->commonHostApiRep.defaultOutputDeviceIndex = device;
    }
}


/** Convert external PA ID to an internal ID that includes WAVE_MAPPER
    Note that WAVE_MAPPER is defined as -1
*/
static int LocalDeviceIndexToWinMmeDeviceId( PaWinMmeHostApiRepresentation *hostApi, PaDeviceIndex device )
{
    if( device < hostApi->numInputDevices )
        return device - 1;
    else
        return device - hostApi->numInputDevices - 1;
}


#define PA_NUM_STANDARDSAMPLINGRATES_   3   /* 11.025, 22.05, 44.1 */
#define PA_NUM_CUSTOMSAMPLINGRATES_     5   /* must be the same number of elements as in the array below */
#define PA_MAX_NUMSAMPLINGRATES_        (PA_NUM_STANDARDSAMPLINGRATES_+PA_NUM_CUSTOMSAMPLINGRATES_)
static DWORD customSamplingRates_[] = { 32000, 48000, 64000, 88200, 96000 };

static PaError InitializeInputDeviceInfo( PaWinMmeHostApiRepresentation *winMmeHostApi, PaDeviceInfo *deviceInfo, PaDeviceIndex deviceIndex )
{
    PaError result = paNoError;
    char *deviceName; /* non-const ptr */
    double *sampleRates; /* non-const ptr */
    int inputWinMmeId;
    MMRESULT mmresult;
    WAVEINCAPS wic;
    int i;

    sampleRates = (double*)deviceInfo->sampleRates;

    inputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, deviceIndex );

    mmresult = waveInGetDevCaps( inputWinMmeId, &wic, sizeof( WAVEINCAPS ) );
    if( mmresult != MMSYSERR_NOERROR )
    {
        result = paHostError;
        PaUtil_SetHostError( mmresult );
        goto error;
    }


    if( inputWinMmeId == WAVE_MAPPER )
    {
        /* Append I/O suffix to WAVE_MAPPER device. */
        deviceName = (char *) PaUtil_AllocateMemory( strlen( wic.szPname ) + 1 + sizeof(constInputMapperSuffix_) );
        if( !deviceName )
        {
            result = paInsufficientMemory;
            goto error;
        }
        strcpy( deviceName, wic.szPname );
        strcat( deviceName, constInputMapperSuffix_ );
    }
    else
    {
        deviceName = (char*)PaUtil_AllocateMemory( strlen( wic.szPname ) + 1 );
        if( !deviceName )
        {
            result = paInsufficientMemory;
            goto error;
        }
        strcpy( deviceName, wic.szPname  );
    }
    deviceInfo->name = deviceName;

    deviceInfo->maxInputChannels = wic.wChannels;
    /* Sometimes a device can return a rediculously large number of channels.
     * This happened with an SBLive card on a Windows ME box.
     * If that happens, then force it to 2 channels.  PLB20010413
     */
    if( (deviceInfo->maxInputChannels < 1) || (deviceInfo->maxInputChannels > 256) )
    {
        PA_DEBUG(("Pa_GetDeviceInfo: Num input channels reported as %d! Changed to 2.\n", deviceInfo->maxOutputChannels ));
        deviceInfo->maxInputChannels = 2;
    }
    /* Add a sample rate to the list if we can do stereo 16 bit at that rate
     * based on the format flags. */
    if( wic.dwFormats & WAVE_FORMAT_1M16 ||wic.dwFormats & WAVE_FORMAT_1S16 )
        sampleRates[ deviceInfo->numSampleRates++ ] = 11025.;
    if( wic.dwFormats & WAVE_FORMAT_2M16 ||wic.dwFormats & WAVE_FORMAT_2S16 )
        sampleRates[ deviceInfo->numSampleRates++ ] = 22050.;
    if( wic.dwFormats & WAVE_FORMAT_4M16 ||wic.dwFormats & WAVE_FORMAT_4S16 )
        sampleRates[ deviceInfo->numSampleRates++ ] = 44100.;
    /* Add a sample rate to the list if we can do stereo 16 bit at that rate
     * based on opening the device successfully. */
    for( i=0; i < PA_NUM_CUSTOMSAMPLINGRATES_; ++i )
    {
        WAVEFORMATEX wfx;
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nSamplesPerSec = customSamplingRates_[i];
        wfx.wBitsPerSample = 16;
        wfx.cbSize = 0; /* ignored */
        wfx.nChannels = (WORD)deviceInfo->maxInputChannels;
        wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * sizeof(short);
        wfx.nBlockAlign = (WORD)(wfx.nChannels * sizeof(short));
        if( waveInOpen( NULL, inputWinMmeId, &wfx, 0, 0, WAVE_FORMAT_QUERY ) == MMSYSERR_NOERROR )
        {
            sampleRates[ deviceInfo->numSampleRates++ ] = customSamplingRates_[i];
        }
    }

error:
    return result;
}


static PaError InitializeOutputDeviceInfo( PaWinMmeHostApiRepresentation *winMmeHostApi, PaDeviceInfo *deviceInfo, PaDeviceIndex deviceIndex )
{
    PaError result = paNoError;
    char *deviceName; /* non-const ptr */
    double *sampleRates; /* non-const ptr */
    int outputWinMmeId;
    MMRESULT mmresult;
    WAVEOUTCAPS woc;
    int i;

    sampleRates = (double*)deviceInfo->sampleRates;

    outputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, deviceIndex );

    mmresult = waveOutGetDevCaps( outputWinMmeId, &woc, sizeof( WAVEOUTCAPS ) );
    if( mmresult != MMSYSERR_NOERROR )
    {
        result = paHostError;
        PaUtil_SetHostError( mmresult );
        goto error;
    }

    if( outputWinMmeId == WAVE_MAPPER )
    {
        /* Append I/O suffix to WAVE_MAPPER device. */
        deviceName = (char *) PaUtil_AllocateMemory( strlen( woc.szPname ) + 1 + sizeof(constOutputMapperSuffix_) );
        if( !deviceName )
        {
            result = paInsufficientMemory;
            goto error;
        }
        strcpy( deviceName, woc.szPname );
        strcat( deviceName, constOutputMapperSuffix_ );
    }
    else
    {
        deviceName = (char*)PaUtil_AllocateMemory( strlen( woc.szPname ) + 1 );
        if( !deviceName )
        {
            result = paInsufficientMemory;
            goto error;
        }
        strcpy( deviceName, woc.szPname  );
    }
    deviceInfo->name = deviceName;

    deviceInfo->maxOutputChannels = woc.wChannels;
    /* Sometimes a device can return a rediculously large number of channels.
     * This happened with an SBLive card on a Windows ME box.
     * It also happens on Win XP!
     */
    if( (deviceInfo->maxOutputChannels < 1) || (deviceInfo->maxOutputChannels > 256) )
    {
#if 1
        deviceInfo->maxOutputChannels = 2;
#else
        /* If channel max is goofy, then query for max channels. PLB20020228
        * This doesn't seem to help. Disable code for now. Remove it later.
        */
        PA_DEBUG(("Pa_GetDeviceInfo: Num output channels reported as %d!", deviceInfo->maxOutputChannels ));
        deviceInfo->maxOutputChannels = 0;
        /* Attempt to find the correct maximum by querying the device. */
        for( i=2; i<16; i += 2 )
        {
            WAVEFORMATEX wfx;
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nSamplesPerSec = 44100;
            wfx.wBitsPerSample = 16;
            wfx.cbSize = 0; /* ignored */
            wfx.nChannels = (WORD) i;
            wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * sizeof(short);
            wfx.nBlockAlign = (WORD)(wfx.nChannels * sizeof(short));
            if( waveOutOpen( NULL, outputWinMmeId, &wfx, 0, 0, WAVE_FORMAT_QUERY ) == MMSYSERR_NOERROR )
            {
                deviceInfo->maxOutputChannels = i;
            }
            else
            {
                break;
            }
        }
#endif
        PA_DEBUG((" Changed to %d.\n", deviceInfo->maxOutputChannels ));
    }

    /* Add a sample rate to the list if we can do stereo 16 bit at that rate
     * based on the format flags. */
    if( woc.dwFormats & WAVE_FORMAT_1M16 ||woc.dwFormats & WAVE_FORMAT_1S16 )
        sampleRates[ deviceInfo->numSampleRates++ ] = 11025.;
    if( woc.dwFormats & WAVE_FORMAT_2M16 ||woc.dwFormats & WAVE_FORMAT_2S16 )
        sampleRates[ deviceInfo->numSampleRates++ ] = 22050.;
    if( woc.dwFormats & WAVE_FORMAT_4M16 ||woc.dwFormats & WAVE_FORMAT_4S16 )
        sampleRates[ deviceInfo->numSampleRates++ ] = 44100.;

    /* Add a sample rate to the list if we can do stereo 16 bit at that rate
     * based on opening the device successfully. */
    for( i=0; i < PA_NUM_CUSTOMSAMPLINGRATES_; i++ )
    {
        WAVEFORMATEX wfx;
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nSamplesPerSec = customSamplingRates_[i];
        wfx.wBitsPerSample = 16;
        wfx.cbSize = 0; /* ignored */
        wfx.nChannels = (WORD)deviceInfo->maxOutputChannels;
        wfx.nAvgBytesPerSec = wfx.nChannels * wfx.nSamplesPerSec * sizeof(short);
        wfx.nBlockAlign = (WORD)(wfx.nChannels * sizeof(short));
        if( waveOutOpen( NULL, outputWinMmeId, &wfx, 0, 0, WAVE_FORMAT_QUERY ) == MMSYSERR_NOERROR )
        {
            sampleRates[ deviceInfo->numSampleRates++ ] = customSamplingRates_[i];
        }
    }

error:
    return result;
}


PaError PaWinMme_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaWinMmeHostApiRepresentation *winMmeHostApi = 0;
    PaDeviceInfo *deviceInfoArray = 0;
    double *sampleRates; /* non-const ptr */

    winMmeHostApi = (PaWinMmeHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaWinMmeHostApiRepresentation) );
    if( !winMmeHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    *hostApi = &winMmeHostApi->commonHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paWin32MME;
    (*hostApi)->info.name = "Windows MME";

    InitializeDeviceCount( winMmeHostApi );
    deviceCount = (*hostApi)->deviceCount;

    (*hostApi)->deviceInfos = 0;

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

        /* initializes buffer ptrs to zero so they can be deallocated on error */
        for( i=0; i < deviceCount; ++i )
        {
            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            (*hostApi)->deviceInfos[i] = deviceInfo;
            deviceInfo->name = 0;
            deviceInfo->sampleRates = 0;
        }

        for( i=0; i < deviceCount; ++i )
        {
            PaDeviceInfo *deviceInfo = (*hostApi)->deviceInfos[i];

            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = index;

            deviceInfo->maxInputChannels = 0;
            deviceInfo->maxOutputChannels = 0;
            deviceInfo->numSampleRates = 0;

            sampleRates = (double*)PaUtil_AllocateMemory( PA_MAX_NUMSAMPLINGRATES_ * sizeof(double) );
            if( !sampleRates )
            {
                result = paInsufficientMemory;
                goto error;
            }

            deviceInfo->sampleRates = sampleRates;
            deviceInfo->nativeSampleFormats = paInt16;

            if( i < winMmeHostApi->numInputDevices )
            {
                result = InitializeInputDeviceInfo( winMmeHostApi, deviceInfo, i );
                if( result != paNoError )
                    goto error;

            }
            else
            {
                result = InitializeOutputDeviceInfo( winMmeHostApi, deviceInfo, i );
                if( result != paNoError )
                    goto error;
            }
        }
    }
    else
    {
        (*hostApi)->deviceInfos = 0;
    }

    InitializeDefaultDeviceIdsFromEnv( winMmeHostApi );

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;


    PaUtil_InitializeStreamInterface( &winMmeHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive, GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite, PaUtil_DummyGetAvailable, PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &winMmeHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( winMmeHostApi )
    {
        if( deviceCount > 0 )
        {
            if( winMmeHostApi->commonHostApiRep.deviceInfos )
            {
                if( winMmeHostApi->commonHostApiRep.deviceInfos[0] )
                {
                    for( i=0; i < deviceCount; ++i )
                    {
                        if( winMmeHostApi->commonHostApiRep.deviceInfos[0]->name )
                            PaUtil_FreeMemory( (void*)winMmeHostApi->commonHostApiRep.deviceInfos[0]->name );

                        if( winMmeHostApi->commonHostApiRep.deviceInfos[0]->sampleRates )
                            PaUtil_FreeMemory( (void*)winMmeHostApi->commonHostApiRep.deviceInfos[0]->sampleRates );
                    }

                    PaUtil_FreeMemory( winMmeHostApi->commonHostApiRep.deviceInfos[0] );
                }

                PaUtil_FreeMemory( winMmeHostApi->commonHostApiRep.deviceInfos );
            }
        }
        PaUtil_FreeMemory( winMmeHostApi );
    }

    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaWinMmeHostApiRepresentation *winMmeHostApi = (PaWinMmeHostApiRepresentation*)hostApi;
    int deviceCount = winMmeHostApi->commonHostApiRep.deviceCount;
    int i;

    if( deviceCount > 0 )
    {
        for( i=0; i < deviceCount; ++i )
        {
            PaUtil_FreeMemory( (void*)winMmeHostApi->commonHostApiRep.deviceInfos[i]->name );
            PaUtil_FreeMemory( (void*)winMmeHostApi->commonHostApiRep.deviceInfos[i]->sampleRates );
        }

        PaUtil_FreeMemory( hostApi->deviceInfos[0] );
        PaUtil_FreeMemory( hostApi->deviceInfos );
    }

    PaUtil_FreeMemory( winMmeHostApi );
}


/*
    NOTE: this is a hack to allow InitializeBufferSet et al to be used on both
    input and output buffer sets. we assume that it is safe to kludge some
    types and store waveIn and waveOut preparation functions and handles
    in storage typed for wave in only.
*/
typedef WINMMAPI MMRESULT WINAPI MmePrepareHeader(
    HWAVEIN hwi,
    LPWAVEHDR pwh,
    UINT cbwh
);


typedef HWAVEIN MmeHandle;

static PaError InitializeBufferSet( WAVEHDR **bufferSet, int numBuffers, int bufferBytes,
                                    MmePrepareHeader *prepareHeader, MmePrepareHeader *unprepareHeader, MmeHandle mmeWaveHandle )
{
    PaError result = paNoError;
    MMRESULT mmresult;
    int i;

    *bufferSet = 0;

    /* Allocate an array to hold the buffer pointers. */
    *bufferSet = (WAVEHDR *) PaUtil_AllocateMemory( sizeof(WAVEHDR)*numBuffers );
    if( !*bufferSet )
    {
        result = paInsufficientMemory;
        goto error;
    }

    for( i=0; i<numBuffers; ++i )
    {
        (*bufferSet)[i].lpData = 0;
    }

    /* Allocate each buffer. */
    for( i=0; i<numBuffers; ++i )
    {
        (*bufferSet)[i].lpData = (char *)PaUtil_AllocateMemory( bufferBytes );
        if( !(*bufferSet)[i].lpData )
        {
            result = paInsufficientMemory;
            goto error;
        }
        (*bufferSet)[i].dwBufferLength = bufferBytes;
        (*bufferSet)[i].dwUser = 0xFFFFFFFF; /* indicates unprepared to error clean up code */

        mmresult = prepareHeader( mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );
        if( mmresult != MMSYSERR_NOERROR )
        {
            result = paHostError;
            PaUtil_SetHostError( mmresult );
            goto error;
        }
        (*bufferSet)[i].dwUser = i;
    }

    return result;

error:
    if( *bufferSet )
    {
        for( i=0; i<numBuffers; ++i )
        {
            if( (*bufferSet)[i].lpData )
            {

                if( (*bufferSet)[i].dwUser != 0xFFFFFFFF )
                    unprepareHeader( mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );

                PaUtil_FreeMemory( (*bufferSet)[i].lpData );
            }
        }

        PaUtil_FreeMemory( *bufferSet );
    }

    return result;
}


static void TerminateBufferSet( WAVEHDR * *bufferSet, unsigned int numBuffers, MmePrepareHeader *unprepareHeader, MmeHandle mmeWaveHandle )
{
    unsigned int i;

    for( i=0; i<numBuffers; ++i )
    {
        if( (*bufferSet)[i].lpData )
        {
            unprepareHeader( mmeWaveHandle, &(*bufferSet)[i], sizeof(WAVEHDR) );

            PaUtil_FreeMemory( (*bufferSet)[i].lpData );
        }
    }

    if( *bufferSet )
        PaUtil_FreeMemory( *bufferSet );
}


/* PaWinMmeStream - a stream data structure specifically for this implementation */

typedef struct PaWinMmeStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    CRITICAL_SECTION lock;

    /* Input -------------- */
    HWAVEIN hWaveIn;
    unsigned int numInputChannels;
    WAVEHDR *inputBuffers;
    unsigned int numInputBuffers;
    unsigned int currentInputBufferIndex;

    /* Output -------------- */
    HWAVEOUT hWaveOut;
    unsigned int numOutputChannels;
    WAVEHDR *outputBuffers;
    unsigned int numOutputBuffers;
    unsigned int currentOutputBufferIndex;

    /* Processing thread management -------------- */
    HANDLE abortEvent;
    HANDLE bufferEvent;
    HANDLE processingThread;
    DWORD processingThreadId;

    volatile int isActive;
    volatile int stopProcessing; /* stop thread once existing buffers have been returned */
    volatile int abortProcessing; /* stop thread immediately */

    DWORD allBuffersDurationMs; /* used to calculate timeouts */

    /* GetStreamTime() support ------------- */

    PaTimestamp streamPosition;
    long previousStreamPosition;                /* used to track frames played. */
}
PaWinMmeStream;


/* the following macros are intended to improve the readability of the following code */
#define PA_IS_INPUT_STREAM_( stream ) ( stream ->hWaveIn)
#define PA_IS_OUTPUT_STREAM_( stream ) ( stream ->hWaveOut)
#define PA_IS_FULL_DUPLEX_STREAM_( stream ) ( stream ->hWaveIn && stream ->hWaveOut)


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
    PaWinMmeHostApiRepresentation *winMmeHostApi = (PaWinMmeHostApiRepresentation*)hostApi;
    PaWinMmeStream *stream = 0;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
    unsigned long bytesPerInputFrame, bytesPerOutputFrame;
    int numHostInputBuffers = 0;
    int numHostOutputBuffers = 0;
    int framesPerHostInputBuffer = 0;
    int framesPerHostOutputBuffer = 0;
    int framesPerBufferProcessorCall;  
    int lockInited = 0;
    int bufferEventInited = 0;
    int abortEventInited = 0;
    WAVEFORMATEX wfx;
    MMRESULT mmresult;


    /* check that input device can support numInputChannels */
    if( inputDevice != paNoDevice &&
            numInputChannels > hostApi->deviceInfos[ inputDevice ]->maxInputChannels )
        return paInvalidChannelCount;


    /* check that output device can support numInputChannels */
    if( outputDevice != paNoDevice &&
            numOutputChannels > hostApi->deviceInfos[ outputDevice ]->maxOutputChannels )
        return paInvalidChannelCount;


    /*
        IMPLEMENT ME:
            - alter sampleRate to a close allowable rate if possible / necessary
    */


    /* validate inputStreamInfo */
    if( inputStreamInfo )
    {
        if( ((PaWinMmeStreamInfo*)inputStreamInfo)->header.size != sizeof( PaWinMmeStreamInfo )
                || ((PaWinMmeStreamInfo*)inputStreamInfo)->header.version != 1 )
        {
            return paIncompatibleStreamInfo;
        }

        if( ((PaWinMmeStreamInfo*)inputStreamInfo)->flags & PaWinMmeUseLowLevelLatencyParameters )
        {
            if( ((PaWinMmeStreamInfo*)inputStreamInfo)->numBuffers <= 0
                    || ((PaWinMmeStreamInfo*)inputStreamInfo)->framesPerBuffer <= 0 )
            {
                return paIncompatibleStreamInfo;
            }

            numHostInputBuffers = ((PaWinMmeStreamInfo*)inputStreamInfo)->numBuffers;
            framesPerHostInputBuffer = ((PaWinMmeStreamInfo*)inputStreamInfo)->framesPerBuffer;
        }

        /* IMPLEMENT ME: validate multidevice fields */
    }

    /* validate outputStreamInfo */
    if( outputStreamInfo )
    {
        if( ((PaWinMmeStreamInfo*)outputStreamInfo)->header.size != sizeof( PaWinMmeStreamInfo )
                || ((PaWinMmeStreamInfo*)outputStreamInfo)->header.version != 1 )
        {
            return paIncompatibleStreamInfo;
        }

        if( ((PaWinMmeStreamInfo*)outputStreamInfo)->flags & PaWinMmeUseLowLevelLatencyParameters )
        {
            if( ((PaWinMmeStreamInfo*)outputStreamInfo)->numBuffers <= 0
                    || ((PaWinMmeStreamInfo*)outputStreamInfo)->framesPerBuffer <= 0 )
            {
                return paIncompatibleStreamInfo;
            }

            numHostOutputBuffers = ((PaWinMmeStreamInfo*)outputStreamInfo)->numBuffers;
            framesPerHostOutputBuffer = ((PaWinMmeStreamInfo*)outputStreamInfo)->framesPerBuffer;
        }

        /* IMPLEMENT ME: validate multidevice fields */                 
    }

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */


    stream = (PaWinMmeStream*)PaUtil_AllocateMemory( sizeof(PaWinMmeStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    stream->hWaveIn = 0;
    stream->hWaveOut = 0;
    stream->processingThread = 0;

    PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                           &winMmeHostApi->callbackStreamInterface, callback, userData );

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );



    /* FIXME: establish which host formats are available */
    hostInputSampleFormat =
        PaUtil_SelectClosestAvailableFormat( paInt16 /* native formats */, inputSampleFormat );

    /* FIXME: establish which host formats are available */
    hostOutputSampleFormat =
        PaUtil_SelectClosestAvailableFormat( paInt16 /* native formats */, outputSampleFormat );


    /* calculate frames per buffer and number of buffers from generic
        latency parameters if specific values have not been provided */
    /* FIXME: derive values from latency params */

    if( inputDevice != paNoDevice && numHostInputBuffers == 0 )
    {
        framesPerHostInputBuffer = 16384;
        numHostInputBuffers = 4;
    }

    if( outputDevice != paNoDevice && numHostOutputBuffers == 0 )
    {
        framesPerHostOutputBuffer = 16384;
        numHostOutputBuffers = 4;
    }

    framesPerBufferProcessorCall = (framesPerHostInputBuffer < framesPerHostOutputBuffer )
            ? framesPerHostInputBuffer : framesPerHostOutputBuffer;


    if( inputDevice != paNoDevice && outputDevice != paNoDevice )
    {
        /*
            either input and output buffers must be the same size, or the
            larger one must be an integer multiple of the smaller one.
        */
        assert( framesPerHostInputBuffer % framesPerBufferProcessorCall == 0 );
        assert( framesPerHostOutputBuffer % framesPerBufferProcessorCall == 0 );
    }

                                  
    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              numInputChannels, inputSampleFormat, hostInputSampleFormat,
              numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerCallback, framesPerBufferProcessorCall,
              callback, userData );
    if( result != paNoError )
        goto error;

    stream->isActive = 0;

    stream->streamPosition = 0.;
    stream->previousStreamPosition = 0;


    stream->bufferEvent = CreateEvent( NULL, FALSE, FALSE, NULL );
    if( stream->bufferEvent == NULL )
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }
    bufferEventInited = 1;

    if( inputDevice != paNoDevice )
    {
        int inputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, inputDevice );

        bytesPerInputFrame = numInputChannels * stream->bufferProcessor.bytesPerHostInputSample;

        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = (WORD) numInputChannels;
        wfx.nSamplesPerSec = (DWORD) sampleRate;
        wfx.nAvgBytesPerSec = (DWORD)(bytesPerInputFrame * sampleRate);
        wfx.nBlockAlign = (WORD)bytesPerInputFrame;
        wfx.wBitsPerSample = (WORD)((bytesPerInputFrame/numInputChannels) * 8);
        wfx.cbSize = 0;

        /* REVIEW: consider not firing an event for input when a full duplex stream is being used */
        mmresult = waveInOpen( &stream->hWaveIn, inputWinMmeId, &wfx,
                               (DWORD)stream->bufferEvent, (DWORD) stream, CALLBACK_EVENT );
        if( mmresult != MMSYSERR_NOERROR )
        {
            result = paHostError;
            PaUtil_SetHostError( mmresult );
            goto error;
        }
    }

    if( outputDevice != paNoDevice )
    {
        int outputWinMmeId = LocalDeviceIndexToWinMmeDeviceId( winMmeHostApi, outputDevice );

        bytesPerOutputFrame = numOutputChannels * stream->bufferProcessor.bytesPerHostOutputSample;

        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = (WORD) numOutputChannels;
        wfx.nSamplesPerSec = (DWORD) sampleRate;
        wfx.nAvgBytesPerSec = (DWORD)(bytesPerOutputFrame * sampleRate);
        wfx.nBlockAlign = (WORD)bytesPerOutputFrame;
        wfx.wBitsPerSample = (WORD)((bytesPerOutputFrame/numOutputChannels) * 8);
        wfx.cbSize = 0;

        mmresult = waveOutOpen( &stream->hWaveOut, outputWinMmeId, &wfx,
                                (DWORD)stream->bufferEvent, (DWORD) stream, CALLBACK_EVENT );
        if( mmresult != MMSYSERR_NOERROR )
        {
            result = paHostError;
            PaUtil_SetHostError( mmresult );
            goto error;
        }
    }
    
    if( PA_IS_INPUT_STREAM_(stream) )
    {
        int hostInputBufferBytes = Pa_GetSampleSize( hostInputSampleFormat ) * framesPerHostInputBuffer * numInputChannels;
        if( hostInputBufferBytes < 0 )
        {
            result = paInternalError;
            goto error;
        }

        result = InitializeBufferSet( &stream->inputBuffers, numHostInputBuffers, hostInputBufferBytes,
                             (MmePrepareHeader*)waveInPrepareHeader, (MmePrepareHeader*)waveInUnprepareHeader, (MmeHandle)stream->hWaveIn );

        if( result != paNoError )
            goto error;

        stream->numInputBuffers = numHostInputBuffers;
    }

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        int hostOutputBufferBytes = Pa_GetSampleSize( hostOutputSampleFormat ) * framesPerHostOutputBuffer * numOutputChannels;
        if( hostOutputBufferBytes < 0 )
        {
            result = paInternalError;
            goto error;
        }

        result = InitializeBufferSet( &stream->outputBuffers, numHostOutputBuffers, hostOutputBufferBytes,
                             (MmePrepareHeader*)waveOutPrepareHeader, (MmePrepareHeader*)waveOutUnprepareHeader, (MmeHandle)stream->hWaveOut );

        if( result != paNoError )
            goto error;

        stream->numOutputBuffers = numHostOutputBuffers;
    }

    stream->abortEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
    if( stream->abortEvent == NULL )
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }
    abortEventInited = 1;

    InitializeCriticalSection( &stream->lock );
    lockInited = 1;

    if( PA_IS_OUTPUT_STREAM_(stream) )
        stream->allBuffersDurationMs = (DWORD) (1000.0 * (framesPerHostOutputBuffer * stream->numOutputBuffers) / sampleRate);
    else
        stream->allBuffersDurationMs = (DWORD) (1000.0 * (framesPerHostInputBuffer * stream->numInputBuffers) / sampleRate);


    *s = (PaStream*)stream;

    return result;

error:
    if( lockInited )
        DeleteCriticalSection( &stream->lock );

    if( abortEventInited )
        CloseHandle( stream->abortEvent );

    if( stream->hWaveOut )
    {
        if( stream->outputBuffers )
        {
            TerminateBufferSet( &stream->outputBuffers, stream->numOutputBuffers,
                    (MmePrepareHeader*)waveOutUnprepareHeader, (MmeHandle)stream->hWaveOut );
        }
    }

    if( stream->hWaveIn )
    {
        if( stream->inputBuffers )
        {
            TerminateBufferSet( &stream->inputBuffers, stream->numInputBuffers,
                    (MmePrepareHeader*)waveInUnprepareHeader, (MmeHandle)stream->hWaveIn );
        }
    }

    if( stream->hWaveOut )
        waveOutClose( stream->hWaveOut );

    if( stream->hWaveIn )
        waveInClose( stream->hWaveIn );

    if( bufferEventInited )
        CloseHandle( stream->bufferEvent );

    if( stream )
        PaUtil_FreeMemory( stream );

    return result;
}


static int CountQueuedOuputBuffers( PaWinMmeStream *stream )
{
    int result = 0;
    unsigned int i;

    if( stream->numOutputChannels > 0 )
    {
        for( i=0; i<stream->numOutputBuffers; ++i )
        {
            if( !( stream->outputBuffers[ i ].dwFlags & WHDR_DONE) )
            {
                result++;
            }
        }
    }

    return result;
}


static PaError AdvanceToNextInputBuffer( PaWinMmeStream *stream )
{
    PaError result = paNoError;
    MMRESULT mmresult;

    mmresult = waveInAddBuffer( stream->hWaveIn,
                                &stream->inputBuffers[ stream->currentInputBufferIndex ],
                                sizeof(WAVEHDR) );
    if( mmresult != MMSYSERR_NOERROR )
    {
        PaUtil_SetHostError( mmresult );
        result = paHostError;
    }
    stream->currentInputBufferIndex = (stream->currentInputBufferIndex+1 >= stream->numInputBuffers) ?
                                      0 : stream->currentInputBufferIndex+1;

    return result;
}


static PaError AdvanceToNextOutputBuffer( PaWinMmeStream *stream )
{
    PaError result = paNoError;
    MMRESULT mmresult;

    mmresult = waveOutWrite( stream->hWaveOut,
                             &stream->outputBuffers[ stream->currentOutputBufferIndex ],
                             sizeof(WAVEHDR) );
    if( mmresult != MMSYSERR_NOERROR )
    {
        PaUtil_SetHostError( mmresult );
        result = paHostError;
    }
    stream->currentOutputBufferIndex = (stream->currentOutputBufferIndex+1 >= stream->numOutputBuffers) ?
                                       0 : stream->currentOutputBufferIndex+1;

    return result;
}


static DWORD WINAPI ProcessingThreadProc( void *pArg )
{
    PaWinMmeStream *stream = (PaWinMmeStream *)pArg;
    HANDLE events[2];
    int numEvents = 0;
    DWORD result = paNoError;
    DWORD waitResult;
    DWORD timeout = stream->allBuffersDurationMs * 0.5;
    DWORD numTimeouts = 0;
    int hostBuffersAvailable;
    void *hostInputBuffer, *hostOutputBuffer;
    int callbackResult;
    int done = 0;
    
    /* prepare event array for call to WaitForMultipleObjects() */
    events[numEvents++] = stream->bufferEvent;
    events[numEvents++] = stream->abortEvent;

    /* loop until something causes us to stop */
    while( !done )
    {
        /* wait for MME to signal that a buffer is available, or for
            the PA abort event to be signaled */
        waitResult = WaitForMultipleObjects( numEvents, events, FALSE, timeout );
        if( waitResult == WAIT_FAILED )
        {
            PaUtil_SetHostError( GetLastError() );
            result = paHostError;
            done = 1;
        }
        else if( waitResult == WAIT_TIMEOUT )
        {
            /* if a timeout is encountered, continue */
            numTimeouts += 1;
        }

        if( stream->abortProcessing )
        {
            /* Pa_AbortStream() has been called, stop processing immediately */
            done = 1;
        }
        else if( stream->stopProcessing )
        {
            /* Pa_StopStream() has been called or the user callback returned
                non-zero, processing will continue until all output buffers
                are marked as done. The stream will stop immediately if it
                is input-only.
            */

            if( CountQueuedOuputBuffers( stream ) == 0 )
            {
                done = 1; /* Will cause thread to return. */
            }
        }
        else
        {
            hostBuffersAvailable = 1;

            /* process all available host buffers */
            do
            {
                hostInputBuffer = 0;
                hostOutputBuffer = 0;

                if( PA_IS_INPUT_STREAM_(stream) &&
                        stream->inputBuffers[ stream->currentInputBufferIndex ].dwFlags & WHDR_DONE )
                {
                    hostInputBuffer = stream->inputBuffers[ stream->currentInputBufferIndex ].lpData;
                }

                if( PA_IS_OUTPUT_STREAM_(stream) &&
                        stream->outputBuffers[ stream->currentOutputBufferIndex ].dwFlags & WHDR_DONE )
                {
                    hostOutputBuffer = stream->outputBuffers[ stream->currentOutputBufferIndex ].lpData;
                }

                if( (PA_IS_FULL_DUPLEX_STREAM_(stream) && hostInputBuffer && hostOutputBuffer) ||
                        (!PA_IS_FULL_DUPLEX_STREAM_(stream) && ( hostInputBuffer || hostOutputBuffer ) ) )
                {

                    /*
                    IMPLEMENT ME:
                        - generate timing information
                        - handle buffer slips
                    */
                    PaTimestamp outTime = 0; /* FIXME */

                    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer, stream->bufferProcessor.framesPerHostBuffer /*FIXME: this is a bit of a hack*/ );

                    callbackResult = PaUtil_ProcessInterleavedBuffers( &stream->bufferProcessor, hostInputBuffer, hostOutputBuffer, outTime );

                    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer );

                    if( callbackResult == paContinue )
                    {
                        /* nothing special to do */
                    }
                    else if( callbackResult == paAbort )
                    {
                        stream->abortProcessing = 1;
                        done = 1;
                        /* FIXME: should probably do a reset here */
                        result = paNoError;
                    }
                    else
                    {
                        /* User callback has asked us to stop with paComplete or other non-zero value */
                        stream->stopProcessing = 1; /* stop once currently queued audio has finished */
                        result = paNoError;
                    }

                    /*
                    FIXME: the following code is buggy, because stopProcessing should
                    still queue the current buffer.
                    */
                    if( stream->stopProcessing == 0 && stream->abortProcessing == 0 )
                    {
                        if( PA_IS_INPUT_STREAM_(stream) )
                        {
                            result = AdvanceToNextInputBuffer( stream );
                            if( result != paNoError )
                                done = 1;
                        }

                        if( PA_IS_OUTPUT_STREAM_(stream) )
                        {
                            result = AdvanceToNextOutputBuffer( stream );
                            if( result != paNoError )
                                done = 1;
                        }
                    }
                }
                else
                {
                    hostBuffersAvailable = 0;
                }
            }
            while( hostBuffersAvailable &&
                    stream->stopProcessing == 0 &&
                    stream->abortProcessing == 0 &&
                    !done );
        }

        result = UpdateStreamTime( stream );
        if( result != paNoError )
            done = 1;
    }

    stream->isActive = 0;
    
    return result;
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    MMRESULT mmresult;


    if( PA_IS_INPUT_STREAM_(stream) )
    {
        TerminateBufferSet( &stream->inputBuffers, stream->numInputBuffers,
                (MmePrepareHeader*)waveInUnprepareHeader, (MmeHandle)stream->hWaveIn );
    }
  
    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        TerminateBufferSet( &stream->outputBuffers, stream->numOutputBuffers,
                (MmePrepareHeader*)waveOutUnprepareHeader, (MmeHandle)stream->hWaveOut );
    }
  

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        mmresult = waveInClose( stream->hWaveIn );
        if( mmresult != MMSYSERR_NOERROR )
        {
            result = paHostError;
            PaUtil_SetHostError( mmresult );
            goto error;
        }
    }

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        mmresult = waveOutClose( stream->hWaveOut );
        if( mmresult != MMSYSERR_NOERROR )
        {
            result = paHostError;
            PaUtil_SetHostError( mmresult );
            goto error;
        }
    }

    if( CloseHandle( stream->bufferEvent ) == 0 )
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }

    if( CloseHandle( stream->abortEvent ) == 0 )
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }

    DeleteCriticalSection( &stream->lock );

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

error:
    /* FIXME: consider how to best clean up on failure */
    return result;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    MMRESULT mmresult;
    unsigned int i;

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        for( i=0; i<stream->numInputBuffers; ++i )
        {
            mmresult = waveInAddBuffer( stream->hWaveIn, &stream->inputBuffers[i], sizeof(WAVEHDR) );
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paHostError;
                PaUtil_SetHostError( mmresult );
                goto error;
            }
        }
        stream->currentInputBufferIndex = 0;
    }

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        if( (mmresult = waveOutPause( stream->hWaveOut )) != MMSYSERR_NOERROR )
        {
            result = paHostError;
            PaUtil_SetHostError( mmresult );
            goto error;
        }

        for( i=0; i<stream->numOutputBuffers; ++i )
        {
            ZeroMemory( stream->outputBuffers[i].lpData, stream->outputBuffers[i].dwBufferLength );
            mmresult = waveOutWrite( stream->hWaveOut, &stream->outputBuffers[i], sizeof(WAVEHDR) );
            if( mmresult != MMSYSERR_NOERROR )
            {
                result = paHostError;
                PaUtil_SetHostError( mmresult );
                goto error;
            }
            /* stream->past_FrameCount += wmmeStreamData->framesPerHostBuffer; <-- REVIEW: why was this here? */
        }
        stream->currentOutputBufferIndex = 0;
    }

    stream->streamPosition = 0.;
    stream->previousStreamPosition = 0;

    stream->isActive = 1;
    stream->stopProcessing = 0;
    stream->abortProcessing = 0;

    if( ResetEvent( stream->bufferEvent ) == 0 )
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }

    if( ResetEvent( stream->abortEvent ) == 0 )
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }

    /* Create thread that waits for audio buffers to be ready for processing. */
    stream->processingThread = CreateThread( 0, 0, ProcessingThreadProc, stream, 0, &stream->processingThreadId );
    if( !stream->processingThread )
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }

    /* I used to pass the thread which was failing. I now pass GetCurrentProcess().
     * This fix could improve latency for some applications. It could also result in CPU
     * starvation if the callback did too much processing.
     * I also added result checks, so we might see more failures at initialization.
     * Thanks to Alberto di Bene for spotting this.
     */
    /* REVIEW: should we reset the priority class when the stream has stopped?
        - would be best to refcount priority boosts incase more than one
        stream is open
    */


#if 0
FIXME this code defed out while debugging only

    if( !SetPriorityClass( GetCurrentProcess(), HIGH_PRIORITY_CLASS ) ) /* PLB20010816 */
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }

    /* FIXME: could go TIME_CRITICAL with mme-specific flag */
    if( !SetThreadPriority( stream->processingThread, THREAD_PRIORITY_HIGHEST ) )
    {
        result = paHostError;
        PaUtil_SetHostError( GetLastError() );
        goto error;
    }
#endif

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        mmresult = waveInStart( stream->hWaveIn );
        PA_DEBUG(("Pa_StartStream: waveInStart returned = 0x%X.\n", mmresult));
        if( mmresult != MMSYSERR_NOERROR )
        {
            result = paHostError;
            PaUtil_SetHostError( mmresult );
            goto error;
        }
    }

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        if( (mmresult = waveOutRestart( stream->hWaveOut )) != MMSYSERR_NOERROR )
        {
            result = paHostError;
            PaUtil_SetHostError( mmresult );
            goto error;
        }
    }

    return result;

error:
    /* FIXME: implement recovery as best we can
    This should involve rolling back to a state as-if this function had never been called
    */
    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    int timeout;
    DWORD waitResult;
    MMRESULT mmresult;

    /*
        FIXME: the error checking in this function needs review. the basic
        idea is to return from this function in a known state - for example
        there is no point avoiding calling waveInReset just because
        the thread times out.
    */


    /* Tell processing thread to stop generating more data and to let current data play out. */
    stream->stopProcessing = 1;

    /* Calculate timeOut longer than longest time it could take to return all buffers. */
    timeout = stream->allBuffersDurationMs * 1.5;
    if( timeout < PA_MIN_TIMEOUT_MSEC_ )
        timeout = PA_MIN_TIMEOUT_MSEC_;

    PA_DEBUG(("WinMME StopStream: waiting for background thread.\n"));

    waitResult = WaitForSingleObject( stream->processingThread, timeout );
    if( waitResult == WAIT_TIMEOUT )
    {
        /* try to abort */
        stream->abortProcessing = 1;
        SetEvent( stream->abortEvent );
        waitResult = WaitForSingleObject( stream->processingThread, timeout );
        if( waitResult == WAIT_TIMEOUT )
        {
            PA_DEBUG(("WinMME StopStream: timed out while waiting for background thread to finish.\n"));
            result = paTimedOut;
        }
    }

    CloseHandle( stream->processingThread );
    stream->processingThread = NULL;

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        mmresult = waveOutReset( stream->hWaveOut );
        if( mmresult != MMSYSERR_NOERROR )
        {
            PaUtil_SetHostError( mmresult );
            result = paHostError;
        }
    }

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        mmresult = waveInReset( stream->hWaveIn );
        if( mmresult != MMSYSERR_NOERROR )
        {
            PaUtil_SetHostError( mmresult );
            result = paHostError;
        }
    }

    stream->isActive = 0;

    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    int timeout;
    DWORD waitResult;
    MMRESULT mmresult;

    /*
        FIXME: the error checking in this function needs review. the basic
        idea is to return from this function in a known state - for example
        there is no point avoiding calling waveInReset just because
        the thread times out.
    */

    /* Tell processing thread to abort immediately */
    stream->abortProcessing = 1;
    SetEvent( stream->abortEvent );

    /* Calculate timeOut longer than longest time it could take to return all buffers. */
    timeout = stream->allBuffersDurationMs * 1.5;
    if( timeout < PA_MIN_TIMEOUT_MSEC_ )
        timeout = PA_MIN_TIMEOUT_MSEC_;

    if( PA_IS_OUTPUT_STREAM_(stream) )
    {
        mmresult = waveOutReset( stream->hWaveOut );
        if( mmresult != MMSYSERR_NOERROR )
        {
            PaUtil_SetHostError( mmresult );
            return paHostError;
        }
    }

    if( PA_IS_INPUT_STREAM_(stream) )
    {
        mmresult = waveInReset( stream->hWaveIn );
        if( mmresult != MMSYSERR_NOERROR )
        {
            PaUtil_SetHostError( mmresult );
            return paHostError;
        }
    }


    PA_DEBUG(("WinMME AbortStream: waiting for background thread.\n"));

    waitResult = WaitForSingleObject( stream->processingThread, timeout );
    if( waitResult == WAIT_TIMEOUT )
    {
        PA_DEBUG(("WinMME AbortStream: timed out while waiting for background thread to finish.\n"));
        return paTimedOut;
    }

    CloseHandle( stream->processingThread );
    stream->processingThread = NULL;

    stream->isActive = 0;

    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    return ( stream->processingThread != NULL );

    return 0;
}


static PaError IsStreamActive( PaStream *s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    return stream->isActive;
}


/*  UpdateStreamTime() must be called periodically because mmtime.u.sample
    is a DWORD and can wrap and lose sync after a few hours.
 */
static PaError UpdateStreamTime( PaWinMmeStream *stream )
{
    MMRESULT  mmresult;
    MMTIME    mmtime;
    mmtime.wType = TIME_SAMPLES;

    if( stream->hWaveOut != NULL )
    {
        mmresult = waveOutGetPosition( stream->hWaveOut, &mmtime, sizeof(mmtime) );
    }
    else
    {
        mmresult = waveInGetPosition( stream->hWaveIn, &mmtime, sizeof(mmtime) );
    }

    if( mmresult != MMSYSERR_NOERROR )
    {
        PaUtil_SetHostError( mmresult );
        return paHostError;
    }

    /* This data has two variables and is shared by foreground and background.
     * So we need to make it thread safe. */
    EnterCriticalSection( &stream->lock );
    stream->streamPosition += ((long)mmtime.u.sample) - stream->previousStreamPosition;
    stream->previousStreamPosition = (long)mmtime.u.sample;
    LeaveCriticalSection( &stream->lock );

    return paNoError;
}


static PaTimestamp GetStreamTime( PaStream *s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;
    PaError error = UpdateStreamTime( stream );

    if( error == paNoError )
        return stream->streamPosition;
    else
        return 0;
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

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
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            void *buffer,
                            unsigned long frames )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static unsigned long GetStreamReadAvailable( PaStream* s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static unsigned long GetStreamWriteAvailable( PaStream* s )
{
    PaWinMmeStream *stream = (PaWinMmeStream*)s;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}

