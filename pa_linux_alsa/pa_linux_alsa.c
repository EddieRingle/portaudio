/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 * ALSA implementation by Joshua Haberman
 *
 * Copyright (c) 2002 Joshua Haberman <joshua@haberman.com>
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

#include <sys/poll.h>
#include <pthread.h>

#include <string.h> /* strlen() */
#include <limits.h>

#include <alsa/asoundlib.h>

#include "portaudio.h"
#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_linux_alsa.h"

/* PaAlsaHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation commonHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    PaHostApiIndex hostApiIndex;
}
PaAlsaHostApiRepresentation;


/* prototypes for functions declared in this file */

PaError PaAlsa_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex );
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *callback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError BuildDeviceList( PaAlsaHostApiRepresentation *hostApi );

/* blocking calls are in blocking_calls.c */
extern PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
extern PaError WriteStream( PaStream* stream, void *buffer, unsigned long frames );
extern signed long GetStreamReadAvailable( PaStream* stream );
extern signed long GetStreamWriteAvailable( PaStream* stream );

/* all callback-related functions are in callback_thread.c */
extern void *CallbackThread( void *userData );


PaError PaAlsa_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaAlsaHostApiRepresentation *skeletonHostApi;

    skeletonHostApi = (PaAlsaHostApiRepresentation*)
        PaUtil_AllocateMemory( sizeof(PaAlsaHostApiRepresentation) );
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

    skeletonHostApi->hostApiIndex = hostApiIndex;
    *hostApi = (PaUtilHostApiRepresentation*)skeletonHostApi;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paALSA;
    (*hostApi)->info.name = "ALSA implementation";

    BuildDeviceList( skeletonHostApi );

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;

    PaUtil_InitializeStreamInterface( &skeletonHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite,
                                      PaUtil_DummyGetAvailable,
                                      PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &skeletonHostApi->blockingStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream,
                                      GetStreamReadAvailable,
                                      GetStreamWriteAvailable );

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

