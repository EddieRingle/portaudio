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

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#undef ALSA_PCM_NEW_HW_PARAMS_API
#undef ALSA_PCM_NEW_SW_PARAMS_API

#include <sys/poll.h>
#include <string.h> /* strlen() */
#include <limits.h>
#include <math.h>
#include <pthread.h>

#include "portaudio.h"
#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_linux_alsa.h"

#define MIN(x,y) ( (x) < (y) ? (x) : (y) )
#define MAX(x,y) ( (x) > (y) ? (x) : (y) )

static pthread_mutex_t gmtx;    /* Global mutex */
static int aErr_;               /* Used with ENSURE */
static PaError paErr_;          /* Used with PA_ENSURE */

#define STRINGIZE_HELPER(exp) #exp
#define STRINGIZE(exp) STRINGIZE_HELPER(exp)

/* Check return value of ALSA function, and map it to PaError */
#define ENSURE(exp, code) \
    if( (aErr_ = (exp)) < 0 ) \
    { \
        if( (code) == paUnanticipatedHostError ) \
        { \
            pthread_mutex_lock( &gmtx ); \
            PaUtil_SetLastHostErrorInfo( paALSA, aErr_, snd_strerror( aErr_ ) ); \
            pthread_mutex_unlock( &gmtx ); \
        } \
        PA_DEBUG(( "Expression '" #exp "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = (code); \
        goto error; \
    }

/* Check PaError */
#define PA_ENSURE(exp) \
    if( (paErr_ = (exp)) < paNoError ) \
    { \
        PA_DEBUG(( "Expression '" #exp "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = paErr_; \
        goto error; \
    }

#define UNLESS(exp, code) \
    if( (exp) == 0 ) \
    { \
        PA_DEBUG(( "Expression '" #exp "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = (code); \
        goto error; \
    }

/* Implementation specific stream structure */
typedef struct PaAlsaStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    snd_pcm_t *pcm_capture;
    snd_pcm_t *pcm_playback;

    snd_pcm_uframes_t frames_per_period;
    snd_pcm_uframes_t playbackBufferSize;
    snd_pcm_uframes_t captureBufferSize;
    snd_pcm_format_t playbackNativeFormat;

    int capture_channels;
    int playback_channels;

    int capture_interleaved;    /* bool: is capture interleaved? */
    int playback_interleaved;   /* bool: is playback interleaved? */

    int callback_mode;          /* bool: are we running in callback mode? */
    int callback_finished;      /* bool: are we in the "callback finished" state? See if stream has been stopped in background */
    pthread_t callback_thread;

    /* the callback thread uses these to poll the sound device(s), waiting
     * for data to be ready/available */
    unsigned int capture_nfds;
    unsigned int playback_nfds;
    struct pollfd *pfds;
    int pollTimeout;

    /* these aren't really stream state, the callback uses them */
    snd_pcm_uframes_t capture_offset;
    snd_pcm_uframes_t playback_offset;

    int pcmsSynced;	            /* Have we successfully synced pcms */
    int callbackAbort;		    /* Drop frames? */
    int isActive;                   /* Is stream in active state? (Between StartStream and StopStream || !paContinue) */
    snd_pcm_uframes_t startThreshold;
    pthread_mutex_t stateMtx;      /* Used to synchronize access to stream state */
    pthread_mutex_t startMtx;      /* Used to synchronize stream start */
    pthread_cond_t startCond;      /* Wait untill audio is started in callback thread */

    /* Used by callback thread for underflow/overflow handling */
    snd_pcm_sframes_t playbackAvail;
    snd_pcm_sframes_t captureAvail;
    int neverDropInput;

    PaTime underrun;
    PaTime overrun;
}
PaAlsaStream;

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
void CleanUpStream( PaAlsaStream *stream );
int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate );
int GetExactSampleRate( snd_pcm_hw_params_t *hwParams, double *sampleRate );

/* Callback prototypes */
static void *CallbackThread( void *userData );

/* Blocking prototypes */
static signed long GetStreamReadAvailable( PaStream* s );
static signed long GetStreamWriteAvailable( PaStream* s );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, const void *buffer, unsigned long frames );


PaError PaAlsa_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    PaAlsaHostApiRepresentation *alsaHostApi = NULL;

    UNLESS( alsaHostApi = (PaAlsaHostApiRepresentation*) PaUtil_AllocateMemory(
                sizeof(PaAlsaHostApiRepresentation) ), paInsufficientMemory );
    UNLESS( alsaHostApi->allocations = PaUtil_CreateAllocationGroup(), paInsufficientMemory );

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
    UNLESS( !pthread_mutex_init( &gmtx, NULL ), paInternalError );   /* 0 == success */

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
    unsigned int uchans;

    assert( pcm );

    ENSURE( snd_pcm_nonblock( pcm, 0 ), paUnanticipatedHostError );

    snd_pcm_hw_params_alloca( &hwParams );
    snd_pcm_hw_params_any( pcm, hwParams );

    if (*defaultSampleRate != 0.)
    {
        /* Could be that the device opened in one mode supports samplerates that the other mode wont have,
         * so try again .. */
        if( SetApproximateSampleRate( pcm, hwParams, *defaultSampleRate ) < 0 )
        {
            *defaultSampleRate = 0.;
            PA_DEBUG(( "Original default samplerate failed, trying again ..\n" ));
        }
    }

    if( *defaultSampleRate == 0. )           /* Default sample rate not set */
    {
        unsigned int sampleRate = 44100;        /* Will contain approximate rate returned by alsa-lib */
        ENSURE( snd_pcm_hw_params_set_rate_near( pcm, hwParams, &sampleRate, NULL ), paUnanticipatedHostError );
        ENSURE( GetExactSampleRate( hwParams, defaultSampleRate ), paUnanticipatedHostError );
    }

    ENSURE( snd_pcm_hw_params_get_channels_max( hwParams, &uchans ), paUnanticipatedHostError );
    assert( uchans <= INT_MAX );
    *channels = (int) uchans;
    assert( *channels > 0 );    /* Weird linking issue could cause wrong version of ALSA symbols to be called,
                                   resulting in zeroed values */

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


/* Build PaDeviceInfo list, ignore devices for which we cannot determine capabilities (possibly busy, sigh) */
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
    int blocking = getenv("PA_INITIALIZE_BLOCK") ? 0 : SND_PCM_NONBLOCK;

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

    UNLESS( commonApi->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaDeviceInfo*) * deviceCount ), paInsufficientMemory );

    /* allocate all device info structs in a contiguous block */
    UNLESS( deviceInfoArray = (PaAlsaDeviceInfo*)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaAlsaDeviceInfo) * deviceCount ), paInsufficientMemory );

    /* now loop over the list of devices again, filling in the deviceInfo for each
     * A: If a device is deemed unavailable (can't get name), its not added to list of devices.
     */
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

        /* First of all, get name of card */
        snprintf( alsaDeviceName, 50, "hw:%d", card_idx );
        if( snd_ctl_open( &ctl, alsaDeviceName, 0 ) < 0 )
            continue;   /* Unable to open card :( */

        snd_ctl_card_info( ctl, card_info );
        snd_ctl_close( ctl );
        cardName = snd_ctl_card_info_get_name( card_info );

        /* Zero fields */
        memset( commonDeviceInfo, 0, sizeof (PaDeviceInfo) );

        UNLESS( deviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                        strlen(cardName) + 1 ), paInsufficientMemory );
        strcpy( deviceName, cardName );
        commonDeviceInfo->name = deviceName;

        deviceInfo->deviceNumber = card_idx;    /* ALSA device number */

        commonDeviceInfo->structVersion = 2;
        commonDeviceInfo->hostApi = alsaApi->hostApiIndex;

        /* to determine device capabilities, we must open the device and query the
         * hardware parameter configuration space */

        /* Query capture */
        if( snd_pcm_open( &pcm, alsaDeviceName, SND_PCM_STREAM_CAPTURE, blocking ) >= 0 )
        {
            if( GropeDevice( pcm, &commonDeviceInfo->maxInputChannels,
                        &commonDeviceInfo->defaultLowInputLatency, &commonDeviceInfo->defaultHighInputLatency,
                        &commonDeviceInfo->defaultSampleRate ) != paNoError )
                continue;   /* Error */
        }
                
        /* Query playback */
        if( snd_pcm_open( &pcm, alsaDeviceName, SND_PCM_STREAM_PLAYBACK, blocking ) >= 0 )
        {
            if( GropeDevice( pcm, &commonDeviceInfo->maxOutputChannels,
                        &commonDeviceInfo->defaultLowOutputLatency, &commonDeviceInfo->defaultHighOutputLatency,
                        &commonDeviceInfo->defaultSampleRate ) != paNoError )
                continue;   /* Error */
        }

        /* A: Storing pointer to PaAlsaDeviceInfo object as pointer to PaDeviceInfo object.
         * Should now be safe to add device info, unless the device supports neither capture nor playback
         */
        if( commonDeviceInfo->maxInputChannels || commonDeviceInfo->maxOutputChannels )
        {
            if( commonApi->info.defaultInputDevice == paNoDevice )
                commonApi->info.defaultInputDevice = device_idx;

            if( commonApi->info.defaultOutputDevice == paNoDevice )
                commonApi->info.defaultOutputDevice = device_idx;

            commonApi->deviceInfos[ device_idx++ ] = (PaDeviceInfo *) deviceInfo;
        }
    }

    commonApi->info.deviceCount = device_idx;   /* Number of successfully queried devices */

