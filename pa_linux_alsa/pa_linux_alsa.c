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
#include <string.h> /* strlen() */
#include <limits.h>

#include "portaudio.h"
#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_linux_alsa.h"

pthread_mutex_t mtx;    /* Used for synchronizing access to SetLastHostErrorInfo */

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

typedef struct PaAlsaDeviceInfo
{
    PaDeviceInfo commonDeviceInfo;
    int deviceNumber;
}
PaAlsaDeviceInfo;


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
int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate );
int GetExactSampleRate( snd_pcm_hw_params_t *hwParams, double *sampleRate );

/* blocking calls are in blocking_calls.c */
extern PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
extern PaError WriteStream( PaStream* stream, const void *buffer, unsigned long frames );
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
    (*hostApi)->info.name = "ALSA";

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &alsaHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &alsaHostApi->blockingStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream,
                                      GetStreamReadAvailable,
                                      GetStreamWriteAvailable );

    BuildDeviceList( alsaHostApi );
    pthread_mutex_init( &mtx, NULL );

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


/*!
  \brief Determine max channels and default latencies

  This function provides functionality to grope an opened (might be opened for capture or playback) pcm device for 
  traits like max channels, suitable default latencies and default sample rate. Upon error, max channels is set to zero,
  and a suitable result returned. The device is closed before returning.
  */
static PaError GropeDevice( snd_pcm_t *pcm, int *channels, double *defaultLowLatency,
        double *defaultHighLatency, double *defaultSampleRate )
{
    PaError result = paNoError;
    snd_pcm_hw_params_t *hwParams;
    snd_pcm_uframes_t lowLatency = 1024, highLatency = 16384;

    assert( pcm );
    snd_pcm_hw_params_alloca( &hwParams );
    snd_pcm_hw_params_any( pcm, hwParams );

    ENSURE( snd_pcm_hw_params_get_channels_max( hwParams, (unsigned int *) channels ), paUnanticipatedHostError );

    if( *defaultSampleRate == 0 )           /* Default sample rate not set */
    {
        unsigned int sampleRate = 44100;        /* Will contain approximate rate returned by alsa-lib */
        ENSURE( snd_pcm_hw_params_set_rate_near( pcm, hwParams, &sampleRate, 0 ), paUnanticipatedHostError );
        ENSURE( GetExactSampleRate( hwParams, defaultSampleRate ), paUnanticipatedHostError );
    }

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
    ENSURE( snd_pcm_hw_params_set_buffer_size_near( pcm, hwParams, &lowLatency ), paUnanticipatedHostError );

    /* Have to reset hwParams, to set new buffer size */
    ENSURE( snd_pcm_hw_params_any( pcm, hwParams ), paUnanticipatedHostError ); 
    ENSURE( snd_pcm_hw_params_set_buffer_size_near( pcm, hwParams, &highLatency ), paUnanticipatedHostError );

    *defaultLowLatency = (double) lowLatency / *defaultSampleRate;
    *defaultHighLatency = (double) highLatency / *defaultSampleRate;

end:
    snd_pcm_close( pcm );
    return result;

error:
    *channels = 0;
    goto end;
}