static PaError BuildDeviceList( PaAlsaHostApiRepresentation *alsaApi )
{
    PaUtilHostApiRepresentation *commonApi = &alsaApi->commonHostApiRep;
    PaDeviceInfo *deviceInfoArray;
    int deviceCount = 0;
    int card_idx;
    int device_idx;
    snd_ctl_t *ctl;
    snd_ctl_card_info_t *card_info;

    /* count the devices by enumerating all the card numbers */

    /* snd_card_next() modifies the integer passed to it to be:
     *      the index of the first card if the parameter is -1
     *      the index of the next card if the parameter is the index of a card
     *      -1 if there are no more cards
     *
     * The function itself returns 0 if it succeeded. */
    card_idx = -1;
    while( snd_card_next( &card_idx ) == 0 && card_idx >= 0 )
    {
        deviceCount++;
    }

    /* allocate deviceInfo memory based on the number of devices */

    commonApi->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaDeviceInfo*) * deviceCount );
    if( !commonApi->deviceInfos )
    {
        return paInsufficientMemory;
    }

    /* allocate all device info structs in a contiguous block */
    deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaDeviceInfo) * deviceCount );
    if( !deviceInfoArray )
    {
        return paInsufficientMemory;
    }

    /* now loop over the list of devices again, filling in the deviceInfo for each */
    card_idx = -1;
    device_idx = 0;
    while( snd_card_next( &card_idx ) == 0 && card_idx >= 0 )
    {
        PaDeviceInfo *deviceInfo = &deviceInfoArray[device_idx];
        char *deviceName;
        char alsaDeviceName[50];
        const char *cardName;

        commonApi->deviceInfos[device_idx++] = deviceInfo;

        deviceInfo->structVersion = 2;
        deviceInfo->hostApi = alsaApi->hostApiIndex;

        sprintf( alsaDeviceName, "hw:%d", card_idx );
        snd_ctl_open( &ctl, alsaDeviceName, 0 );
        snd_ctl_card_info_malloc( &card_info );
        snd_ctl_card_info( ctl, card_info );
        cardName = snd_ctl_card_info_get_id( card_info );

        deviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                        strlen(cardName) + 1 );
        if( !deviceName )
        {
            return paInsufficientMemory;
        }
        strcpy( deviceName, cardName );
        deviceInfo->name = deviceName;

        snd_ctl_card_info_free( card_info );

        /* to determine max. channels, we must open the device and query the
         * hardware parameter configuration space */
        {
            snd_pcm_t *pcm_handle;
            snd_pcm_hw_params_t *hw_params;
            int dir;

            snd_pcm_hw_params_malloc( &hw_params );

            /* get max channels for capture */

            if( snd_pcm_open( &pcm_handle, alsaDeviceName, SND_PCM_STREAM_CAPTURE, 0 ) < 0 )
            {
                deviceInfo->maxInputChannels = 0;
            }
            else
            {
                snd_pcm_hw_params_any( pcm_handle, hw_params );
                deviceInfo->maxInputChannels = snd_pcm_hw_params_get_channels_max( hw_params );
                /* TODO: I'm not really sure what to do here */
                //deviceInfo->defaultLowInputLatency = snd_pcm_hw_params_get_period_size_min( hw_params, &dir );
                //deviceInfo->defaultHighInputLatency = snd_pcm_hw_params_get_period_size_max( hw_params, &dir );
                deviceInfo->defaultLowInputLatency = 128. / 44100;
                deviceInfo->defaultHighInputLatency = 16384. / 44100;
                snd_pcm_close( pcm_handle );
            }

            /* get max channels for playback */
            if( snd_pcm_open( &pcm_handle, alsaDeviceName, SND_PCM_STREAM_PLAYBACK, 0 ) < 0 )
            {
                deviceInfo->maxOutputChannels = 0;
            }
            else
            {
                snd_pcm_hw_params_any( pcm_handle, hw_params );
                deviceInfo->maxOutputChannels = snd_pcm_hw_params_get_channels_max( hw_params );
                /* TODO: I'm not really sure what to do here */
                //deviceInfo->defaultLowOutputLatency = snd_pcm_hw_params_get_period_size_min( hw_params, &dir );
                //deviceInfo->defaultHighOutputLatency = snd_pcm_hw_params_get_period_size_max( hw_params, &dir );
                deviceInfo->defaultLowOutputLatency = 128. / 44100;
                deviceInfo->defaultHighOutputLatency = 16384. / 44100;
                snd_pcm_close( pcm_handle );
            }

            snd_pcm_hw_params_free( hw_params );
        }

        deviceInfo->defaultSampleRate = 44100.; /* IMPLEMENT ME */
    }

    commonApi->info.deviceCount = deviceCount;
    commonApi->info.defaultInputDevice = 0;
    commonApi->info.defaultOutputDevice = 0;

    return paNoError;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaAlsaHostApiRepresentation *skeletonHostApi;
    skeletonHostApi = (PaAlsaHostApiRepresentation*)hostApi;

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


/* Given an open stream, what sample formats are available? */

static PaSampleFormat GetAvailableFormats( snd_pcm_t *stream )
{
    PaSampleFormat available = 0;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca( &hw_params );

    snd_pcm_hw_params_any( stream, hw_params );

    if( snd_pcm_hw_params_test_format( stream, hw_params, SND_PCM_FORMAT_FLOAT ) == 0)
        available |= paFloat32;

    if( snd_pcm_hw_params_test_format( stream, hw_params, SND_PCM_FORMAT_S16 ) == 0)
        available |= paInt16;

    if( snd_pcm_hw_params_test_format( stream, hw_params, SND_PCM_FORMAT_S24 ) == 0)
        available |= paInt24;

    if( snd_pcm_hw_params_test_format( stream, hw_params, SND_PCM_FORMAT_S32 ) == 0)
        available |= paInt32;

    if( snd_pcm_hw_params_test_format( stream, hw_params, SND_PCM_FORMAT_S8 ) == 0)
        available |= paInt8;

    if( snd_pcm_hw_params_test_format( stream, hw_params, SND_PCM_FORMAT_U8 ) == 0)
        available |= paUInt8;

    return available;
}