end:
    return result;

error:
    goto end;   /* No particular action */
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaAlsaHostApiRepresentation *alsaHostApi = (PaAlsaHostApiRepresentation*)hostApi;

    assert( hostApi );

    if( alsaHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( alsaHostApi->allocations );
        PaUtil_DestroyAllocationGroup( alsaHostApi->allocations );
    }

    PaUtil_FreeMemory( alsaHostApi );
    pthread_mutex_destroy( &gmtx );
}

typedef enum
{
    validateIn = 0,
    validateOut
} IOValidate;

/* Check against known device capabilities */
static PaError ValidateParameters( const PaStreamParameters *parameters, const PaAlsaDeviceInfo *deviceInfo, IOValidate io,
        const PaAlsaStreamInfo *streamInfo )
{
    int maxChans;

    assert( parameters );

    if( streamInfo )
    {
        if( streamInfo->size != sizeof (PaAlsaStreamInfo) || streamInfo->version != 1 )
            return paIncompatibleHostApiSpecificStreamInfo;
        if( parameters->device != paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;
    }
    if( parameters->device == paUseHostApiSpecificDeviceSpecification )
    {
        if( streamInfo )
            return paNoError;   /* Skip further checking */

        return paInvalidDevice;
    }

    maxChans = (io == validateIn ? deviceInfo->commonDeviceInfo.maxInputChannels :
        deviceInfo->commonDeviceInfo.maxOutputChannels);
    if( parameters->channelCount > maxChans )
    {
        return paInvalidChannelCount;
    }

    return paNoError;
}


/* Given an open stream, what sample formats are available? */

static PaSampleFormat GetAvailableFormats( snd_pcm_t *pcm )
{
    PaSampleFormat available = 0;
    snd_pcm_hw_params_t *hwParams;
    snd_pcm_hw_params_alloca( &hwParams );

    snd_pcm_hw_params_any( pcm, hwParams );

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT ) >= 0)
        available |= paFloat32;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S32 ) >= 0)
        available |= paInt32;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24 ) >= 0)
        available |= paInt24;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S16 ) >= 0)
        available |= paInt16;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U8 ) >= 0)
        available |= paUInt8;

    if( snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S8 ) >= 0)
        available |= paInt8;

    return available;
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


static PaError TestParameters( const PaStreamParameters *parameters, const char *deviceString, double sampleRate,
        snd_pcm_stream_t streamType )
{
    PaError result = paNoError;
    snd_pcm_t *pcm = NULL;
    PaSampleFormat availableFormats;
    PaSampleFormat paFormat;
    int ret;
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca( &params );

    if( (ret = snd_pcm_open( &pcm, deviceString, streamType, SND_PCM_NONBLOCK )) < 0 )
    {
        pcm = NULL;     /* Not to be closed */
        /* Take action based on return value */
        ENSURE( ret, ret == -EBUSY ? paDeviceUnavailable : paUnanticipatedHostError );
    }
    ENSURE( snd_pcm_nonblock( pcm, 0 ), paUnanticipatedHostError );

    snd_pcm_hw_params_any( pcm, params );

    ENSURE( SetApproximateSampleRate( pcm, params, sampleRate ), paInvalidSampleRate );
    ENSURE( snd_pcm_hw_params_set_channels( pcm, params, parameters->channelCount ), paInvalidChannelCount );

    /* See if we can find a best possible match */
    availableFormats = GetAvailableFormats( pcm );
    PA_ENSURE( paFormat = PaUtil_SelectClosestAvailableFormat( availableFormats, parameters->sampleFormat ) );

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
    PaError result = paFormatIsSupported;
    const PaAlsaDeviceInfo *inputDeviceInfo = NULL, *outputDeviceInfo = NULL;
    const PaAlsaStreamInfo *inputStreamInfo = NULL, *outputStreamInfo = NULL;

    if( inputParameters )
    {
        if( inputParameters->device != paUseHostApiSpecificDeviceSpecification )
        {
            assert( inputParameters->device < hostApi->info.deviceCount );
            inputDeviceInfo = (PaAlsaDeviceInfo *)hostApi->deviceInfos[ inputParameters->device ];
        }
        else
            inputStreamInfo = inputParameters->hostApiSpecificStreamInfo;

        PA_ENSURE( ValidateParameters( inputParameters, inputDeviceInfo, validateIn, inputStreamInfo ) );

        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
    }

    if( outputParameters )
    {
        if( outputParameters->device != paUseHostApiSpecificDeviceSpecification )
        {
            assert( outputParameters->device < hostApi->info.deviceCount );
            outputDeviceInfo = (PaAlsaDeviceInfo *)hostApi->deviceInfos[ outputParameters->device ];
        }
        else
            outputStreamInfo = outputParameters->hostApiSpecificStreamInfo;

        PA_ENSURE( ValidateParameters( outputParameters, outputDeviceInfo, validateOut, outputStreamInfo ) );

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
        char *deviceName = alloca( 50 );

        if( !inputStreamInfo )
            snprintf( deviceName, 50, "hw:%d", inputDeviceInfo->deviceNumber );
        else
            deviceName = (char *) inputStreamInfo->deviceString;

        PA_ENSURE( TestParameters( inputParameters, deviceName, sampleRate, SND_PCM_STREAM_CAPTURE ) );
    }

    if ( outputChannelCount )
    {
        char *deviceName = alloca( 50 );

        if( !outputStreamInfo )
            snprintf( deviceName, 50, "hw:%d", outputDeviceInfo->deviceNumber );
        else
            deviceName = (char *) outputStreamInfo->deviceString;

        PA_ENSURE( TestParameters( outputParameters, deviceName, sampleRate, SND_PCM_STREAM_PLAYBACK ) );
    }

    return paFormatIsSupported;

error:
    return result;
}


/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