static PaError BuildDeviceList( PaAlsaHostApiRepresentation *alsaApi )
{
    PaUtilHostApiRepresentation *commonApi = &alsaApi->commonHostApiRep;
    PaAlsaDeviceInfo *deviceInfoArray;
    int deviceCount = 0;
    int card_idx;
    int device_idx;
    snd_ctl_t *ctl;
    snd_ctl_card_info_t *card_info;
    PaError result = paNoError;

    /* These two will be set to the first working input and output device, respectively */
    commonApi->info.defaultInputDevice = paNoDevice;
    commonApi->info.defaultOutputDevice = paNoDevice;

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
        ++deviceCount;
    }

    /* allocate deviceInfo memory based on the number of devices */

    commonApi->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaDeviceInfo*) * deviceCount );
    if( !commonApi->deviceInfos )
    {
        return paInsufficientMemory;
    }

    /* allocate all device info structs in a contiguous block */
    deviceInfoArray = (PaAlsaDeviceInfo*)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaAlsaDeviceInfo) * deviceCount );
    if( !deviceInfoArray )
    {
        return paInsufficientMemory;
    }

    /* now loop over the list of devices again, filling in the deviceInfo for each */
    card_idx = -1;
    device_idx = 0;
    snd_ctl_card_info_alloca( &card_info );
    while( snd_card_next( &card_idx ) == 0 && card_idx >= 0 )
    {
        PaAlsaDeviceInfo *deviceInfo = &deviceInfoArray[device_idx];
        PaDeviceInfo *commonDeviceInfo = &deviceInfo->commonDeviceInfo;
        snd_pcm_t *pcm;
        char *deviceName;
        char alsaDeviceName[50];
        const char *cardName;
        int ret;

        /* A: Storing pointer to PaAlsaDeviceInfo object as pointer to PaDeviceInfo object */
        commonApi->deviceInfos[device_idx] = (PaDeviceInfo *) deviceInfo;

        deviceInfo->deviceNumber = card_idx;
        commonDeviceInfo->structVersion = 2;
        commonDeviceInfo->hostApi = alsaApi->hostApiIndex;

        sprintf( alsaDeviceName, "hw:%d", card_idx );
        snd_ctl_open( &ctl, alsaDeviceName, 0 );
        snd_ctl_card_info( ctl, card_info );
        snd_ctl_close( ctl );
        cardName = snd_ctl_card_info_get_name( card_info );

        deviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                        strlen(cardName) + 1 );
        if( !deviceName )
        {
            return paInsufficientMemory;
        }
        strcpy( deviceName, cardName );
        commonDeviceInfo->name = deviceName;

        /* to determine max. channels, we must open the device and query the
         * hardware parameter configuration space */
        commonDeviceInfo->defaultSampleRate = 0.0;

        /* get max channels for capture */
        commonDeviceInfo->maxInputChannels = 0;
        if( (ret = snd_pcm_open( &pcm, alsaDeviceName, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK )) >= 0 ) {
            ENSURE( snd_pcm_nonblock( pcm, 0 ), paUnanticipatedHostError );

            if( GropeDevice( pcm, &commonDeviceInfo->maxInputChannels, &commonDeviceInfo->defaultLowInputLatency,
                    &commonDeviceInfo->defaultHighInputLatency, &commonDeviceInfo->defaultSampleRate ) == paNoError
                    && commonApi->info.defaultInputDevice == paNoDevice )
                commonApi->info.defaultInputDevice = device_idx;
        }
                

        /* get max channels for playback */
        commonDeviceInfo->maxOutputChannels = 0;
        if( (ret = snd_pcm_open( &pcm, alsaDeviceName, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK )) >= 0 )
        {
            ENSURE( snd_pcm_nonblock( pcm, 0 ), paUnanticipatedHostError );

            if( GropeDevice( pcm, &commonDeviceInfo->maxOutputChannels, &commonDeviceInfo->defaultLowOutputLatency,
                    &commonDeviceInfo->defaultHighOutputLatency, &commonDeviceInfo->defaultSampleRate ) == paNoError
                    && commonApi->info.defaultOutputDevice == paNoDevice )
                commonApi->info.defaultOutputDevice = device_idx;
        }

        ++device_idx;
    }

    commonApi->info.deviceCount = deviceCount;

end:
    return result;

error:
    goto end;   /* No particular action */
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{

    PaAlsaHostApiRepresentation *alsaHostApi = (PaAlsaHostApiRepresentation*)hostApi;

    assert( hostApi );
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
    pthread_mutex_destroy( &mtx );
}


static PaError ValidateParameters( const PaStreamParameters *parameters, int maxChannels )
{
    /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification
            [JH] this could be supported in the future, to allow ALSA device strings
                 like hw:0 */
    if( parameters->device == paUseHostApiSpecificDeviceSpecification )
        return paInvalidDevice;
    if( parameters->channelCount > maxChannels )
        return paInvalidChannelCount;
    if( parameters->hostApiSpecificStreamInfo )
        return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */

    return paNoError;
}