/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

static PaError ConfigureStream( snd_pcm_t *stream, int channels,
                                int interleaved, unsigned long rate,
                                PaSampleFormat pa_format, int framesPerBuffer )
{
#define ENSURE(functioncall)   \
    if( (functioncall) < 0 ) { \
        printf("Error executing ALSA call, line %d\n", __LINE__); \
        return 1; \
    } \
    else { \
        printf("ALSA call at line %d succeeded\n", __LINE__ ); \
    }

    snd_pcm_access_t access_mode;
    snd_pcm_format_t alsa_format;

    /* configuration consists of setting all of ALSA's parameters.
     * These parameters come in two flavors: hardware parameters
     * and software paramters.  Hardware parameters will affect
     * the way the device is initialized, software parameters
     * affect the way ALSA interacts with me, the user-level client. */

    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;

    snd_pcm_hw_params_alloca( &hw_params );

    /* ... fill up the configuration space with all possibile
     * combinations of parameters this device will accept */
    ENSURE( snd_pcm_hw_params_any( stream, hw_params ) );

    if( interleaved )
        access_mode = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    else
        access_mode = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;

    ENSURE( snd_pcm_hw_params_set_access( stream, hw_params, access_mode ) );

    /* set the format based on what the user selected */
    switch( pa_format )
    {
        case paFloat32:
            alsa_format = SND_PCM_FORMAT_FLOAT;
            break;

        case paInt16:
            alsa_format = SND_PCM_FORMAT_S16;
            break;

        case paInt24:
            alsa_format = SND_PCM_FORMAT_S24;
            break;

        case paInt32:
            alsa_format = SND_PCM_FORMAT_S32;
            break;

        case paInt8:
            alsa_format = SND_PCM_FORMAT_S8;
            break;

        case paUInt8:
            alsa_format = SND_PCM_FORMAT_U8;
            break;

        default:
            printf("Unknown PortAudio format %d\n", pa_format );
            return 1;
    }
    //printf("PortAudio format: %d\n", pa_format);
    printf("ALSA format: %d\n", alsa_format);
    ENSURE( snd_pcm_hw_params_set_format( stream, hw_params, alsa_format ) );

    /* ... set the sample rate */
    ENSURE( snd_pcm_hw_params_set_rate( stream, hw_params, rate, 0 ) );

    /* ... set the number of channels */
    ENSURE( snd_pcm_hw_params_set_channels( stream, hw_params, channels ) );

    /* ... set the number of periods to 2, which is essentially double buffering.
     * this makes the latency the number of samples per buffer, which is the best
     * it can be */
    ENSURE( snd_pcm_hw_params_set_periods ( stream, hw_params, 2, 0 ) );

    /* ... set the period size, which is essentially the hardware buffer size */
    if( framesPerBuffer != 0 )
    {
        ENSURE( snd_pcm_hw_params_set_period_size( stream, hw_params, 
                                                   framesPerBuffer, 0 ) );
    }
    else
    {
        ENSURE( snd_pcm_hw_params_set_period_size( stream, hw_params, 
                                                   2048, 0 ) );
    }


    /* Set the parameters! */
    ENSURE( snd_pcm_hw_params( stream, hw_params ) );

    return 0;
#undef ENSURE
}

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *callback,
                           void *userData )
{
    PaError result = paNoError;
    PaAlsaHostApiRepresentation *skeletonHostApi =
        (PaAlsaHostApiRepresentation*)hostApi;
    PaAlsaStream *stream = 0;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
    int numInputChannels, numOutputChannels;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    unsigned long framesPerHostBuffer = framesPerBuffer;

    if( framesPerHostBuffer == paFramesPerBufferUnspecified )
    {
        // TODO: have some reason
        framesPerHostBuffer = 2048;
    }

    if( inputParameters )
    {
        numInputChannels = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification.
            [JH] this could be supported in the future, to allow ALSA device strings
                 like hw:0 */
        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that input device can support numInputChannels */
        if( numInputChannels > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
    }
    else
    {
        numInputChannels = 0;
    }

    if( outputParameters )
    {
        numOutputChannels = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification
            [JH] this could be supported in the future, to allow ALSA device strings
                 like hw:0 */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support numInputChannels */
        if( numOutputChannels > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
    }
    else
    {
        numOutputChannels = 0;
    }

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */

    /* allocate and do basic initialization of the stream structure */

    stream = (PaAlsaStream*)PaUtil_AllocateMemory( sizeof(PaAlsaStream) );
    if( !stream )
    {
        printf("memory point 2\n");
        result = paInsufficientMemory;
        goto error;
    }

    stream->pcm_capture = NULL;
    stream->pcm_playback = NULL;
    stream->callback_mode = (callback != 0);
    stream->callback_finished = 0;

    if( callback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &skeletonHostApi->callbackStreamInterface,
                                               callback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &skeletonHostApi->blockingStreamInterface,
                                               callback, userData );
    }


    stream->streamRepresentation.streamInfo.inputLatency = framesPerHostBuffer;
    stream->streamRepresentation.streamInfo.outputLatency = framesPerHostBuffer;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    /* open the devices now, so we can obtain info about the available formats */

    if( numInputChannels > 0 )
    {
        char inputDeviceName[50];

        sprintf( inputDeviceName, "hw:CARD=%s", hostApi->deviceInfos[inputParameters->device]->name );
        if( snd_pcm_open( &stream->pcm_capture, inputDeviceName, SND_PCM_STREAM_CAPTURE, 0 ) < 0 )
        {
            result = paBadIODeviceCombination;
            goto error;
        }
        hostInputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats(stream->pcm_capture),
                                                 inputSampleFormat );
    }

    if( numOutputChannels > 0 )
    {
        char outputDeviceName[50];

        sprintf( outputDeviceName, "hw:CARD=%s", hostApi->deviceInfos[outputParameters->device]->name );
        if( snd_pcm_open( &stream->pcm_playback, outputDeviceName, SND_PCM_STREAM_PLAYBACK, 0 ) < 0 )
        {
            result = paBadIODeviceCombination;
            goto error;
        }
        hostOutputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats(stream->pcm_playback),
                                                 outputSampleFormat );
        stream->playback_hostsampleformat = hostOutputSampleFormat;
    }



    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              numInputChannels, inputSampleFormat, hostInputSampleFormat,
              numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerBuffer, framesPerHostBuffer,
              paUtilFixedHostBufferSize, callback, userData );
    if( result != paNoError )
        goto error;

    /* configure the streams */

    if( numInputChannels > 0 )
    {
        int interleaved;
        PaSampleFormat plain_format = hostInputSampleFormat & ~paNonInterleaved;

        if( inputSampleFormat & paNonInterleaved )
            interleaved = 0;
        else
            interleaved = 1;

        if( ConfigureStream( stream->pcm_capture, numInputChannels, interleaved,
                             sampleRate, plain_format, framesPerHostBuffer ) != 0 )
        {
            result = paBadIODeviceCombination;
            goto error;
        }

        stream->capture_interleaved = interleaved;
    }

    if( numOutputChannels > 0 )
    {
        int interleaved;
        PaSampleFormat plain_format = hostOutputSampleFormat & ~paNonInterleaved;

        if( outputSampleFormat & paNonInterleaved )
            interleaved = 0;
        else
            interleaved = 1;

        if( ConfigureStream( stream->pcm_playback, numOutputChannels, interleaved,
                             sampleRate, plain_format, framesPerHostBuffer ) != 0 )
        {
            result = paBadIODeviceCombination;
            goto error;
        }

        stream->playback_interleaved = interleaved;
    }

    stream->capture_nfds = 0;
    stream->playback_nfds = 0;

    if( stream->pcm_capture )
        stream->capture_nfds = snd_pcm_poll_descriptors_count( stream->pcm_capture );

    if( stream->pcm_playback )
        stream->playback_nfds = snd_pcm_poll_descriptors_count( stream->pcm_playback );

    /* TODO: free this properly */
    printf("trying to allocate %d bytes of memory\n", (stream->capture_nfds + stream->playback_nfds + 1) * sizeof(struct pollfd) );
    stream->pfds = (struct pollfd*)PaUtil_AllocateMemory( (stream->capture_nfds +
                                                           stream->playback_nfds + 1) *
                                                           sizeof(struct pollfd) );
    if( !stream->pfds )
    {
        printf("bad memory point 1\n");
        result = paInsufficientMemory;
        goto error;
    }

    stream->frames_per_period = framesPerHostBuffer;
    stream->capture_channels = numInputChannels;
    stream->playback_channels = numOutputChannels;

    *s = (PaStream*)stream;

    return result;