static PaError ConfigureStream( snd_pcm_t *pcm, int channels, int *interleaved, double *sampleRate,
                                PaSampleFormat paFormat, unsigned long framesPerBuffer, snd_pcm_uframes_t
                                *bufferSize, PaTime *latency, int primeBuffers, int callbackMode )
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
    unsigned int numPeriods;

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
    assert( alsaFormat != SND_PCM_FORMAT_UNKNOWN );
    ENSURE( snd_pcm_hw_params_set_format( pcm, hwParams, alsaFormat ), paUnanticipatedHostError );

    /* ... set the sample rate */
    ENSURE( SetApproximateSampleRate( pcm, hwParams, *sampleRate ), paInvalidSampleRate );
    ENSURE( GetExactSampleRate( hwParams, sampleRate ), paUnanticipatedHostError );

    /* ... set the number of channels */
    ENSURE( snd_pcm_hw_params_set_channels( pcm, hwParams, channels ), paInvalidChannelCount );

    /* Set buffer size */
    ENSURE( snd_pcm_hw_params_set_periods_integer( pcm, hwParams ), paUnanticipatedHostError );
    ENSURE( snd_pcm_hw_params_set_period_size_integer( pcm, hwParams ), paUnanticipatedHostError );
    ENSURE( snd_pcm_hw_params_set_period_size( pcm, hwParams, framesPerBuffer, 0 ), paUnanticipatedHostError );

    /* Find an acceptable number of periods */
    numPeriods = (*latency * *sampleRate) / framesPerBuffer + 1;
    numPeriods = MAX( numPeriods, 2 );  /* Should be at least 2 periods I think? */
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
    ENSURE( snd_pcm_hw_params_get_buffer_size( hwParams, bufferSize ), paUnanticipatedHostError );

    /* Latency in seconds, one period is not counted as latency */
    *latency = (numPeriods - 1) * framesPerBuffer / *sampleRate;

    /* Now software parameters... */
    ENSURE( snd_pcm_sw_params_current( pcm, swParams ), paUnanticipatedHostError );

    ENSURE( snd_pcm_sw_params_set_start_threshold( pcm, swParams, framesPerBuffer ), paUnanticipatedHostError );
    ENSURE( snd_pcm_sw_params_set_stop_threshold( pcm, swParams, *bufferSize ), paUnanticipatedHostError );

    /* Silence buffer in the case of underrun */
    if( !primeBuffers )
    {
        ENSURE( snd_pcm_sw_params_set_silence_threshold( pcm, swParams, 0 ), paUnanticipatedHostError );
        ENSURE( snd_pcm_sw_params_set_silence_size( pcm, swParams, INT_MAX ), paUnanticipatedHostError );
    }
        
    ENSURE( snd_pcm_sw_params_set_avail_min( pcm, swParams, framesPerBuffer ), paUnanticipatedHostError );
    ENSURE( snd_pcm_sw_params_set_xfer_align( pcm, swParams, 1 ), paUnanticipatedHostError );
    ENSURE( snd_pcm_sw_params_set_tstamp_mode( pcm, swParams, SND_PCM_TSTAMP_MMAP ), paUnanticipatedHostError );

    /* Set the parameters! */
    ENSURE( snd_pcm_sw_params( pcm, swParams ), paUnanticipatedHostError );

end:
    return result;

error:
    goto end;   /* No particular action */
}