/* Given an open stream, what sample formats are available? */

static PaSampleFormat GetAvailableFormats( snd_pcm_t *pcm )
{
    PaSampleFormat available = 0;
    snd_pcm_hw_params_t *hwParams;
    snd_pcm_hw_params_alloca( &hwParams );

    snd_pcm_hw_params_any( pcm, hwParams );

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT ) == 0)
        available |= paFloat32;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S16 ) == 0)
        available |= paInt16;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24 ) == 0)
        available |= paInt24;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S32 ) == 0)
        available |= paInt32;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S8 ) == 0)
        available |= paInt8;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U8 ) == 0)
        available |= paUInt8;

    return available;
}


static PaError TestParameters( const PaStreamParameters *parameters, int cardID, double sampleRate )
{
    PaError result = paNoError;
    char device[50];
    snd_pcm_t *pcm = NULL;
    PaSampleFormat availableFormats;
    PaSampleFormat format;
    int ret;
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca( &params );

    snprintf( device, 50, "hw:%d", cardID );
    if( (ret = snd_pcm_open( &pcm, device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK )) < 0 )
    {
        pcm = NULL;     /* Not to be closed */
        /* Take action based on return value */
        ENSURE( ret, ret == -EBUSY ? paDeviceUnavailable : paUnanticipatedHostError );
    }
    ENSURE( snd_pcm_nonblock( pcm, 0 ), paUnanticipatedHostError );

    snd_pcm_hw_params_any( pcm, params );
    if( SetApproximateSampleRate( pcm, params, sampleRate ) < 0 )
    {
        result = paInvalidSampleRate;
        goto end;
    }
    if( snd_pcm_hw_params_set_channels( pcm, params, parameters->channelCount ) < 0 )
    {
        result = paInvalidChannelCount;
        goto end;
    }

    /* See if we can find a best possible match */
    availableFormats = GetAvailableFormats( pcm );
    if ( (format = PaUtil_SelectClosestAvailableFormat( availableFormats, parameters->sampleFormat ))
                == paSampleFormatNotSupported )
    {
        result = (PaError) format;
        goto end;
    }

end:
    if( pcm )
        snd_pcm_close( pcm );
    return result;

error:
    goto end;
}


static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount = 0, outputChannelCount = 0;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaError result;

    if( inputParameters )
    {
        if( (result = ValidateParameters( inputParameters,
                hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )) != paNoError )
            return result;

        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
    }

    if( outputParameters )
    {
        if( (result = ValidateParameters( outputParameters,
                hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )) != paNoError )
            return result;

        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
    }

    /*
        IMPLEMENT ME:

            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported if necessary

            - check that the device supports sampleRate

        Because the buffer adapter handles conversion between all standard
        sample formats, the following checks are only required if paCustomFormat
        is implemented, or under some other unusual conditions.

            - check that input device can support inputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - check that output device can support outputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format
    */

    if( inputChannelCount )
    {
        PaAlsaDeviceInfo *devInfo = (PaAlsaDeviceInfo *) hostApi->deviceInfos[ inputParameters->device ];
        if( (result = TestParameters( inputParameters, devInfo->deviceNumber, sampleRate )) != paNoError )
            return result;
    }

    if ( outputChannelCount )
    {
        PaAlsaDeviceInfo *devInfo = (PaAlsaDeviceInfo *) hostApi->deviceInfos[ outputParameters->device ];
        if( (result = TestParameters( outputParameters, devInfo->deviceNumber,
                sampleRate )) != paNoError )
            return result;
    }

    return paFormatIsSupported;
}


