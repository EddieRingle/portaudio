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
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate );
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
void InitializeStream( PaAlsaStream *stream, int callback, PaStreamFlags streamFlags );
void CleanUpStream( PaAlsaStream *stream );

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
    PaAlsaHostApiRepresentation *alsaHostApi;

    alsaHostApi = (PaAlsaHostApiRepresentation*)
        PaUtil_AllocateMemory( sizeof(PaAlsaHostApiRepresentation) );
    if( !alsaHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    alsaHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !alsaHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    alsaHostApi->hostApiIndex = hostApiIndex;
    *hostApi = (PaUtilHostApiRepresentation*)alsaHostApi;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paALSA;
    (*hostApi)->info.name = "ALSA implementation";

    BuildDeviceList( alsaHostApi );

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &alsaHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyReadWrite, PaUtil_DummyReadWrite,
                                      PaUtil_DummyGetAvailable,
                                      PaUtil_DummyGetAvailable );

    PaUtil_InitializeStreamInterface( &alsaHostApi->blockingStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream,
                                      GetStreamReadAvailable,
                                      GetStreamWriteAvailable );

    return result;

error:
    if( alsaHostApi )
    {
        if( alsaHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( alsaHostApi->allocations );
            PaUtil_DestroyAllocationGroup( alsaHostApi->allocations );
        }

        PaUtil_FreeMemory( alsaHostApi );
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
                snd_pcm_close( pcm_handle );

                /* TWEAKME:
                 *
                 * Giving values for default min and max latency is not
                 * straightforward.  Here are our objectives:
                 *
                 *         * for low latency, we want to give the lowest value
                 *         that will work reliably.  This varies based on the
                 *         sound card, kernel, CPU, etc.  I think it is better
                 *         to give sub-optimal latency than to give a number
                 *         too low and cause dropouts.  My conservative
                 *         estimate at this point is to base it on 4096-sample
                 *         latency at 44.1 kHz, which gives a latency of 23ms.
                 *         * for high latency we want to give a large enough
                 *         value that dropouts are basically impossible.  This
                 *         doesn't really require as much tweaking, since
                 *         providing too large a number will just cause us to
                 *         select the nearest setting that will work at stream
                 *         config time.
                 */
                deviceInfo->defaultLowInputLatency = 4096. / 44100;
                deviceInfo->defaultHighInputLatency = 16384. / 44100;
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
                snd_pcm_close( pcm_handle );

                /* TWEAKME: see above */
                deviceInfo->defaultLowOutputLatency = 4096. / 44100;
                deviceInfo->defaultHighOutputLatency = 16384. / 44100;
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
    PaAlsaHostApiRepresentation *alsaHostApi;
    alsaHostApi = (PaAlsaHostApiRepresentation*)hostApi;

    /*
        IMPLEMENT ME:
            - clean up any resourced not handled by the allocation group
    */

    if( alsaHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( alsaHostApi->allocations );
        PaUtil_DestroyAllocationGroup( alsaHostApi->allocations );
    }

    PaUtil_FreeMemory( alsaHostApi );
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;

    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
    }
    else
    {
        inputChannelCount = 0;
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support inputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
    }
    else
    {
        outputChannelCount = 0;
    }

    /*
        IMPLEMENT ME:
            - check that input device can support inputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - check that output device can support outputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported

            - check that the device supports sampleRate
    */

    return paFormatIsSupported;
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
                                PaSampleFormat pa_format, int framesPerBuffer,
                                float latency)
{
#define ENSURE(functioncall)   \
    if( (functioncall) < 0 ) { \
        PA_DEBUG(("Error executing ALSA call, line %d\n", __LINE__)); \
        return 1; \
    } \
    else { \
        PA_DEBUG(("ALSA call at line %d succeeded\n", __LINE__ )); \
    }

    snd_pcm_access_t access_mode;
    snd_pcm_format_t alsa_format;
    int numPeriods;

    if( getenv("PA_NUMPERIODS") != NULL )
        numPeriods = atoi( getenv("PA_NUMPERIODS") );
    else
        numPeriods = ( (latency*rate) / framesPerBuffer ) + 1;

    PA_DEBUG(("latency: %f, rate: %ld, framesPerBuffer: %d\n", latency, rate, framesPerBuffer));
    if( numPeriods <= 1 )
        numPeriods = 2;

    /* configuration consists of setting all of ALSA's parameters.
     * These parameters come in two flavors: hardware parameters
     * and software paramters.  Hardware parameters will affect
     * the way the device is initialized, software parameters
     * affect the way ALSA interacts with me, the user-level client. */

    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;

    snd_pcm_hw_params_alloca( &hw_params );
    snd_pcm_sw_params_alloca( &sw_params );

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
            PA_DEBUG(("Unknown PortAudio format %ld\n", pa_format ));
            return 1;
    }
    //PA_DEBUG(("PortAudio format: %d\n", pa_format));
    PA_DEBUG(("ALSA format: %d\n", alsa_format));
    ENSURE( snd_pcm_hw_params_set_format( stream, hw_params, alsa_format ) );

    /* ... set the sample rate */
    ENSURE( snd_pcm_hw_params_set_rate( stream, hw_params, rate, 0 ) );

    /* ... set the number of channels */
    ENSURE( snd_pcm_hw_params_set_channels( stream, hw_params, channels ) );

    /* ... set the period size, which is essentially the hardware buffer size */
    ENSURE( snd_pcm_hw_params_set_period_size( stream, hw_params, 
                                               framesPerBuffer, 0 ) );

    PA_DEBUG(("numperiods: %d\n", numPeriods));
    if( snd_pcm_hw_params_set_periods ( stream, hw_params, numPeriods, 0 ) < 0 )
    {
        int i;
        for( i = numPeriods; i >= 2; i-- )
        {
            if( snd_pcm_hw_params_set_periods( stream, hw_params, i, 0 ) >= 0 )
            {
                PA_DEBUG(("settled on %d periods\n", i));
                break;
            }
        }
    }


    /* Set the parameters! */
    ENSURE( snd_pcm_hw_params( stream, hw_params ) );


    /* Now software parameters... */

    ENSURE( snd_pcm_sw_params_current( stream, sw_params ) );

    /* until there's explicit xrun support in PortAudio, we'll configure ALSA
     * devices never to stop on account of an xrun.  Basically, if the software
     * falls behind and there are dropouts, we'll never even know about it.
     */
    ENSURE( snd_pcm_sw_params_set_stop_threshold( stream, sw_params, (snd_pcm_uframes_t)-1) );

    /* Set the parameters! */
    ENSURE( snd_pcm_sw_params( stream, sw_params ) );

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
    PaAlsaHostApiRepresentation *alsaHostApi =
        (PaAlsaHostApiRepresentation*)hostApi;
    PaAlsaStream *stream = 0;
    PaSampleFormat hostInputSampleFormat = 0, hostOutputSampleFormat = 0;
    int numInputChannels = 0, numOutputChannels = 0;
    PaSampleFormat inputSampleFormat = 0, outputSampleFormat = 0;
    unsigned long framesPerHostBuffer = framesPerBuffer;

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
        result = paInsufficientMemory;
        goto error;
    }
    InitializeStream( stream, (int) callback, streamFlags );    // Initialize structure

    if( callback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &alsaHostApi->callbackStreamInterface,
                                               callback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &alsaHostApi->blockingStreamInterface,
                                               callback, userData );
    }


    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    /* open the devices now, so we can obtain info about the available formats */

    char deviceName[50];
    if( numInputChannels > 0 )
    {
        int ret;
        snprintf( deviceName, 50, "hw:CARD=%s", hostApi->deviceInfos[inputParameters->device]->name );
		if( (ret = snd_pcm_open( &stream->pcm_capture, deviceName, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK )) < 0 )
        {
            if (ret == -EBUSY)
                result = paDeviceUnavailable;
            else
                result = paBadIODeviceCombination;
            goto error;
        }
        if( snd_pcm_nonblock( stream->pcm_capture, 0 ) < 0 )
        {
            result = paUnanticipatedHostError;
            goto error;
        }

        stream->capture_nfds = snd_pcm_poll_descriptors_count( stream->pcm_capture );

        hostInputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats(stream->pcm_capture),
                                                 inputSampleFormat );
    }

    if( numOutputChannels > 0 )
    {
        int ret;
        snprintf( deviceName, 50, "hw:CARD=%s", hostApi->deviceInfos[outputParameters->device]->name );
        if( (ret = snd_pcm_open( &stream->pcm_playback, deviceName, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK )) < 0 )
        {
            if (ret == -EBUSY)
                result = paDeviceUnavailable;
            else
                result = paBadIODeviceCombination;
            goto error;
        }
        if( snd_pcm_nonblock( stream->pcm_playback, 0 ) < 0 )
        {
            result = paUnanticipatedHostError;
            goto error;
        }

        stream->playback_nfds = snd_pcm_poll_descriptors_count( stream->pcm_playback );

        hostOutputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats(stream->pcm_playback),
                                                 outputSampleFormat );
        stream->playback_hostsampleformat = hostOutputSampleFormat;
    }


    /* If the number of frames per buffer is unspecified, we have to come up with
     * one.  This is both a blessing and a curse: a blessing because we can optimize
     * the number to best meet the requirements, but a curse because that's really
     * hard to do well.  For this reason we also support an interface where the user
     * specifies these by setting environment variables. */
    if( framesPerBuffer == paFramesPerBufferUnspecified )
    {
        if( getenv("PA_PERIODSIZE") != NULL )
            framesPerHostBuffer = atoi( getenv("PA_PERIODSIZE") );
        else
        {
            /* We need to determine how many frames per host buffer to use.  Our
             * goals are to provide the best possible performance, but also to
             * most closely honor the requested latency settings.  Therefore this
             * decision is based on:
             *
             *   - the period sizes that playback and/or capture support.  The
             *     host buffer size has to be one of these.
             *   - the number of periods that playback and/or capture support.
             *
             * We want to make period_size*(num_periods-1) to be as close as possible
             * to latency*rate for both playback and capture.
             *
             * This is one of those blocks of code that will just take a lot of
             * refinement to be any good.
             */

            int reasonablePeriodSizes[] = { 256, 512, 1024, 2048, 4096, 8192, 16384, 128, 0 };
            int i;
#define ENSURE(x) \
            if ( (x) < 0 ) \
            { \
                PA_DEBUG(("failed at line %d\n", __LINE__)); \
                continue; \
            }

            if( stream->pcm_capture && stream->pcm_playback )
            {
                /* TODO */
            }
            else
            {
                /* half-duplex is a slightly simpler case */
                int desiredLatency, channels;
                snd_pcm_t *pcm_handle;
                snd_pcm_hw_params_t *hw_params;
                snd_pcm_hw_params_alloca( &hw_params );

                framesPerHostBuffer = paFramesPerBufferUnspecified;

                if( stream->pcm_capture )
                {
                    pcm_handle = stream->pcm_capture;
                    desiredLatency = inputParameters->suggestedLatency * sampleRate;
                    channels = inputParameters->channelCount;
                }
                else
                {
                    pcm_handle = stream->pcm_playback;
                    desiredLatency = outputParameters->suggestedLatency * sampleRate;
                    channels = outputParameters->channelCount;
                }


                for( i = 0; reasonablePeriodSizes[i] != 0; i++ )
                {
                    int periodSize = reasonablePeriodSizes[i];
                    int numPeriods = ( desiredLatency / periodSize ) + 1;
                    if( numPeriods <= 1 )
                        numPeriods = 2;

                    PA_DEBUG(("trying periodSize=%d, numPeriods=%d, sampleRate=%f, channels=%d\n", periodSize, numPeriods, sampleRate, channels));
                    ENSURE( snd_pcm_hw_params_any( pcm_handle, hw_params ) );
                    ENSURE( snd_pcm_hw_params_set_rate( pcm_handle, hw_params, sampleRate, 0 ) );
                    ENSURE( snd_pcm_hw_params_set_channels( pcm_handle, hw_params, channels ) );
                    ENSURE( snd_pcm_hw_params_set_period_size( pcm_handle, hw_params, periodSize, 0 ) );
                    ENSURE( snd_pcm_hw_params_set_periods( pcm_handle, hw_params, numPeriods, 0 ) );

                    /* if we made it this far, we have a winner. */
                    framesPerHostBuffer = periodSize;
                    PA_DEBUG(("I came up with %ld frames per host buffer.\n", framesPerHostBuffer));
                    break;
                }

                /* if we didn't find an acceptable host buffer size 
                 * (this should be extremely rare) */
                if( framesPerHostBuffer == paFramesPerBufferUnspecified )
                {
                    result = paBadIODeviceCombination;
                    goto error;
                }
            }
        }
    }
    else
    {
        framesPerHostBuffer = framesPerBuffer;
    }