static void InitializeStream( PaAlsaStream *stream, int callback, PaStreamFlags streamFlags )
{
    assert( stream );

    stream->pcm_capture = NULL;
    stream->pcm_playback = NULL;
    stream->callback_finished = 0;
    stream->callback_mode = callback;
    stream->capture_nfds = 0;
    stream->playback_nfds = 0;
    stream->pfds = NULL;
    stream->pollTimeout = 0;
    stream->pcmsSynced = 0;
    stream->callbackAbort = 0;
    stream->isActive = 0;
    stream->startThreshold = 0;
    pthread_mutex_init( &stream->stateMtx, NULL );
    pthread_mutex_init( &stream->startMtx, NULL );
    pthread_cond_init( &stream->startCond, NULL );
    stream->neverDropInput = 0;
    stream->underrun = stream->overrun = 0.0;
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
    const PaAlsaDeviceInfo *inputDeviceInfo = 0, *outputDeviceInfo = 0;
    PaAlsaStream *stream = 0;
    PaSampleFormat hostInputSampleFormat = 0, hostOutputSampleFormat = 0;
    int numInputChannels = 0, numOutputChannels = 0;
    PaSampleFormat inputSampleFormat = 0, outputSampleFormat = 0;
    unsigned long framesPerHostBuffer = framesPerBuffer;
    PaAlsaStreamInfo *inputStreamInfo = NULL, *outputStreamInfo = NULL;
    PaTime inputLatency, outputLatency;

    if( inputParameters )
    {
        if( inputParameters->device != paUseHostApiSpecificDeviceSpecification )
        {
            assert( inputParameters->device < hostApi->info.deviceCount );
            inputDeviceInfo = (PaAlsaDeviceInfo*)hostApi->deviceInfos[ inputParameters->device ];
        }
        else
            inputStreamInfo = inputParameters->hostApiSpecificStreamInfo;

        PA_ENSURE( ValidateParameters( inputParameters, inputDeviceInfo, validateIn, inputStreamInfo ) );

        numInputChannels = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
    }

    if( outputParameters )
    {
        if( outputParameters->device != paUseHostApiSpecificDeviceSpecification )
        {
            assert( outputParameters->device < hostApi->info.deviceCount );
            outputDeviceInfo = (PaAlsaDeviceInfo*)hostApi->deviceInfos[ outputParameters->device ];
        }
        else
            outputStreamInfo = outputParameters->hostApiSpecificStreamInfo;

        PA_ENSURE( ValidateParameters( outputParameters, outputDeviceInfo, validateOut, outputStreamInfo ) );

        /*
        outputDeviceInfo = (PaAlsaDeviceInfo*)hostApi->deviceInfos[ outputParameters->device ];
        PA_ENSURE( ValidateParameters( outputParameters, outputDeviceInfo->commonDeviceInfo.maxOutputChannels ) );
        */

        numOutputChannels = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
    }

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */

    /* allocate and do basic initialization of the stream structure */

    UNLESS( stream = (PaAlsaStream*)PaUtil_AllocateMemory( sizeof(PaAlsaStream) ), paInsufficientMemory );
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
        char *deviceName = alloca( 50 );

        if( !inputStreamInfo )
            snprintf( deviceName, 50, "hw:%d", inputDeviceInfo->deviceNumber );
        else
            deviceName = (char *) inputStreamInfo->deviceString;

        if( (ret = snd_pcm_open( &stream->pcm_capture, deviceName, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK )) < 0 )
        {
            stream->pcm_capture = NULL;     /* Not to be closed */
            ENSURE( ret, ret == -EBUSY ? paDeviceUnavailable : paBadIODeviceCombination );
        }
        ENSURE( snd_pcm_nonblock( stream->pcm_capture, 0 ), paUnanticipatedHostError );

        stream->capture_nfds = snd_pcm_poll_descriptors_count( stream->pcm_capture );

        hostInputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats( stream->pcm_capture ),
                                                 inputSampleFormat );
    }

    if( numOutputChannels > 0 )
    {
        int ret;
        char *deviceName = alloca( 50 );

        if( !outputStreamInfo )
            snprintf( deviceName, 50, "hw:%d", outputDeviceInfo->deviceNumber );
        else
            deviceName = (char *) outputStreamInfo->deviceString;

        if( (ret = snd_pcm_open( &stream->pcm_playback, deviceName, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK )) < 0 )
        {
            stream->pcm_playback = NULL;     /* Not to be closed */
            ENSURE( ret, ret == -EBUSY ? paDeviceUnavailable : paBadIODeviceCombination );
        }
        ENSURE( snd_pcm_nonblock( stream->pcm_playback, 0 ), paUnanticipatedHostError );

        stream->playback_nfds = snd_pcm_poll_descriptors_count( stream->pcm_playback );

        hostOutputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats( stream->pcm_playback ),
                                                 outputSampleFormat );
        stream->playbackNativeFormat = Pa2AlsaFormat( hostOutputSampleFormat );
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
                snd_pcm_uframes_t desiredLatency, e;
                snd_pcm_uframes_t minPeriodSize, minPlayback, minCapture, maxPeriodSize, maxPlayback, maxCapture,
                    optimalPeriodSize, periodSize;
                int dir;

                snd_pcm_t *pcm;
                snd_pcm_hw_params_t *hwParamsPlayback, *hwParamsCapture;

                snd_pcm_hw_params_alloca( &hwParamsPlayback );
                snd_pcm_hw_params_alloca( &hwParamsCapture );

                /* Come up with a common desired latency */
                pcm = stream->pcm_playback;
                snd_pcm_hw_params_any( pcm, hwParamsPlayback );
                ENSURE( SetApproximateSampleRate( pcm, hwParamsPlayback, sampleRate ), paBadIODeviceCombination );
                ENSURE( snd_pcm_hw_params_set_channels( pcm, hwParamsPlayback, outputParameters->channelCount ),
                        paBadIODeviceCombination );

                ENSURE( snd_pcm_hw_params_set_period_size_integer( pcm, hwParamsPlayback ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_set_periods_integer( pcm, hwParamsPlayback ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_get_period_size_min( hwParamsPlayback, &minPlayback, &dir ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_get_period_size_max( hwParamsPlayback, &maxPlayback, &dir ), paUnanticipatedHostError );

                pcm = stream->pcm_capture;
                ENSURE( snd_pcm_hw_params_any( pcm, hwParamsCapture ), paUnanticipatedHostError );
                ENSURE( SetApproximateSampleRate( pcm, hwParamsCapture, sampleRate ), paBadIODeviceCombination );
                ENSURE( snd_pcm_hw_params_set_channels( pcm, hwParamsCapture, inputParameters->channelCount ),
                        paBadIODeviceCombination );

                ENSURE( snd_pcm_hw_params_set_period_size_integer( pcm, hwParamsCapture ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_set_periods_integer( pcm, hwParamsCapture ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_get_period_size_min( hwParamsCapture, &minCapture, &dir ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_get_period_size_max( hwParamsCapture, &maxCapture, &dir ), paUnanticipatedHostError );

                minPeriodSize = MAX( minPlayback, minCapture );
                maxPeriodSize = MIN( maxPlayback, maxCapture );

                desiredLatency = (snd_pcm_uframes_t) (MIN( outputParameters->suggestedLatency, inputParameters->suggestedLatency )
                    * sampleRate);
                /* Clamp desiredLatency */
                {
                    snd_pcm_uframes_t tmp, maxBufferSize = ULONG_MAX;
                    ENSURE( snd_pcm_hw_params_get_buffer_size_max( hwParamsPlayback, &tmp ), paUnanticipatedHostError );
                    maxBufferSize = MIN( maxBufferSize, tmp );
                    ENSURE( snd_pcm_hw_params_get_buffer_size_max( hwParamsCapture, &tmp ), paUnanticipatedHostError );
                    maxBufferSize = MIN( maxBufferSize, tmp );

                    desiredLatency = MIN( desiredLatency, maxBufferSize );
                }

                /* Find the closest power of 2 */
                e = ilogb( minPeriodSize );
                if( minPeriodSize & (minPeriodSize - 1) )
                    e += 1;

                periodSize = (snd_pcm_uframes_t) pow( 2, e );
                while( periodSize <= maxPeriodSize )
                {
                    if( snd_pcm_hw_params_test_period_size( stream->pcm_playback, hwParamsPlayback, periodSize, 0 ) >= 0 &&
                            snd_pcm_hw_params_test_period_size( stream->pcm_capture, hwParamsCapture, periodSize, 0 ) >= 0 )
                        break;  /* Ok! */

                    periodSize *= 2;
                }

                /* 4 periods considered optimal */
                optimalPeriodSize = MAX( desiredLatency / 4, minPeriodSize );
                optimalPeriodSize = MIN( optimalPeriodSize, maxPeriodSize );

                /* Find the closest power of 2 */
                e = ilogb( optimalPeriodSize );
                if( optimalPeriodSize & (optimalPeriodSize - 1) )
                    e += 1;

                optimalPeriodSize = (snd_pcm_uframes_t) pow( 2, e );
                while( optimalPeriodSize >= periodSize )
                {
                    pcm = stream->pcm_playback;
                    if( snd_pcm_hw_params_test_period_size( pcm, hwParamsPlayback, optimalPeriodSize, 0 ) < 0 )
                        continue;

                    pcm = stream->pcm_capture;
                    if( snd_pcm_hw_params_test_period_size( pcm, hwParamsCapture, optimalPeriodSize, 0 ) >= 0 )
                        break;

                    optimalPeriodSize /= 2;
                }

                if( optimalPeriodSize >= periodSize )
                    periodSize = optimalPeriodSize;

                if( periodSize <= maxPeriodSize )
                {
                    /* Looks good */
                    framesPerHostBuffer = periodSize;
                }
                else    /* XXX: Some more descriptive error code might be appropriate */
                    PA_ENSURE( paBadIODeviceCombination );
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

        inputLatency = inputParameters->suggestedLatency; /* Real latency in seconds returned from ConfigureStream */
        PA_ENSURE( ConfigureStream( stream->pcm_capture, numInputChannels, &interleaved,
                             &sampleRate, plain_format, framesPerHostBuffer, &stream->captureBufferSize,
                             &inputLatency, 0, stream->callback_mode ) );

        stream->capture_interleaved = interleaved;
    }

    if( numOutputChannels > 0 )
    {
        int interleaved = !(outputSampleFormat & paNonInterleaved);
        PaSampleFormat plain_format = hostOutputSampleFormat & ~paNonInterleaved;
        int primeBuffers = streamFlags & paPrimeOutputBuffersUsingStreamCallback;

        outputLatency = outputParameters->suggestedLatency; /* Real latency in seconds returned from ConfigureStream */

        PA_ENSURE( ConfigureStream( stream->pcm_playback, numOutputChannels, &interleaved,
                             &sampleRate, plain_format, framesPerHostBuffer, &stream->playbackBufferSize,
                             &outputLatency, primeBuffers, stream->callback_mode ) );

        /* If the user wants to prime the buffer before stream start, the start threshold will equal buffer size */
        if( primeBuffers )
            stream->startThreshold = stream->playbackBufferSize;
        stream->playback_interleaved = interleaved;
    }
    /* Should be exact now */
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    PA_ENSURE( PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
                    numInputChannels, inputSampleFormat, hostInputSampleFormat,
                    numOutputChannels, outputSampleFormat, hostOutputSampleFormat,
                    sampleRate, streamFlags, framesPerBuffer, framesPerHostBuffer,
                    paUtilFixedHostBufferSize, callback, userData ) );

    /* Ok, buffer processor is initialized, now we can deduce it's latency */
    if( numInputChannels > 0 )
        stream->streamRepresentation.streamInfo.inputLatency = inputLatency + PaUtil_GetBufferProcessorInputLatency(
                &stream->bufferProcessor );
    if( numOutputChannels > 0 )
        stream->streamRepresentation.streamInfo.outputLatency = outputLatency + PaUtil_GetBufferProcessorOutputLatency(
                &stream->bufferProcessor );

    /* this will cause the two streams to automatically start/stop/prepare in sync.
     * We only need to execute these operations on one of the pair.
     * A: We don't want to do this on a blocking stream.
     */
    if( stream->callback_mode && stream->pcm_capture && stream->pcm_playback && 
            snd_pcm_link( stream->pcm_capture, stream->pcm_playback ) >= 0 )
            stream->pcmsSynced = 1;

    UNLESS( stream->pfds = (struct pollfd*)PaUtil_AllocateMemory( (stream->capture_nfds +
                    stream->playback_nfds) * sizeof(struct pollfd) ), paInsufficientMemory );

    stream->frames_per_period = framesPerHostBuffer;
    stream->capture_channels = numInputChannels;
    stream->playback_channels = numOutputChannels;
    stream->pollTimeout = (int) ceil( 1000 * stream->frames_per_period/sampleRate );    /* Period in msecs, rounded up */

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

static void SilenceBuffer( PaAlsaStream *stream )
{
    const snd_pcm_channel_area_t *areas;
    snd_pcm_uframes_t frames = snd_pcm_avail_update( stream->pcm_playback );

    snd_pcm_mmap_begin( stream->pcm_playback, &areas, &stream->playback_offset, &frames );
    snd_pcm_areas_silence( areas, stream->playback_offset, stream->playback_channels, frames, stream->playbackNativeFormat );
    snd_pcm_mmap_commit( stream->pcm_playback, stream->playback_offset, frames );
}

/*! \brief Start/prepare pcm(s) for streaming
 *
 * Depending on wether the stream is in callback or blocking mode, we will respectively start or simply
 * prepare the playback pcm. If the buffer has _not_ been primed, we will in callback mode prepare and
 * silence the buffer before starting playback. In blocking mode we simply prepare, as the playback will
 * be started automatically as the user writes to output. 
 *
 * The capture pcm, however, will simply be prepared and started.
 *
 * PaAlsaStream::startMtx makes sure access is synchronized (useful in callback mode)
 */
static PaError AlsaStart( PaAlsaStream *stream, int priming )
{
    PaError result = paNoError;

    pthread_mutex_lock( &stream->startMtx );
    if( stream->pcm_playback )
    {
        if( stream->callback_mode )
        {
            /* We're not priming buffer, so prepare and silence */
            if( !priming )
            {
                ENSURE( snd_pcm_prepare( stream->pcm_playback ), paUnanticipatedHostError );
                SilenceBuffer( stream );
            }
            ENSURE( snd_pcm_start( stream->pcm_playback ), paUnanticipatedHostError );
        }
        else
            ENSURE( snd_pcm_prepare( stream->pcm_playback ), paUnanticipatedHostError );
    }
    if( stream->pcm_capture && !stream->pcmsSynced )
    {
        ENSURE( snd_pcm_prepare( stream->pcm_capture ), paUnanticipatedHostError );
        /* We want to start capture for a blocking stream as well, since nothing will happen otherwise */
        ENSURE( snd_pcm_start( stream->pcm_capture ), paUnanticipatedHostError );
    }

end:
    pthread_mutex_unlock( &stream->startMtx );
    return result;
error:
    goto end;
}

/*! \brief Utility function for determining if pcms are in running state
 * */
static int IsRunning( PaAlsaStream *stream )
{
    int result = 0;

    pthread_mutex_lock( &stream->stateMtx ); /* Synchronize access to pcm state */
    if( stream->pcm_capture )
    {
        snd_pcm_state_t capture_state = snd_pcm_state( stream->pcm_capture );

        if( capture_state == SND_PCM_STATE_RUNNING || capture_state == SND_PCM_STATE_XRUN
                || capture_state == SND_PCM_STATE_DRAINING )
        {
            result = 1;
            goto end;
        }
    }

    if( stream->pcm_playback )
    {
        snd_pcm_state_t playback_state = snd_pcm_state( stream->pcm_playback );

        if( playback_state == SND_PCM_STATE_RUNNING || playback_state == SND_PCM_STATE_XRUN
                || playback_state == SND_PCM_STATE_DRAINING )
        {
            result = 1;
            goto end;
        }
    }

end:
    pthread_mutex_unlock( &stream->stateMtx );
    return result;
}

static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* Ready the processor */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    if( stream->callback_mode )
    {
        int res = 0;
        PaTime pt = PaUtil_GetTime();
        struct timespec ts;

        pthread_mutex_lock( &stream->startMtx );
        ENSURE( pthread_create( &stream->callback_thread, NULL, &CallbackThread, stream ), paInternalError );

        /*! Wait for stream to be started */
        ts.tv_sec = (__time_t) pt + 1;
        ts.tv_nsec = (long) pt * 1000000000;

        while( !IsRunning( stream ) && res != ETIMEDOUT )
            res = pthread_cond_timedwait( &stream->startCond, &stream->startMtx, &ts );
        pthread_mutex_unlock( &stream->startMtx );
        PA_DEBUG(( "Waited for %g seconds for stream to start\n", PaUtil_GetTime() - pt ));

        if( res == ETIMEDOUT )
        {
            pthread_cancel( stream->callback_thread );
            pthread_join( stream->callback_thread, NULL );
            PA_ENSURE( paTimedOut );
        }
    }
    else
        PA_ENSURE( AlsaStart( stream, 0 ) );

    stream->isActive = 1;

end:
    return result;
error:
    goto end;
}

static PaError AlsaStop( PaAlsaStream *stream, int abort )
{
    PaError result = paNoError;

    if( abort )
    {
        if( stream->pcm_playback )
            ENSURE( snd_pcm_drop( stream->pcm_playback ), paUnanticipatedHostError );
        if( stream->pcm_capture && !stream->pcmsSynced )
            ENSURE( snd_pcm_drop( stream->pcm_capture ), paUnanticipatedHostError );

        PA_DEBUG(( "Dropped frames\n" ));
    }
    else
    {
        if( stream->pcm_playback )
            ENSURE( snd_pcm_drain( stream->pcm_playback ), paUnanticipatedHostError );
        if( stream->pcm_capture && !stream->pcmsSynced )
            ENSURE( snd_pcm_drain( stream->pcm_capture ), paUnanticipatedHostError );
    }

end:
    return result;
error:
    goto end;
}

/*! \brief Stop or abort stream
 *
 * If a stream is in callback mode we will have to inspect wether the background thread has
 * finished, or we will have to take it out. In either case we join the thread before
 * returning. In blocking mode, we simply tell ALSA to stop abruptly (abort) or finish
 * buffers (drain)
 *
 * Stream will be considered inactive (!PaAlsaStream::isActive) after a call to this function
 */
static PaError RealStop( PaStream *s, int abort )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* These two are used to obtain return value when joining thread */
    void *pret;
    int retVal;

    /* First deal with the callback thread, cancelling and/or joining
     * it if necessary
     */
    if( stream->callback_mode )
    {
        if( stream->callback_finished )
        {
            pthread_join( stream->callback_thread, &pret );  /* Just wait for it to die */
            
            if( pret )  /* Message from dying thread */
            {
                retVal = *(int *) pret;
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
        PA_ENSURE( AlsaStop( stream, abort ) );
    }

    stream->isActive = 0;

end:
    return result;

error:
    goto end;
}

static PaError StopStream( PaStream *s )
{
    ((PaAlsaStream *) s)->callbackAbort = 0;    /* In case abort has been called earlier */
    return RealStop( s, 0);
}

static PaError AbortStream( PaStream *s )
{
    ((PaAlsaStream *) s)->callbackAbort = 1;
    return RealStop( s, 1);
}

/*!
 * The stream is considered stopped before StartStream, or AFTER a call to Abort/StopStream (callback
 * returning !paContinue is not considered)
 */
static PaError IsStreamStopped( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream *)s;
    PaError res;

    /* callback_finished indicates we need to join callback thread (ie. in Abort/StopStream) */
    res = !IsStreamActive(s) && !stream->callback_finished;

    return res;
}

static PaError IsStreamActive( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    return stream->isActive;

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
    PA_DEBUG(( "Time in secs: %d\n", timestamp.tv_sec ));

    return timestamp.tv_sec + (PaTime) timestamp.tv_usec/1000000;
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
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

    PaUtil_FreeMemory( stream->pfds );
    pthread_mutex_destroy( &stream->stateMtx );
    pthread_mutex_destroy( &stream->startMtx );
    pthread_cond_destroy( &stream->startCond );

    PaUtil_FreeMemory( stream );
}

int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate )
{
    unsigned long approx = (unsigned long) sampleRate;
    int dir = 0;
    double fraction = sampleRate - approx;

    assert( pcm && hwParams );

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


/* Utility functions for blocking/callback interfaces */

/* Atomic restart of stream (we don't want the intermediate state visible) */
static PaError AlsaRestart( PaAlsaStream *stream )
{
    PaError result = paNoError;

    PA_DEBUG(( "Restarting audio\n" ));

    pthread_mutex_lock( &stream->stateMtx );
    PA_ENSURE( AlsaStop( stream, 0 ) );
    PA_ENSURE( AlsaStart( stream, 0 ) );

    PA_DEBUG(( "Restarted audio\n" ));

end:
    pthread_mutex_unlock( &stream->stateMtx );
    return result;
error:
   goto end;
}

static PaError HandleXrun( PaAlsaStream *stream )
{
    PaError result = paNoError;
    snd_pcm_status_t *st;
    PaTime now = PaUtil_GetTime();
    snd_timestamp_t t;

    snd_pcm_status_alloca( &st );

    if( stream->pcm_playback )
    {
        snd_pcm_status( stream->pcm_playback, st );
        if( snd_pcm_status_get_state( st ) == SND_PCM_STATE_XRUN )
        {
            snd_pcm_status_get_trigger_tstamp( st, &t );
            stream->underrun = now * 1000 - ((PaTime) t.tv_sec * 1000 + (PaTime) t.tv_usec / 1000);
        }
    }
    if( stream->pcm_capture )
    {
        snd_pcm_status( stream->pcm_capture, st );
        if( snd_pcm_status_get_state( st ) == SND_PCM_STATE_XRUN )
        {
            snd_pcm_status_get_trigger_tstamp( st, &t );
            stream->overrun = now * 1000 - ((PaTime) t.tv_sec * 1000 + (PaTime) t.tv_usec / 1000);
        }
    }

    PA_ENSURE( AlsaRestart( stream ) );

end:
    return result;
error:
    goto end;
}

/*!
  \brief Poll on I/O filedescriptors

  Poll till we've determined there's data for read or write. In the full-duplex case,
  we don't want to hang around forever waiting for either input or output frames, so
  whenever we have a timed out filedescriptor we check if we're nearing under/overrun
  for the other pcm (critical limit set at one buffer). If so, we exit the waiting state,
  and go on with what we got.
  */
static PaError Wait( PaAlsaStream *stream, snd_pcm_uframes_t *frames )
{
    PaError result = paNoError;
    int pollPlayback = 0, pollCapture = 0;
    snd_pcm_sframes_t captureAvail = INT_MAX, playbackAvail = INT_MAX, commonAvail;
    int xrun = 0;   /* Under/overrun? */

    assert( stream && frames && stream->pollTimeout > 0 );

    if( stream->pcm_capture )
        pollCapture = 1;

    if( stream->pcm_playback )
        pollPlayback = 1;

    while( pollPlayback || pollCapture )
    {
	unsigned short revents;
        int totalFds = 0;
        int ofs = 0;

        /* get the fds, packing all applicable fds into a single array,
         * so we can check them all with a single poll() call 
         */
        if( stream->pcm_capture && pollCapture )
        {
            snd_pcm_poll_descriptors( stream->pcm_capture, stream->pfds, stream->capture_nfds );
            ofs += stream->capture_nfds;
            totalFds += stream->capture_nfds;
        }
        if( stream->pcm_playback && pollPlayback )
        {
            snd_pcm_poll_descriptors( stream->pcm_playback, stream->pfds + ofs, stream->playback_nfds );
            totalFds += stream->playback_nfds;
        }

        /* if the main thread has requested that we stop, do so now */
        pthread_testcancel();

        /* now poll on the combination of playback and capture fds. */
        if( poll( stream->pfds, totalFds, stream->pollTimeout ) < 0 )
        {
            /* GDB
            if( errno == EINTR ) {
                continue;
            }
            */

            PA_ENSURE( paInternalError );
        }

        pthread_testcancel();

        /* check the return status of our pfds */
        if( pollCapture )
        {
            ENSURE( snd_pcm_poll_descriptors_revents( stream->pcm_capture, stream->pfds,
                        stream->capture_nfds, &revents ), paUnanticipatedHostError );
            if( revents )
            {
                if( revents & POLLERR )
                    xrun = 1;

                pollCapture = 0;
            }
            else     /* Timed out, go on with playback? */
                if( stream->pcm_playback )
                {
                    if( snd_pcm_avail_update( stream->pcm_playback ) >= stream->playbackBufferSize - stream->frames_per_period )
                    {
                        PA_DEBUG(( "Capture timed out, pollTimeOut: %d\n", stream->pollTimeout ));
                        pollCapture = 0;    /* Go on without me .. *sob* ... */
                    }
                }
        }

        if( pollPlayback )
        {
            unsigned short revents;
            ENSURE( snd_pcm_poll_descriptors_revents( stream->pcm_playback, stream->pfds +
                        stream->capture_nfds, stream->playback_nfds, &revents ), paUnanticipatedHostError );
            if( revents )
            {
                if( revents & POLLERR )
                    xrun = 1;

                pollPlayback = 0;
            }
            else    /* Timed out, go on with capture? */
                if( stream->pcm_capture )
                {
                    PA_DEBUG(( "Playback timed out\n" ));
                    if( snd_pcm_avail_update( stream->pcm_capture ) >= stream->captureBufferSize - stream->frames_per_period )
                        pollPlayback = 0;   /* Go on without me, son .. */
                }
        }
    }

    /* we have now established that there are buffers ready to be
     * operated on.  Now determine how many frames are available.
     */
    if( stream->pcm_capture )
    {
        if( (captureAvail = snd_pcm_avail_update( stream->pcm_capture )) == -EPIPE )
            xrun = 1;
        else
            ENSURE( captureAvail, paUnanticipatedHostError );

        if( !captureAvail )
            PA_DEBUG(( "Wait: captureAvail: 0\n" ));

        captureAvail = captureAvail == 0 ? INT_MAX : captureAvail;      /* Disregard if zero */
    }

    if( stream->pcm_playback )
    {
        if( (playbackAvail = snd_pcm_avail_update( stream->pcm_playback )) == -EPIPE )
            xrun = 1;
        else
            ENSURE( playbackAvail, paUnanticipatedHostError );

        if( !playbackAvail )
            PA_DEBUG(( "Wait: playbackAvail: 0\n" ));

        playbackAvail = playbackAvail == 0 ? INT_MAX : playbackAvail;   /* Disregard if 0 */
    }
    
    assert( !(captureAvail == playbackAvail == INT_MAX) );

    commonAvail = MIN( captureAvail, playbackAvail );
    commonAvail -= commonAvail % stream->frames_per_period;

    if( xrun )
    {
        HandleXrun( stream );
        commonAvail = 0;    /* Wait will be called again, to obtain the number of available frames */
    }

    assert( commonAvail >= 0 );
    *frames = commonAvail;

end:
    return result;
error:
    goto end;
}

/* Extract buffer from channel area */
static unsigned char *ExtractAddress( const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset )
{
    return (unsigned char *) area->addr + (area->first + offset * area->step) / 8;
}

/* Get buffers from ALSA for read/write, and determine the amount of frames available.
   Underflow/underflow complicates matters
*/
static PaError SetUpBuffers( PaAlsaStream *stream, snd_pcm_uframes_t requested, int alignFrames,
        snd_pcm_uframes_t *frames )
{
    PaError result = paNoError;
    int i;
    snd_pcm_uframes_t captureFrames = requested, playbackFrames = requested, commonFrames;
    const snd_pcm_channel_area_t *areas, *area;
    unsigned char *buffer;

    assert( stream && frames );

    if( stream->pcm_capture )
    {
        ENSURE( snd_pcm_mmap_begin( stream->pcm_capture, &areas, &stream->capture_offset, &captureFrames ),
                paUnanticipatedHostError );

        if( stream->capture_interleaved )
        {
            buffer = ExtractAddress( areas, stream->capture_offset );
            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor,
                                                 0, /* starting at channel 0 */
                                                 buffer,
                                                 0  /* default numInputChannels */
                                               );
        }
        else
            /* noninterleaved */
            for( i = 0; i < stream->capture_channels; ++i )
            {
                area = &areas[i];
                buffer = ExtractAddress( area, stream->capture_offset );
                PaUtil_SetNonInterleavedInputChannel( &stream->bufferProcessor,
                                                      i,
                                                      buffer );
            }
    }

    if( stream->pcm_playback )
    {
        ENSURE( snd_pcm_mmap_begin( stream->pcm_playback, &areas, &stream->playback_offset, &playbackFrames ),
                paUnanticipatedHostError );

        if( stream->playback_interleaved )
        {
            buffer = ExtractAddress( areas, stream->playback_offset );
            PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                                                 0, /* starting at channel 0 */
                                                 buffer,
                                                 0  /* default numInputChannels */
                                               );
        }
        else
            /* noninterleaved */
            for( i = 0; i < stream->playback_channels; ++i )
            {
                area = &areas[i];
                buffer = ExtractAddress( area, stream->playback_offset );
                PaUtil_SetNonInterleavedOutputChannel( &stream->bufferProcessor,
                                                      i,
                                                      buffer );
            }
    }

    if( alignFrames )
    {
        playbackFrames -= (playbackFrames % stream->frames_per_period);
        captureFrames -= (captureFrames % stream->frames_per_period);
    }
    commonFrames = MIN( captureFrames, playbackFrames );

    if( stream->pcm_playback && stream->pcm_capture )
    {
        /* Full-duplex, but we are starved for data in either end
           If we're out of input, go on. Input buffer will be zeroed.
           In the case of output underflow, drop input frames unless stream->neverDropInput.
           If we're starved for output, while keeping input, we'll discard output samples.
       */
        if( !commonFrames ) 
        {
            if( !captureFrames )    /* Input underflow */
                commonFrames = playbackFrames;  /* We still want output */
            else                    /* Output underflow */
                if( stream->neverDropInput )    /* Output underflow, but do not drop input */
                    commonFrames = captureFrames;
        }
        else    /* Safe to commit commonFrames for both */
            playbackFrames = captureFrames = commonFrames;
    }
    
    /* Inform PortAudio of the number of frames we got.
       We might be experiencing underflow in either end; if its an input underflow, we go on
       with output. If its output underflow however, depending on the paNeverDropInput flag,
       we may want to simply discard the excess input or call the callback with
       paOutputOverflow flagged.
    */
    if( stream->pcm_capture )
    {
        if( captureFrames || !commonFrames )    /* Either we have input, or we have neither */
            PaUtil_SetInputFrameCount( &stream->bufferProcessor, commonFrames );
        else    /* We have input underflow */
            PaUtil_SetNoInput( &stream->bufferProcessor );
    }
    if( stream->pcm_playback )
    {
        if( playbackFrames || !commonFrames )   /* Either we have output, or we have neither */
            PaUtil_SetOutputFrameCount( &stream->bufferProcessor, commonFrames );
        else    /* We have output underflow, but keeping input data (paNeverDropInput) */
        {
            /*
            PaUtil_SetNoOutput( &stream->bufferProcessor );
            */
        }
    }

    /* PA_DEBUG(( "SetUpBuffers: captureAvail: %d, playbackAvail: %d, commonFrames: %d\n\n", captureFrames, playbackFrames, commonFrames )); */
    stream->playbackAvail = playbackFrames;
    stream->captureAvail = captureFrames;

    *frames = commonFrames;

end:
    return result;
error:
    goto end;
}

/* Callback interface */

static void OnExit( void *data )
{
    PaAlsaStream *stream = (PaAlsaStream *) data;

    assert( data );

    stream->callback_finished = 1;  /* Let the outside world know stream was stopped in callback */
    AlsaStop( stream, stream->callbackAbort );
    stream->callbackAbort = 0;      /* Clear state */
    
    PA_DEBUG(( "Stoppage\n" ));

    /* Eventually notify user all buffers have played */
    if( stream->streamRepresentation.streamFinishedCallback )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    stream->isActive = 0;
}

void *CallbackThread( void *userData )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*) userData;
    snd_pcm_uframes_t framesAvail, framesGot, framesProcessed;
    snd_pcm_sframes_t startThreshold = stream->startThreshold;
    snd_pcm_status_t *capture_status, *playback_status;
    int *pres;

    assert( userData );

    /* Allocation should happen here, once per iteration is no good */
    snd_pcm_status_alloca( &capture_status );
    snd_pcm_status_alloca( &playback_status );

    pthread_cleanup_push( &OnExit, stream );	/* Execute OnExit when exiting */

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, 44100.0 );

    if( startThreshold <= 0 )
    {
        PA_ENSURE( AlsaStart( stream, 0 ) );    /* Buffer will be zeroed */
        pthread_mutex_lock( &stream->startMtx);
        pthread_cond_signal( &stream->startCond );
        pthread_mutex_unlock( &stream->startMtx);
    }
    else /* Priming output? Prepare first */
    {
        if( stream->pcm_playback )
            ENSURE( snd_pcm_prepare( stream->pcm_playback ), paUnanticipatedHostError );
        if( stream->pcm_capture && !stream->pcmsSynced )
            ENSURE( snd_pcm_prepare( stream->pcm_capture ), paUnanticipatedHostError );
    }

    while( 1 )
    {
        PaError callbackResult;
	PaStreamCallbackTimeInfo timeInfo = {0,0,0};
        PaStreamCallbackFlags cbFlags = 0;

        pthread_testcancel();

        {
            /* calculate time info */
            snd_timestamp_t capture_timestamp, playback_timestamp;
            PaTime capture_time, playback_time;

            if( stream->pcm_capture )
            {
                snd_pcm_sframes_t capture_delay;

                snd_pcm_status( stream->pcm_capture, capture_status );
                snd_pcm_status_get_tstamp( capture_status, &capture_timestamp );

                capture_time = capture_timestamp.tv_sec +
                                     ((PaTime)capture_timestamp.tv_usec/1000000);
                timeInfo.currentTime = capture_time;

                capture_delay = snd_pcm_status_get_delay( capture_status );
                timeInfo.inputBufferAdcTime = timeInfo.currentTime -
                    (PaTime)capture_delay / stream->streamRepresentation.streamInfo.sampleRate;
            }
            if( stream->pcm_playback )
            {
                snd_pcm_sframes_t playback_delay;

                snd_pcm_status( stream->pcm_playback, playback_status );
                snd_pcm_status_get_tstamp( playback_status, &playback_timestamp );

                playback_time = playback_timestamp.tv_sec +
                                     ((PaTime)playback_timestamp.tv_usec/1000000);

                if( stream->pcm_capture ) /* Full duplex */
                {
                    /* Hmm, we have both a playback and a capture timestamp.
                     * Hopefully they are the same... */
                    if( fabs( capture_time - playback_time ) > 0.01 )
                        PA_DEBUG(("Capture time and playback time differ by %f\n", fabs(capture_time-playback_time)));
                }
                else
                    timeInfo.currentTime = playback_time;

                playback_delay = snd_pcm_status_get_delay( playback_status );
                timeInfo.outputBufferDacTime = timeInfo.currentTime +
                    (PaTime)playback_delay / stream->streamRepresentation.streamInfo.sampleRate;
            }
        }

        /* Set callback flags *after* one of these has been detected */
        if( stream->underrun != 0.0 )
        {
            cbFlags |= paOutputUnderflow;
            stream->underrun = 0.0;
        }
        if( stream->overrun != 0.0 )
        {
            cbFlags |= paInputOverflow;
            stream->overrun = 0.0;
        }

        PA_ENSURE( Wait( stream, &framesAvail ) );
        while( framesAvail > 0 )
        {
            pthread_testcancel();

            /* Priming output */
            if( startThreshold > 0 )
            {
                PA_DEBUG(( "Priming\n" ));
                cbFlags |= paPrimingOutput;
                framesAvail = MIN( framesAvail, startThreshold );
            }

            /* now we know the soundcard is ready to produce/receive at least
             * one period.  we just need to get the buffers for the client
             * to read/write. */
            PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, cbFlags );

            PA_ENSURE( SetUpBuffers( stream, framesAvail, 1, &framesGot ) );
            /* Check for underflow/overflow */
            if( stream->pcm_playback && stream->pcm_capture )
            {
                if( !stream->captureAvail )
                {
                    cbFlags |= paInputUnderflow;
                    PA_DEBUG(( "Input underflow\n" ));
                }
                if( !stream->playbackAvail )
                {
                    if( !framesGot )    /* The normal case, dropping input */
                    {
                        cbFlags |= paInputOverflow;
                        PA_DEBUG(( "Input overflow\n" ));
                    }
                    else                /* Keeping input (paNeverDropInput) */
                    {
                        cbFlags |= paOutputOverflow;
                        PA_DEBUG(( "Output overflow\n" ));
                    }
                }
            }

            PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

            callbackResult = paContinue;

            /* this calls the callback */
            framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                                          &callbackResult );
            PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

            /* Inform ALSA how many frames we read/wrote
               Now, this number may differ between capture and playback, due to under/overflow.
               If we're dropping input frames, we effectively sink them here.
             */
            if( stream->pcm_capture )
            {
                ENSURE( snd_pcm_mmap_commit( stream->pcm_capture, stream->capture_offset, stream->captureAvail ),
                        paUnanticipatedHostError );
            }
            if( stream->pcm_playback )
            {
                ENSURE( snd_pcm_mmap_commit( stream->pcm_playback, stream->playback_offset, stream->playbackAvail ),
                        paUnanticipatedHostError );
            }

            /* If threshold for starting stream specified (priming buffer), decrement and compare */
            if( startThreshold > 0 )
            {
                if( (startThreshold -= framesGot) <= 0 )
                {
                    PA_ENSURE( AlsaStart( stream, 1 ) );    /* Buffer will be zeroed */
                    pthread_mutex_lock( &stream->startMtx);
                    pthread_cond_signal( &stream->startCond );
                    pthread_mutex_unlock( &stream->startMtx);
                }
            }

            if( callbackResult != paContinue )
                break;

            framesAvail -= framesGot;
        }


        /*
            If you need to byte swap outputBuffer, you can do it here using
            routines in pa_byteswappers.h
        */

        if( callbackResult != paContinue )
        {
            stream->callbackAbort = (callbackResult == paAbort);
            goto end;
            
        }
    }

    /* This code is unreachable, but important to include regardless because it
     * is possibly a macro with a closing brace to match the opening brace in
     * pthread_cleanup_push() above.  The documentation states that they must
     * always occur in pairs. */
    pthread_cleanup_pop( 1 );