static snd_pcm_format_t Pa2AlsaFormat( PaSampleFormat paFormat )
{
    switch( paFormat )
    {
        case paFloat32:
            return SND_PCM_FORMAT_FLOAT;

        case paInt16:
            return SND_PCM_FORMAT_S16;

        case paInt24:
            return SND_PCM_FORMAT_S24;

        case paInt32:
            return SND_PCM_FORMAT_S32;

        case paInt8:
            return SND_PCM_FORMAT_S8;

        case paUInt8:
            return SND_PCM_FORMAT_U8;

        default:
            return SND_PCM_FORMAT_UNKNOWN;
    }
}


/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

static PaError ConfigureStream( snd_pcm_t *pcm, int channels,
                                int *interleaved, double *sampleRate,
                                PaSampleFormat paFormat, unsigned long *framesPerBuffer,
                                PaTime *latency, int primeBuffers, snd_pcm_uframes_t *startThreshold )
{
    /*
    int numPeriods;

    if( getenv("PA_NUMPERIODS") != NULL )
        numPeriods = atoi( getenv("PA_NUMPERIODS") );
    else
        numPeriods = ( (*latency * sampleRate) / *framesPerBuffer ) + 1;

    PA_DEBUG(( "latency: %f, rate: %f, framesPerBuffer: %d\n", *latency, sampleRate, *framesPerBuffer ));
    if( numPeriods <= 1 )
        numPeriods = 2;
    */

    /* configuration consists of setting all of ALSA's parameters.
     * These parameters come in two flavors: hardware parameters
     * and software paramters.  Hardware parameters will affect
     * the way the device is initialized, software parameters
     * affect the way ALSA interacts with me, the user-level client. */

    snd_pcm_hw_params_t *hwParams;
    snd_pcm_sw_params_t *swParams;
    PaError result = paNoError;
    snd_pcm_access_t accessMode, alternateAccessMode;
    snd_pcm_format_t alsaFormat;
    unsigned int bufTime;
    unsigned int numPeriods;
    snd_pcm_uframes_t threshold = 0;

    assert(pcm);

    snd_pcm_hw_params_alloca( &hwParams );
    snd_pcm_sw_params_alloca( &swParams );

    /* ... fill up the configuration space with all possibile
     * combinations of parameters this device will accept */
    ENSURE( snd_pcm_hw_params_any( pcm, hwParams ), paUnanticipatedHostError );

    if( *interleaved )
    {
        accessMode = SND_PCM_ACCESS_MMAP_INTERLEAVED;
        alternateAccessMode = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
    }
    else
    {
        accessMode = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
        alternateAccessMode = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    }

    /* If requested access mode fails, try alternate mode */
    if( snd_pcm_hw_params_set_access( pcm, hwParams, accessMode ) < 0 ) {
        ENSURE( snd_pcm_hw_params_set_access( pcm, hwParams, alternateAccessMode ), paUnanticipatedHostError );
        *interleaved = !(*interleaved);     /* Flip mode */
    }

    /* set the format based on what the user selected */
    alsaFormat = Pa2AlsaFormat( paFormat );
    if ( alsaFormat == SND_PCM_FORMAT_UNKNOWN )
        return paSampleFormatNotSupported;
    ENSURE( snd_pcm_hw_params_set_format( pcm, hwParams, alsaFormat ), paUnanticipatedHostError );

    /* ... set the sample rate */
    ENSURE( SetApproximateSampleRate( pcm, hwParams, *sampleRate ), paInvalidSampleRate );
    ENSURE( GetExactSampleRate( hwParams, sampleRate ), paUnanticipatedHostError );

    /* ... set the number of channels */
    ENSURE( snd_pcm_hw_params_set_channels( pcm, hwParams, channels ), paInvalidChannelCount );

    /* Set buffer size */
    ENSURE( snd_pcm_hw_params_set_periods_integer( pcm, hwParams ), paUnanticipatedHostError );
    ENSURE( snd_pcm_hw_params_set_period_size_integer( pcm, hwParams ), paUnanticipatedHostError );
    ENSURE( snd_pcm_hw_params_set_period_size( pcm, hwParams, *framesPerBuffer, 0 ), paUnanticipatedHostError );

    /* Find an acceptable number of periods */
    numPeriods = (*latency * *sampleRate) / *framesPerBuffer + 1;
    ENSURE( snd_pcm_hw_params_set_periods_near( pcm, hwParams, &numPeriods, 0 ), paUnanticipatedHostError );

    /*
    PA_DEBUG(( "numperiods: %d\n", numPeriods ));
    if( snd_pcm_hw_params_set_periods ( pcm, hwParams, numPeriods, 0 ) < 0 )
    {
        int i;
        for( i = numPeriods; i >= 2; i-- )
        {
            if( snd_pcm_hw_params_set_periods( pcm, hwParams, i, 0 ) >= 0 )
            {
                PA_DEBUG(( "settled on %d periods\n", i ));
                break;
            }
        }
    }
    */

    /* Set the parameters! */
    ENSURE( snd_pcm_hw_params( pcm, hwParams ), paUnanticipatedHostError );

    /* Obtain correct latency */
    ENSURE( snd_pcm_hw_params_get_buffer_time( hwParams, &bufTime, 0 ), paUnanticipatedHostError );
    bufTime -= bufTime / numPeriods;    /* One period is not counted as latency */
    *latency = (PaTime) bufTime / 1000000; /* Latency in seconds */

    /* Now software parameters... */
    /* snd_pcm_uframes_t boundary; */
    ENSURE( snd_pcm_sw_params_current( pcm, swParams ), paUnanticipatedHostError );

    /* If the user wants to prime the buffer before stream start, the start threshold will equal buffer size */
    threshold = 0;
    if( primeBuffers )
    {
        snd_pcm_uframes_t bufSz;
        ENSURE( snd_pcm_hw_params_get_buffer_size( hwParams, &bufSz ), paUnanticipatedHostError );
        *startThreshold = bufSz;
    }
    else    /* Fill buffer with silence */
    {
        snd_pcm_uframes_t boundary;
        ENSURE( snd_pcm_sw_params_get_boundary( swParams, &boundary ), paUnanticipatedHostError );
        ENSURE( snd_pcm_sw_params_set_silence_threshold( pcm, swParams, 0 ), paUnanticipatedHostError );
        ENSURE( snd_pcm_sw_params_set_silence_size( pcm, swParams, boundary ), paUnanticipatedHostError );
    }
    ENSURE( snd_pcm_sw_params_set_start_threshold( pcm, swParams, threshold ), paUnanticipatedHostError );
        
    ENSURE( snd_pcm_sw_params_set_avail_min( pcm, swParams, *framesPerBuffer ), paUnanticipatedHostError );

    /* until there's explicit xrun support in PortAudio, we'll configure ALSA
     * devices never to stop on account of an xrun.  Basically, if the software
     * falls behind and there are dropouts, we'll never even know about it.
     */
    ENSURE( snd_pcm_sw_params_set_stop_threshold( pcm, swParams, (snd_pcm_uframes_t)-1), paUnanticipatedHostError );

    /* Set the parameters! */
    ENSURE( snd_pcm_sw_params( pcm, swParams ), paUnanticipatedHostError );

end:
    return result;

error:
    goto end;   /* No particular action */
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
    PaAlsaHostApiRepresentation *alsaHostApi = (PaAlsaHostApiRepresentation*)hostApi;
    PaAlsaDeviceInfo *inputDeviceInfo = 0, *outputDeviceInfo = 0;
    PaAlsaStream *stream = 0;
    PaSampleFormat hostInputSampleFormat = 0, hostOutputSampleFormat = 0;
    int numInputChannels = 0, numOutputChannels = 0;
    PaSampleFormat inputSampleFormat = 0, outputSampleFormat = 0;
    unsigned long framesPerHostBuffer = framesPerBuffer;
    int ofs = 0;

    if( inputParameters )
    {
        inputDeviceInfo = (PaAlsaDeviceInfo*)hostApi->deviceInfos[ inputParameters->device ];
        if ( (result = ValidateParameters( inputParameters, inputDeviceInfo->commonDeviceInfo.maxInputChannels ))
                != paNoError )
            return result;

        numInputChannels = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
    }
    else
    {
        numInputChannels = 0;
    }

    if( outputParameters )
    {
        outputDeviceInfo = (PaAlsaDeviceInfo*)hostApi->deviceInfos[ outputParameters->device ];
        if( (result = ValidateParameters( outputParameters, outputDeviceInfo->commonDeviceInfo.maxOutputChannels ))
                != paNoError )
            return result;

        numOutputChannels = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
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
    InitializeStream( stream, (int) callback, streamFlags );    /* Initialize structure */

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

    if( numInputChannels > 0 )
    {
        int ret;
        char deviceName[50];

        snprintf( deviceName, 50, "hw:%d", inputDeviceInfo->deviceNumber );
        if( (ret = snd_pcm_open( &stream->pcm_capture, deviceName, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK )) < 0 )
        {
            stream->pcm_capture = NULL;     /* Not to be closed */
            ENSURE( ret, ret == -EBUSY ? paDeviceUnavailable : paBadIODeviceCombination );
        }
        ENSURE( snd_pcm_nonblock( stream->pcm_capture, 0 ), paUnanticipatedHostError );

        stream->capture_nfds = snd_pcm_poll_descriptors_count( stream->pcm_capture );

        hostInputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats(stream->pcm_capture),
                                                 inputSampleFormat );
    }

    if( numOutputChannels > 0 )
    {
        int ret;
        char deviceName[50];

        snprintf( deviceName, 50, "hw:%d", outputDeviceInfo->deviceNumber );
        if( (ret = snd_pcm_open( &stream->pcm_playback, deviceName, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK )) < 0 )
        {
            stream->pcm_playback = NULL;     /* Not to be closed */
            ENSURE( ret, ret == -EBUSY ? paDeviceUnavailable : paBadIODeviceCombination );
        }
        ENSURE( snd_pcm_nonblock( stream->pcm_playback, 0 ), paUnanticipatedHostError );

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

            if( stream->pcm_capture && stream->pcm_playback )
            {
                /* TODO */
            }
            else
            {
                /* half-duplex is a slightly simpler case */
                unsigned long desiredLatency, channels;
                snd_pcm_uframes_t minPeriodSize;
                unsigned int numPeriods;
                snd_pcm_t *pcm;
                snd_pcm_hw_params_t *hwParams;
                snd_pcm_hw_params_alloca( &hwParams );

                if( stream->pcm_capture )
                {
                    pcm = stream->pcm_capture;
                    desiredLatency = inputParameters->suggestedLatency * sampleRate;
                    channels = inputParameters->channelCount;
                }
                else
                {
                    pcm = stream->pcm_playback;
                    desiredLatency = outputParameters->suggestedLatency * sampleRate;
                    channels = outputParameters->channelCount;
                }

                ENSURE( snd_pcm_hw_params_any( pcm, hwParams ), paUnanticipatedHostError );
                ENSURE( SetApproximateSampleRate( pcm, hwParams, sampleRate ), paBadIODeviceCombination );
                ENSURE( snd_pcm_hw_params_set_channels( pcm, hwParams, channels ), paBadIODeviceCombination );

                ENSURE( snd_pcm_hw_params_set_period_size_integer( pcm, hwParams ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_set_periods_integer( pcm, hwParams ), paUnanticipatedHostError );

                /* Using 5 as a base number of buffers, we try to approximate the suggested latency (+1 period),
                   finding a period size which best fits these constraints */
                ENSURE( snd_pcm_hw_params_get_period_size_min( hwParams, &minPeriodSize, 0 ), paUnanticipatedHostError );
                numPeriods = MIN( desiredLatency / minPeriodSize, 4 ) + 1;
                ENSURE( snd_pcm_hw_params_set_periods_near( pcm, hwParams, &numPeriods, 0 ), paUnanticipatedHostError );

                framesPerHostBuffer = desiredLatency / (numPeriods - 1);
                ENSURE( snd_pcm_hw_params_set_period_size_near( pcm, hwParams, &framesPerHostBuffer, 0 ), paUnanticipatedHostError );
            }
        }
    }
    else
    {
        framesPerHostBuffer = framesPerBuffer;
    }

    /* Will fill in correct values later */
    stream->streamRepresentation.streamInfo.inputLatency = 0.;
    stream->streamRepresentation.streamInfo.outputLatency = 0.;

    if( numInputChannels > 0 )
    {
        int interleaved = !(inputSampleFormat & paNonInterleaved);
        PaSampleFormat plain_format = hostInputSampleFormat & ~paNonInterleaved;

        PaTime latency = inputParameters->suggestedLatency; /* Real latency in seconds returned from ConfigureStream */
        if( (result = ConfigureStream( stream->pcm_capture, numInputChannels, &interleaved,
                             &sampleRate, plain_format, &framesPerHostBuffer,
                             &latency, 0, 0 )) != paNoError )
        {
            goto error;
        }

        stream->capture_interleaved = interleaved;
        stream->streamRepresentation.streamInfo.inputLatency = latency;
    }

    if( numOutputChannels > 0 )
    {
        int interleaved = !(outputSampleFormat & paNonInterleaved);
        PaSampleFormat plain_format = hostOutputSampleFormat & ~paNonInterleaved;

        PaTime latency = outputParameters->suggestedLatency; /* Real latency in seconds returned from ConfigureStream */
        int primeBuffers = streamFlags & paPrimeOutputBuffersUsingStreamCallback;
        if( (result = ConfigureStream( stream->pcm_playback, numOutputChannels, &interleaved,
                             &sampleRate, plain_format, &framesPerHostBuffer,
                             &latency, primeBuffers, &stream->startThreshold )) != paNoError )
        {
            goto error;
        }

        stream->playback_interleaved = interleaved;
        stream->streamRepresentation.streamInfo.outputLatency = latency;
    }
    /* Should be exact now */
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    if ( (result = PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
                    numInputChannels, inputSampleFormat, hostInputSampleFormat,
                    numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
                    sampleRate, streamFlags, framesPerBuffer, framesPerHostBuffer,
                    paUtilFixedHostBufferSize, callback, userData )) != paNoError )
        goto error;

    /* this will cause the two streams to automatically start/stop/prepare in sync.
     * We only need to execute these operations on one of the pair. */
    if( stream->pcm_capture && stream->pcm_playback && snd_pcm_link( stream->pcm_capture, 
                stream->pcm_playback ) >= 0 )
            stream->pcmsSynced = 1;

    UNLESS( stream->pfds = (struct pollfd*)PaUtil_AllocateMemory( (stream->capture_nfds +
                    stream->playback_nfds + 1) * sizeof(struct pollfd) ), paInsufficientMemory );

    /* get the fds, packing all applicable fds into a single array,
     * so we can check them all with a single poll() call 
     * A: Might as well do this once during initialization, instead 
     * of for each iteration of the poll loop */
    if( stream->pcm_capture )
    {
        snd_pcm_poll_descriptors( stream->pcm_capture, stream->pfds, stream->capture_nfds );
        ofs += stream->capture_nfds;
    }
    if( stream->pcm_playback )
        snd_pcm_poll_descriptors( stream->pcm_playback, stream->pfds + ofs, stream->playback_nfds );

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

    /* A: I've decided to try starting playback in main thread, at least we know pcm will be started
       before we spawn. Otherwise we may exit with an appropriate error code */
    if( stream->pcm_playback )
    {
        ENSURE( snd_pcm_prepare( stream->pcm_playback ), paUnanticipatedHostError );
        ENSURE( snd_pcm_start( stream->pcm_playback ), paUnanticipatedHostError );
    }
    if( stream->pcm_capture && !stream->pcmsSynced )
    {
        ENSURE( snd_pcm_prepare( stream->pcm_capture ), paUnanticipatedHostError );
        ENSURE( snd_pcm_start( stream->pcm_capture ), paUnanticipatedHostError );
    }

    if( stream->callback_mode )
    {
        ENSURE( pthread_create( &stream->callback_thread, NULL, &CallbackThread, stream ), paInternalError );
    }
    else
    {
        /*
        if( stream->pcm_playback )
            snd_pcm_start( stream->pcm_playback );
        if( stream->pcm_capture && !stream->pcmsSynced )
            snd_pcm_start( stream->pcm_capture );
        */
    }

    /* On my machine, the pcm stream will not transition to the RUNNING
     * state for a while after snd_pcm_start is called.  The PortAudio
     * client needs to be able to depend on Pa_IsStreamActive() returning
     * true the moment after this function returns.  So I sleep briefly here.
     *
     * I don't like this one bit.
     */
    while( !IsStreamActive( stream ) )
    {
        PA_DEBUG(( "Not entered running state. Boo!\n" ));
        Pa_Sleep( 100 );
    }