error:
    if( stream )
        PaUtil_FreeMemory( stream );

    return result;
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    if( stream->pcm_capture )
    {
        snd_pcm_close( stream->pcm_capture );
    }

    if( stream->pcm_playback )
    {
        snd_pcm_close( stream->pcm_playback );
    }

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

    return result;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* TODO: support errorText */
#define ENSURE(x) \
    { \
        int error_ret; \
        error_ret = (x); \
        if( error_ret != 0 ) { \
            PaHostErrorInfo err; \
            err.errorCode = error_ret; \
            err.hostApiType = paALSA; \
            printf("call at %d failed\n", __LINE__); \
            return paUnanticipatedHostError; \
        } \
        else \
            printf("call at line %d succeeded\n", __LINE__); \
    }

    if( stream->pcm_capture )
    {
        ENSURE( snd_pcm_prepare( stream->pcm_capture ) );
    }

    if( stream->pcm_playback )
    {
        const snd_pcm_channel_area_t *playback_areas, *area;
        snd_pcm_uframes_t offset, frames;
        int sample_size = Pa_GetSampleSize( stream->playback_hostsampleformat );
        printf("Sample size: %d\n", sample_size );
        ENSURE( snd_pcm_prepare( stream->pcm_playback ) );
        frames = snd_pcm_avail_update( stream->pcm_playback );
        printf("frames: %d\n", frames );
        printf("channels: %d\n", stream->playback_channels );

        snd_pcm_mmap_begin( stream->pcm_playback, &playback_areas, &offset, &frames );

        /* Insert silence */
        if( stream->playback_interleaved )
        {
            void *playback_buffer;
            area = &playback_areas[0];
            playback_buffer = area->addr + (area->first + area->step * offset) / 8;
            memset( playback_buffer, 0,
                    frames * stream->playback_channels * sample_size );
        }
        else
        {
            int i;
            for( i = 0; i < stream->playback_channels; i++ )
            {
                void *channel_buffer;
                area = &playback_areas[i];
                channel_buffer = area->addr + (area->first + area->step * offset) / 8;
                memset( channel_buffer, 0, frames * sample_size );
            }
        }

        snd_pcm_mmap_commit( stream->pcm_playback, offset, frames );
    }

    if( stream->callback_mode )
    {
        ENSURE( pthread_create( &stream->callback_thread, NULL, &CallbackThread, stream ) );

        /* we'll do the snd_pcm_start() in the callback thread */
    }
    else
    {
        if( stream->pcm_capture )
            snd_pcm_start( stream->pcm_capture );
        if( stream->pcm_playback )
            snd_pcm_start( stream->pcm_playback );
    }

    /* On my machine, the pcm stream will not transition to the RUNNING
     * state for a while after snd_pcm_start is called.  The PortAudio
     * client needs to be able to depend on Pa_IsStreamActive() returning
     * true the second after this function returns.  So I sleep briefly here.
     *
     * I don't like this one bit.
     */
    Pa_Sleep( 100 );

    stream->callback_finished = 0;

    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* First deal with the callback thread, cancelling and/or joining
     * it if necessary
     */

    if( stream->callback_mode && stream->callback_finished )
    {
        /* We are running in callback mode but the callback thread has
         * already been cancelled by the return value from the user's
         * callback function.  Therefore we don't need to cancel the
         * thread, but we do want to wait for it. */
        pthread_join( stream->callback_thread, NULL );
    }
    else if( stream->callback_mode )
    {
        /* We are running in callback mode, and the callback thread
         * is still running.  Cancel it and wait for it to be done. */
        pthread_cancel( stream->callback_thread );
        pthread_join( stream->callback_thread, NULL );
    }

    /* Stop the ALSA streams if necessary */

    if( stream->callback_mode && stream->callback_finished )
    {
        /* If we are in the callback_finished state the callback thread
         * already stopped the streams.  So there is nothing to do here.
         */
    }
    else
    {
        if( stream->pcm_capture )
        {
            snd_pcm_drain( stream->pcm_capture );
        }

        if( stream->pcm_playback )
        {
            snd_pcm_drain( stream->pcm_playback );
        }
    }

    stream->callback_finished = 0;

    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* First deal with the callback thread, cancelling and/or joining
     * it if necessary
     */

    if( stream->callback_mode && stream->callback_finished )
    {
        /* We are running in callback mode but the callback thread has
         * already been cancelled by the return value from the user's
         * callback function.  Therefore we don't need to cancel the
         * thread, but we do want to wait for it. */
        pthread_join( stream->callback_thread, NULL );
    }
    else if( stream->callback_mode )
    {
        /* We are running in callback mode, and the callback thread
         * is still running.  Cancel it and wait for it to be done. */
        pthread_cancel( stream->callback_thread );
        pthread_join( stream->callback_thread, NULL );
    }

    /* Stop the ALSA streams if necessary */

    if( stream->callback_mode && stream->callback_finished )
    {
        /* If we are in the callback_finished state the callback thread
         * already stopped the streams.  So there is nothing to do here.
         */
    }
    else
    {
        if( stream->pcm_capture )
        {
            snd_pcm_drop( stream->pcm_capture );
        }

        if( stream->pcm_playback )
        {
            snd_pcm_drop( stream->pcm_playback );
        }
    }

    stream->callback_finished = 0;

    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    if( IsStreamActive(s) || stream->callback_finished )
        return 0;
    else
        return 1;
}