end:
    pthread_exit( NULL );

error:
    /* Pass on error code */
    pres = malloc( sizeof (int) );
    *pres = (result);
    
    pthread_exit( pres );
}

/* Blocking interface */

static PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    PaError result = paNoError;
    signed long err;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    snd_pcm_uframes_t framesGot, framesAvail;
    void *userBuffer;
    int i;
    snd_pcm_t *save;

    assert( stream );

    /* Disregard playback */
    save =  stream->pcm_playback;
    stream->pcm_playback = NULL;

    if( !stream->pcm_capture )
        PA_ENSURE( paCanNotReadFromAnOutputOnlyStream );


    if( stream->overrun )
    {
        result = paInputOverflowed;
        stream->overrun = 0.0;
    }

    if( stream->bufferProcessor.userInputIsInterleaved )
        userBuffer = buffer;
    else /* Copy channels into local array */
    {
        int numBytes = sizeof (void *) * stream->capture_channels;
        UNLESS( userBuffer = alloca( numBytes ), paInsufficientMemory );
        for( i = 0; i < stream->capture_channels; ++i )
            ((const void **) userBuffer)[i] = ((const void **) buffer)[i];
    }

    /* Start stream if in prepared state */
    if( snd_pcm_state( stream->pcm_capture ) == SND_PCM_STATE_PREPARED )
    {
        ENSURE( snd_pcm_start( stream->pcm_capture ), paUnanticipatedHostError );
    }

    while( frames > 0 )
    {
        if( (err = GetStreamReadAvailable( stream )) == paInputOverflowed )
            err = 0;    /* Wait will detect the (unlikely) xrun, and restart capture */
        PA_ENSURE( err );
        framesAvail = (snd_pcm_uframes_t) err;

        if( framesAvail == 0 )
            PA_ENSURE( Wait( stream, &framesAvail ) );
        framesAvail = MIN( framesAvail, frames );

        PA_ENSURE( SetUpBuffers( stream, framesAvail, 0, &framesGot ) );
        framesGot = PaUtil_CopyInput( &stream->bufferProcessor, &userBuffer, framesGot );
        ENSURE( snd_pcm_mmap_commit( stream->pcm_capture, stream->capture_offset, framesGot ),
                paUnanticipatedHostError );

        frames -= framesGot;
    }