end:
    return result;

error:
    goto end;   /* No particular action */
}


static PaError RealStop( PaStream *s, int (*StopFunc)( snd_pcm_t *) )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* These two are used to obtain return value when joining thread */
    int *pret;
    int retVal;

    /* First deal with the callback thread, cancelling and/or joining
     * it if necessary
     */
    if( stream->callback_mode )
    {
        if( stream->callback_finished )
        {
            pthread_join( stream->callback_thread, (void **) &pret );  /* Just wait for it to die */
            
            if( pret )  /* Message from dying thread */
            {
                retVal = *pret;
                free(pret);
                ENSURE(retVal, retVal);
            }
        }
        else
        {
            /* We are running in callback mode, and the callback thread
             * is still running.  Cancel it and wait for it to be done. */
            pthread_cancel( stream->callback_thread );      /* Snuff it! */
            pthread_join( stream->callback_thread, NULL );
            /* XXX: Some way to obtain return value from cancelled thread? */
        }

        stream->callback_finished = 0;
    }
    else
    {
        if( stream->pcm_playback )
            ENSURE( StopFunc( stream->pcm_playback ), paUnanticipatedHostError );
        if( stream->pcm_capture && !stream->pcmsSynced )
            ENSURE( StopFunc( stream->pcm_capture ), paUnanticipatedHostError );
    }