static PaError IsStreamActive( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    if( stream->pcm_capture )
    {
        snd_pcm_state_t capture_state = snd_pcm_state( stream->pcm_capture );

        if( capture_state == SND_PCM_STATE_RUNNING /*||
            capture_state == SND_PCM_STATE_PREPARED*/ )
            return 1;
    }

    if( stream->pcm_playback )
    {
        snd_pcm_state_t playback_state = snd_pcm_state( stream->pcm_playback );

        if( playback_state == SND_PCM_STATE_RUNNING /*||
            playback_state == SND_PCM_STATE_PREPARED*/ )
            return 1;
    }

    return 0;
}


static PaTime GetStreamTime( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    snd_output_t *output;
    snd_timestamp_t timestamp;
    snd_pcm_status_t *status;
    snd_pcm_status_alloca( &status );

    /* TODO: what if we have both?  does it really matter? */

    /* TODO: if running in callback mode, this will mean
     * libasound routines are being called form multiple threads.
     * need to verify that libasound is thread-safe. */

    if( stream->pcm_capture )
    {
        snd_pcm_status( stream->pcm_capture, status );
    }
    else if( stream->pcm_playback )
    {
        snd_pcm_status( stream->pcm_playback, status );
    }

    snd_pcm_status_get_tstamp( status, &timestamp );

    return timestamp.tv_sec + ((float)timestamp.tv_usec/1000000);
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}