end:
    stream->pcm_playback = save;
    return result;
error:
    goto end;
}

static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaError result = paNoError;
    signed long err;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    snd_pcm_uframes_t framesGot, framesAvail;
    const void *userBuffer;
    int i;
    snd_pcm_t *save;
    
    /* Disregard capture */
    save = stream->pcm_capture;
    stream->pcm_capture = NULL;

    assert( stream );
    if( !stream->pcm_playback )
        PA_ENSURE( paCanNotWriteToAnInputOnlyStream );

    if( stream->underrun )
    {
        result = paOutputUnderflowed;
        stream->underrun = 0.0;
    }

    if( stream->bufferProcessor.userOutputIsInterleaved )
        userBuffer = buffer;
    else /* Copy channels into local array */
    {
        int numBytes = sizeof (void *) * stream->playback_channels;
        UNLESS( userBuffer = alloca( numBytes ), paInsufficientMemory );
        for( i = 0; i < stream->playback_channels; ++i )
            ((const void **) userBuffer)[i] = ((const void **) buffer)[i];
    }

    while( frames > 0 )
    {
        snd_pcm_uframes_t hwAvail;

        PA_ENSURE( err = GetStreamWriteAvailable( stream ) );
        framesAvail = err;
        if( framesAvail == 0 )
            PA_ENSURE( Wait( stream, &framesAvail ) );
        framesAvail = MIN( framesAvail, frames );

        PA_ENSURE( SetUpBuffers( stream, framesAvail, 0, &framesGot ) );
        framesGot = PaUtil_CopyOutput( &stream->bufferProcessor, &userBuffer, framesGot );
        ENSURE( snd_pcm_mmap_commit( stream->pcm_playback, stream->playback_offset, framesGot ),
                paUnanticipatedHostError );

        frames -= framesGot;

        /* Frames residing in buffer */
        PA_ENSURE( err = GetStreamWriteAvailable( stream ) );
        framesAvail = err;
        hwAvail = stream->playbackBufferSize - framesAvail;

        /* Start stream after one period of samples worth */
        if( snd_pcm_state( stream->pcm_playback ) == SND_PCM_STATE_PREPARED &&
            hwAvail >= stream->frames_per_period )
        {
            ENSURE( snd_pcm_start( stream->pcm_playback ), paUnanticipatedHostError );
        }
    }