end:
    return result;

error:
    goto end;
}


static PaError StopStream( PaStream *s )
{
    ((PaAlsaStream *) s)->callbackAbort = 0;    /* In case abort has been called earlier */
    return RealStop( s, &snd_pcm_drain );
}


static PaError AbortStream( PaStream *s )
{
    ((PaAlsaStream *) s)->callbackAbort = 1;
    return RealStop( s, &snd_pcm_drop );
}


static PaError IsStreamStopped( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* callback_finished indicates we need to join callback thread (ie. in Abort/StopStream) */
    return !(IsStreamActive(s) || stream->callback_finished);
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

    return timestamp.tv_sec + ((PaTime)timestamp.tv_usec/1000000);
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
    stream->pcmsSynced = 0;
    stream->callbackAbort = 0;
    stream->startThreshold = 0;
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

int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate )
{
    unsigned long approx = (unsigned long) sampleRate;
    int dir;
    double fraction = sampleRate - approx;

    assert( pcm );
    assert( hwParams );

    if( fraction > 0.0 )
    {
        if( fraction > 0.5 )
        {
            ++approx;
            dir = -1;
        }
        else
            dir = 1;
    }
    else
        dir = 0;

    return snd_pcm_hw_params_set_rate( pcm, hwParams, approx, dir );
}

/* Return exact sample rate in param sampleRate */
int GetExactSampleRate( snd_pcm_hw_params_t *hwParams, double *sampleRate )
{
    unsigned int num, den;
    int err; 

    assert( hwParams );

    err = snd_pcm_hw_params_get_rate_numden( hwParams, &num, &den );
    *sampleRate = (double) num / den;

    return err;
}