#undef ENSURE


    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              numInputChannels, inputSampleFormat, hostInputSampleFormat,
              numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerBuffer, framesPerHostBuffer,
              paUtilFixedHostBufferSize, callback, userData );
    if( result != paNoError )
        goto error;

    /* configure the streams */
    stream->streamRepresentation.streamInfo.inputLatency = 0.;
    stream->streamRepresentation.streamInfo.outputLatency = 0.;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    if( numInputChannels > 0 )
    {
        int interleaved;
        PaSampleFormat plain_format = hostInputSampleFormat & ~paNonInterleaved;

        if( inputSampleFormat & paNonInterleaved )
            interleaved = 0;
        else
            interleaved = 1;

        if( ConfigureStream( stream->pcm_capture, numInputChannels, interleaved,
                             sampleRate, plain_format, framesPerHostBuffer,
                             inputParameters->suggestedLatency) != 0 )
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
                             sampleRate, plain_format, framesPerHostBuffer,
                             outputParameters->suggestedLatency) != 0 )
        {
            result = paBadIODeviceCombination;
            goto error;
        }

        stream->playback_interleaved = interleaved;
    }

    /* this will cause the two streams to automatically start/stop/prepare in sync.
     * We only need to execute these operations on one of the pair. */
    if( stream->pcm_capture && stream->pcm_playback )
        snd_pcm_link( stream->pcm_capture, stream->pcm_playback );

    /* TODO: free this properly */
    stream->pfds = (struct pollfd*)PaUtil_AllocateMemory( (stream->capture_nfds +
                                                           stream->playback_nfds + 1) *
                                                           sizeof(struct pollfd) );
    if( !stream->pfds )
    {
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
        CleanUpStream( stream );

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

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    CleanUpStream( stream );

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
            PA_DEBUG(("call at %d failed\n", __LINE__)); \
            return paUnanticipatedHostError; \
        } \
        else \
            PA_DEBUG(("call at line %d succeeded\n", __LINE__)); \
    }

    if( stream->pcm_playback )
    {
        const snd_pcm_channel_area_t *playback_areas, *area;
        snd_pcm_uframes_t offset, frames;
        int sample_size = Pa_GetSampleSize( stream->playback_hostsampleformat );
        PA_DEBUG(("Sample size: %d\n", sample_size ));
        ENSURE( snd_pcm_prepare( stream->pcm_playback ) );
        frames = snd_pcm_avail_update( stream->pcm_playback );
        PA_DEBUG(("frames: %ld\n", frames ));
        PA_DEBUG(("channels: %d\n", stream->playback_channels ));

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
    else if( stream->pcm_capture )
    {
        ENSURE( snd_pcm_prepare( stream->pcm_capture ) );
    }

    if( stream->callback_mode )
    {
        ENSURE( pthread_create( &stream->callback_thread, NULL, &CallbackThread, stream ) );

        /* we'll do the snd_pcm_start() in the callback thread */
    }
    else
    {
        if( stream->pcm_playback )
            snd_pcm_start( stream->pcm_playback );
        else if( stream->pcm_capture )
            snd_pcm_start( stream->pcm_capture );
    }

    /* On my machine, the pcm stream will not transition to the RUNNING
     * state for a while after snd_pcm_start is called.  The PortAudio
     * client needs to be able to depend on Pa_IsStreamActive() returning
     * true the moment after this function returns.  So I sleep briefly here.
     *
     * I don't like this one bit.
     */
    Pa_Sleep( 100 );

    stream->callback_finished = 0;
    stream->callbackAbort = 0;

    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* First deal with the callback thread, cancelling and/or joining
     * it if necessary
     */

    if( stream->callback_mode )
    {
        if( stream->callback_finished )
            pthread_join( stream->callback_thread, NULL );  // Just wait for it to die
        else
        {
            /* We are running in callback mode, and the callback thread
             * is still running.  Cancel it and wait for it to be done. */
            pthread_cancel( stream->callback_thread );      // Snuff it!
            pthread_join( stream->callback_thread, NULL );
        }

        stream->callback_finished = 0;
    }
    else
    {
        if( stream->pcm_playback )
            snd_pcm_drain( stream->pcm_playback );
        if( stream->pcm_capture && !stream->pcmsSynced )
            snd_pcm_drain( stream->pcm_capture );
    }

    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* First deal with the callback thread, cancelling and/or joining
     * it if necessary
     */

    if( stream->callback_mode )
    {
        stream->callbackAbort = 1;

        if( stream->callback_finished )
            pthread_join( stream->callback_thread, NULL );  // Just wait for it to die
        else
        {
            /* We are running in callback mode, and the callback thread
             * is still running.  Cancel it and wait for it to be done. */
            pthread_cancel( stream->callback_thread );      // Snuff it!
            pthread_join( stream->callback_thread, NULL );
        }

        stream->callback_finished = 0;
    }
    else
    {
        if( stream->pcm_playback )
            snd_pcm_drop( stream->pcm_playback );
        if( stream->pcm_capture && !stream->pcmsSynced )
            snd_pcm_drop( stream->pcm_capture );
    }

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

void InitializeStream( PaAlsaStream *stream, int callback, PaStreamFlags streamFlags )
{
    assert( stream );

    stream->pcm_capture = NULL;
    stream->pcm_playback = NULL;
    stream->callback_finished = 0;
    stream->callback_mode = callback;
    stream->capture_nfds = 0;
    stream->playback_nfds = 0;
    stream->pfds = NULL;
    stream->callbackAbort = 0;
}

/*!
 * \brief Free resources associated with stream, and eventually stream itself
 *
 * Frees allocated memory, and closes opened pcms.
 */
void CleanUpStream( PaAlsaStream *stream )
{
    assert( stream );

    if( stream->pcm_capture )
    {
        snd_pcm_close( stream->pcm_capture );
    }
    if( stream->pcm_playback )
    {
        snd_pcm_close( stream->pcm_playback );
    }

    if ( stream->pfds )
        PaUtil_FreeMemory( stream->pfds );

    PaUtil_FreeMemory( stream );
}