end:
    stream->pcm_capture = save;
    return result;
error:
    goto end;
}


/* Return frames available for reading. In the event of an overflow, the capture pcm will be restarted */
static signed long GetStreamReadAvailable( PaStream* s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    snd_pcm_sframes_t avail = snd_pcm_avail_update( stream->pcm_capture );

    if( avail < 0 )
    {
        if( avail == -EPIPE )
        {
            PA_ENSURE( HandleXrun( stream ) );
            avail = snd_pcm_avail_update( stream->pcm_capture );
        }

        if( avail == -EPIPE )
            PA_ENSURE( paInputOverflowed );
        ENSURE( avail, paUnanticipatedHostError );
    }

    return avail;

error:
    return result;
}


/* Return frames available for writing. In the event of an underflow, the playback pcm will be prepared */
static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    snd_pcm_sframes_t avail = snd_pcm_avail_update( stream->pcm_playback );

    if( avail < 0 )
    {
        if( avail == -EPIPE )
        {
            PA_ENSURE( HandleXrun( stream ) );
            avail = snd_pcm_avail_update( stream->pcm_playback );
        }

        /* avail should not contain -EPIPE now, since HandleXrun will only prepare the pcm */
        ENSURE( avail, paUnanticipatedHostError );
    }

    return avail;

error:
    return result;
}

/* Extensions */

/* Initialize host api specific structure */
void PaAlsa_InitializeStreamInfo( PaAlsaStreamInfo *info )
{
    info->size = sizeof (PaAlsaStreamInfo);
    info->hostApiType = paALSA;
    info->version = 1;
}
