/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 * JACK Implementation by Joshua Haberman
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

#include <string.h>
#include <regex.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>  /* EBUSY */
#include <signal.h> /* sig_atomic_t */

#include <jack/types.h>
#include <jack/jack.h>

#include "pa_util.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_process.h"
#include "pa_allocation.h"
#include "pa_cpuload.h"

static int aErr_;
static PaError paErr_;     /* For use with ENSURE_PA */

#define STRINGIZE_HELPER(expr) #expr
#define STRINGIZE(expr) STRINGIZE_HELPER(expr)

/* Check PaError */
#define ENSURE_PA(expr) \
    if( (paErr_ = (expr)) < paNoError ) \
    { \
        PaUtil_DebugPrint(( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = paErr_; \
        goto error; \
    }

#define UNLESS(expr, code) \
    if( (expr) == 0 ) \
    { \
        PaUtil_DebugPrint(( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = (code); \
        goto error; \
    }

#define ASSERT_CALL(expr, success) \
    aErr_ = (expr); \
    assert( aErr_ == success );

/*
 * Functions that directly map to the PortAudio stream interface
 */

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
                           PaStreamCallback *streamCallback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
/*static PaTime GetStreamInputLatency( PaStream *stream );*/
/*static PaTime GetStreamOutputLatency( PaStream *stream );*/
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );


/*
 * Data specific to this API
 */

struct PaJackStream;

typedef struct
{
    PaUtilHostApiRepresentation commonHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;

    PaUtilAllocationGroup *deviceInfoMemory;

    jack_client_t *jack_client;
    PaHostApiIndex hostApiIndex;

    pthread_mutex_t mtx;
    pthread_cond_t cond;
    unsigned long inputBase, outputBase;

    /* For dealing with the process thread */
    volatile int xrun;     /* Received xrun notification from JACK? */
    struct PaJackStream * volatile toAdd, * volatile toRemove;
    struct PaJackStream *processQueue;
    volatile sig_atomic_t jackIsDown;
}
PaJackHostApiRepresentation;

/* PaJackStream - a stream data structure specifically for this implementation */

typedef struct PaJackStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilBufferProcessor bufferProcessor;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaJackHostApiRepresentation *hostApi;

    /* our input and output ports */
    jack_port_t **local_input_ports;
    jack_port_t **local_output_ports;

    /* the input and output ports of the client we are connecting to */
    jack_port_t **remote_input_ports;
    jack_port_t **remote_output_ports;

    int num_incoming_connections;
    int num_outgoing_connections;

    jack_client_t *jack_client;

    /* The stream is running if it's still producing samples.
     * The stream is active if samples it produced are still being heard.
     */
    volatile sig_atomic_t is_running;
    volatile sig_atomic_t is_active;
    /* Used to signal processing thread that stream should start or stop, respectively */
    volatile sig_atomic_t doStart, doStop, doAbort;
    int callbackResult;
    int xrun;

    jack_nframes_t t0;

    PaUtilAllocationGroup *stream_memory;

    struct PaJackStream *next;
}
PaJackStream;

#define MAX_CLIENTS 100
#define TRUE 1
#define FALSE 0

/*
 * Functions specific to this API
 */

static int JackCallback( jack_nframes_t frames, void *userData );


/*
 *
 * Implementation
 *
 */


/* BuildDeviceList():
 *
 * The process of determining a list of PortAudio "devices" from
 * JACK's client/port system is fairly involved, so it is separated
 * into its own routine.
 */

static PaError BuildDeviceList( PaJackHostApiRepresentation *jackApi )
{
    /* Utility macros for the repetitive process of allocating memory */

    /* ... MALLOC: allocate memory as part of the device list
     * allocation group */
#define MALLOC(size) \
     (PaUtil_GroupAllocateMemory( jackApi->deviceInfoMemory, (size) ))

    /* JACK has no concept of a device.  To JACK, there are clients
     * which have an arbitrary number of ports.  To make this
     * intelligible to PortAudio clients, we will group each JACK client
     * into a device, and make each port of that client a channel */

    PaError result = paNoError;
    PaUtilHostApiRepresentation *commonApi = &jackApi->commonHostApiRep;

    const char **jack_ports = NULL;
    char *client_names[MAX_CLIENTS];
    char *regex_pattern = alloca( jack_client_name_size() + 3 );
    int num_clients = 0;
    int port_index, client_index, i;
    double globalSampleRate;
    regex_t port_regex;

    commonApi->info.defaultInputDevice = paNoDevice;
    commonApi->info.defaultOutputDevice = paNoDevice;
    commonApi->info.deviceCount = 0;

    /* Parse the list of ports, using a regex to grab the client names */
    ASSERT_CALL( regcomp( &port_regex, "^[^:]*", REG_EXTENDED ), 0 );

    /* since we are rebuilding the list of devices, free all memory
     * associated with the previous list */
    PaUtil_FreeAllAllocations( jackApi->deviceInfoMemory );

    /* We can only retrieve the list of clients indirectly, by first
     * asking for a list of all ports, then parsing the port names
     * according to the client_name:port_name convention (which is
     * enforced by jackd)
     * A: If jack_get_ports returns NULL, there's nothing for us to do */
    UNLESS( (jack_ports = jack_get_ports( jackApi->jack_client, "", "", 0 )) && jack_ports[0], paNoError );

    /* Build a list of clients from the list of ports */
    for( port_index = 0; jack_ports[port_index] != NULL; port_index++ )
    {
        int client_seen;
        regmatch_t match_info;
        char *tmp_client_name = alloca( jack_client_name_size() );
        const char *port = jack_ports[port_index];

        /* extract the client name from the port name, using a regex
         * that parses the clientname:portname syntax */
        UNLESS( !regexec( &port_regex, port, 1, &match_info, 0 ), paInternalError );
        assert(match_info.rm_eo - match_info.rm_so < jack_client_name_size());
        memcpy( tmp_client_name, port + match_info.rm_so,
                match_info.rm_eo - match_info.rm_so );
        tmp_client_name[match_info.rm_eo - match_info.rm_so] = '\0';

        /* do we know about this port's client yet? */
        client_seen = FALSE;
        for( i = 0; i < num_clients; i++ )
            if( strcmp( tmp_client_name, client_names[i] ) == 0 )
                client_seen = TRUE;

        if (client_seen)
            continue;   /* A: Nothing to see here, move along */

        UNLESS( client_names[num_clients] = (char*)MALLOC(strlen(tmp_client_name) + 1), paInsufficientMemory );

        /* The alsa_pcm client should go in spot 0.  If this
         * is the alsa_pcm client AND we are NOT about to put
         * it in spot 0 put it in spot 0 and move whatever
         * was already in spot 0 to the end. */
        if( strcmp( "alsa_pcm", tmp_client_name ) == 0 && num_clients > 0 )
        {
            /* alsa_pcm goes in spot 0 */
            strcpy( client_names[ num_clients ], client_names[0] );
            strcpy( client_names[0], tmp_client_name );
        }
        else
        {
            /* put the new client at the end of the client list */
            strcpy( client_names[ num_clients ], tmp_client_name );
        }
        ++num_clients;
    }

    /* Now we have a list of clients, which will become the list of
     * PortAudio devices. */

    /* there is one global sample rate all clients must conform to */

    globalSampleRate = jack_get_sample_rate( jackApi->jack_client );
    UNLESS( commonApi->deviceInfos = (PaDeviceInfo**)MALLOC( sizeof(PaDeviceInfo*) *
                                                     num_clients ), paInsufficientMemory );

    assert( commonApi->info.deviceCount == 0 );

    /* Create a PaDeviceInfo structure for every client */
    for( client_index = 0; client_index < num_clients; client_index++ )
    {
        PaDeviceInfo *curDevInfo;
        const char **jack_ports = NULL; /* Local definition, for easier cleanup */

        UNLESS( curDevInfo = (PaDeviceInfo*)MALLOC( sizeof(PaDeviceInfo) ), paInsufficientMemory );
        UNLESS( curDevInfo->name = (char*)MALLOC( strlen(client_names[client_index]) + 1 ), paInsufficientMemory );
        strcpy( (char *)curDevInfo->name, client_names[client_index] );

        curDevInfo->structVersion = 2;
        curDevInfo->hostApi = jackApi->hostApiIndex;

        /* JACK is very inflexible: there is one sample rate the whole
         * system must run at, and all clients must speak IEEE float. */
        curDevInfo->defaultSampleRate = globalSampleRate;

        /* To determine how many input and output channels are available,
         * we re-query jackd with more specific parameters. */

        sprintf( regex_pattern, "%s:.*", client_names[client_index] );

        /* ... what are your output ports (that we could input from)? */
        jack_ports = jack_get_ports( jackApi->jack_client, regex_pattern,
                                     NULL, JackPortIsOutput);
        curDevInfo->maxInputChannels = 0;
        curDevInfo->defaultLowInputLatency = 0.;
        curDevInfo->defaultHighInputLatency = 0.;
        if( jack_ports )
        {
            jack_port_t *p = jack_port_by_name( jackApi->jack_client, jack_ports[0] );
            curDevInfo->defaultLowInputLatency = curDevInfo->defaultHighInputLatency =
                jack_port_get_latency( p ) / globalSampleRate;
            free( p );

            for( i = 0; jack_ports[i] != NULL ; i++)
            {
                /* The number of ports returned is the number of output channels.
                 * We don't care what they are, we just care how many */
                curDevInfo->maxInputChannels++;
            }
            free(jack_ports);
        }

        /* ... what are your input ports (that we could output to)? */
        jack_ports = jack_get_ports( jackApi->jack_client, regex_pattern,
                                     NULL, JackPortIsInput);
        curDevInfo->maxOutputChannels = 0;
        curDevInfo->defaultLowOutputLatency = 0.;
        curDevInfo->defaultHighOutputLatency = 0.;
        if( jack_ports )
        {
            jack_port_t *p = jack_port_by_name( jackApi->jack_client, jack_ports[0] );
            curDevInfo->defaultLowOutputLatency = curDevInfo->defaultHighOutputLatency =
                jack_port_get_latency( p ) / globalSampleRate;
            free( p );

            for( i = 0; jack_ports[i] != NULL ; i++)
            {
                /* The number of ports returned is the number of input channels.
                 * We don't care what they are, we just care how many */
                curDevInfo->maxOutputChannels++;
            }
            free(jack_ports);
        }

        /* Add this client to the list of devices */
        commonApi->deviceInfos[client_index] = curDevInfo;
        ++commonApi->info.deviceCount;
        if( commonApi->info.defaultInputDevice == paNoDevice && curDevInfo->maxInputChannels > 0 )
            commonApi->info.defaultInputDevice = client_index;
        if( commonApi->info.defaultOutputDevice == paNoDevice && curDevInfo->maxOutputChannels > 0 )
            commonApi->info.defaultOutputDevice = client_index;
    }

error:
    regfree( &port_regex );
    free( jack_ports );
    return paNoError;
}
#undef MALLOC

static void UpdateSampleRate( PaJackStream *stream, double sampleRate )
{
    /* XXX: Maybe not the cleanest way of going about this? */
    stream->cpuLoadMeasurer.samplingPeriod = stream->bufferProcessor.samplePeriod = 1. / sampleRate;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;
}

static void JackOnShutdown( void *arg )
{
    PaJackHostApiRepresentation *jackApi = (PaJackHostApiRepresentation *)arg;
    PaJackStream *stream = jackApi->processQueue;

    PA_DEBUG(( "%s: JACK server is shutting down\n", __FUNCTION__ ));
    for( ; stream; stream = stream->next )
    {
        stream->is_active = 0;
    }

    /* Make sure that the main thread doesn't get stuck waiting on the condition */
    ASSERT_CALL( pthread_mutex_lock( &jackApi->mtx ), 0 );
    jackApi->jackIsDown = 1;
    ASSERT_CALL( pthread_cond_signal( &jackApi->cond ), 0 );
    ASSERT_CALL( pthread_mutex_unlock( &jackApi->mtx ), 0 );

}
static int JackSrCb( jack_nframes_t nframes, void *arg )
{
    PaJackHostApiRepresentation *jackApi = (PaJackHostApiRepresentation *)arg;
    double sampleRate = (double)nframes;
    PaJackStream *stream = jackApi->processQueue;

    /* Update all streams in process queue */
    PA_DEBUG(( "%s: Acting on change in JACK samplerate: %f\n", __FUNCTION__, sampleRate ));
    for( ; stream; stream = stream->next )
    {
        if( stream->streamRepresentation.streamInfo.sampleRate != sampleRate )
        {
            PA_DEBUG(( "%s: Updating samplerate\n", __FUNCTION__ ));
            UpdateSampleRate( stream, sampleRate );
        }
    }

    return 0;
}
static int JackXRunCb(void *arg) {
    PaJackHostApiRepresentation *hostApi = (PaJackHostApiRepresentation *)arg;
    assert( hostApi );
    hostApi->xrun = TRUE;
    PA_DEBUG(( "%s: JACK signalled xrun\n", __FUNCTION__ ));
    return 0;
}

PaError PaJack_Initialize( PaUtilHostApiRepresentation **hostApi,
                           PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    PaJackHostApiRepresentation *jackHostApi;
    int activated = 0;
    char *clientName;
    *hostApi = NULL;    /* Initialize to NULL */

    UNLESS( jackHostApi = (PaJackHostApiRepresentation*)
        PaUtil_AllocateMemory( sizeof(PaJackHostApiRepresentation) ), paInsufficientMemory );
    jackHostApi->deviceInfoMemory = NULL;

    ASSERT_CALL( pthread_mutex_init( &jackHostApi->mtx, NULL ), 0 );
    ASSERT_CALL( pthread_cond_init( &jackHostApi->cond, NULL ), 0 );

    /* Try to become a client of the JACK server.  If we cannot do
     * this, then this API cannot be used. */

    clientName = alloca( jack_client_name_size() );
    snprintf( clientName, jack_client_name_size(), "PortAudio-%d", getpid() );
    jackHostApi->jack_client = jack_client_new( clientName );
    if( jackHostApi->jack_client == NULL )
    {
       /* the V19 development docs say that if an implementation
        * detects that it cannot be used, it should return a NULL
        * interface and paNoError */
       result = paNoError;
       goto error;
    }

    UNLESS( jackHostApi->deviceInfoMemory = PaUtil_CreateAllocationGroup(), paInsufficientMemory );
    jackHostApi->hostApiIndex = hostApiIndex;

    *hostApi = &jackHostApi->commonHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paJACK;
    (*hostApi)->info.name = "JACK Audio Connection Kit";

    /* Build a device list by querying the JACK server */

    ENSURE_PA( BuildDeviceList( jackHostApi ) );

    /* Register functions */

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &jackHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    jackHostApi->inputBase = jackHostApi->outputBase = 0;
    jackHostApi->xrun = 0;
    jackHostApi->toAdd = jackHostApi->toRemove = NULL;
    jackHostApi->processQueue = NULL;
    jackHostApi->jackIsDown = 0;

    jack_on_shutdown( jackHostApi->jack_client, JackOnShutdown, jackHostApi );
    UNLESS( !jack_set_sample_rate_callback( jackHostApi->jack_client, JackSrCb, jackHostApi ), paUnanticipatedHostError );
    UNLESS( !jack_set_xrun_callback( jackHostApi->jack_client, JackXRunCb, jackHostApi ), paUnanticipatedHostError );
    UNLESS( !jack_set_process_callback( jackHostApi->jack_client, JackCallback, jackHostApi ), paUnanticipatedHostError );
    UNLESS( !jack_activate( jackHostApi->jack_client ), paUnanticipatedHostError );
    activated = 1;

    return result;

error:
    if( activated )
        ASSERT_CALL( jack_deactivate( jackHostApi->jack_client ), 0 );

    if( jackHostApi )
    {
        if( jackHostApi->jack_client )
            ASSERT_CALL( jack_client_close( jackHostApi->jack_client ), 0 );

        if( jackHostApi->deviceInfoMemory )
        {
            PaUtil_FreeAllAllocations( jackHostApi->deviceInfoMemory );
            PaUtil_DestroyAllocationGroup( jackHostApi->deviceInfoMemory );
        }

        PaUtil_FreeMemory( jackHostApi );
    }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaJackHostApiRepresentation *jackHostApi = (PaJackHostApiRepresentation*)hostApi;

    /* note: this automatically disconnects all ports, since a deactivated
     * client is not allowed to have any ports connected */
    ASSERT_CALL( jack_deactivate( jackHostApi->jack_client ), 0 );

    ASSERT_CALL( pthread_mutex_destroy( &jackHostApi->mtx ), 0 );
    ASSERT_CALL( pthread_cond_destroy( &jackHostApi->cond ), 0 );

    ASSERT_CALL( jack_client_close( jackHostApi->jack_client ), 0 );

    if( jackHostApi->deviceInfoMemory )
    {
        PaUtil_FreeAllAllocations( jackHostApi->deviceInfoMemory );
        PaUtil_DestroyAllocationGroup( jackHostApi->deviceInfoMemory );
    }

    PaUtil_FreeMemory( jackHostApi );
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount = 0, outputChannelCount = 0;
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
        The following check is not necessary for JACK.
        
            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported


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

    /* check that the device supports sampleRate */
    
#define ABS(x) ( (x) > 0 ? (x) : -(x) )
    if( ABS(sampleRate - jack_get_sample_rate(((PaJackHostApiRepresentation *) hostApi)->jack_client )) > 1 )
       return paInvalidSampleRate;
#undef ABS

    return paFormatIsSupported;
}

/* Basic stream initialization */
static PaError InitializeStream( PaJackStream *stream, PaJackHostApiRepresentation *hostApi, int numInputChannels,
        int numOutputChannels )
{
    PaError result = paNoError;

    memset( stream, 0, sizeof (PaJackStream) );
    UNLESS( stream->stream_memory = PaUtil_CreateAllocationGroup(), paInsufficientMemory );
    stream->jack_client = hostApi->jack_client;
    stream->hostApi = hostApi;

    if( numInputChannels )
    {
        UNLESS( stream->local_input_ports =
                (jack_port_t**) PaUtil_GroupAllocateMemory( stream->stream_memory, sizeof(jack_port_t*) * numInputChannels ),
                paInsufficientMemory );
        memset( stream->local_input_ports, 0, sizeof(jack_port_t*) * numInputChannels );
        UNLESS( stream->remote_output_ports =
                (jack_port_t**) PaUtil_GroupAllocateMemory( stream->stream_memory, sizeof(jack_port_t*) * numInputChannels ),
                paInsufficientMemory );
        memset( stream->remote_output_ports, 0, sizeof(jack_port_t*) * numInputChannels );
    }
    if( numOutputChannels )
    {
        UNLESS( stream->local_output_ports =
                (jack_port_t**) PaUtil_GroupAllocateMemory( stream->stream_memory, sizeof(jack_port_t*) * numOutputChannels ),
                paInsufficientMemory );
        memset( stream->local_output_ports, 0, sizeof(jack_port_t*) * numOutputChannels );
        UNLESS( stream->remote_input_ports =
                (jack_port_t**) PaUtil_GroupAllocateMemory( stream->stream_memory, sizeof(jack_port_t*) * numOutputChannels ),
                paInsufficientMemory );
        memset( stream->remote_input_ports, 0, sizeof(jack_port_t*) * numOutputChannels );
    }

    stream->num_incoming_connections = numInputChannels;
    stream->num_outgoing_connections = numOutputChannels;

error:
    return result;
}

/*!
 * Free resources associated with stream, and eventually stream itself.
 *
 * Frees allocated memory, and closes opened pcms.
 */
static void CleanUpStream( PaJackStream *stream, int terminateStreamRepresentation, int terminateBufferProcessor )
{
    int i;
    assert( stream );

    for( i = 0; i < stream->num_incoming_connections; ++i )
    {
        if( stream->local_input_ports[i] )
            ASSERT_CALL( jack_port_unregister( stream->jack_client, stream->local_input_ports[i] ), 0 );
        free( stream->remote_output_ports[i] );
    }
    for( i = 0; i < stream->num_outgoing_connections; ++i )
    {
        if( stream->local_output_ports[i] )
            ASSERT_CALL( jack_port_unregister(stream->jack_client, stream->local_output_ports[i] ), 0 );
        free( stream->remote_input_ports[i] );
    }

    if( terminateStreamRepresentation )
        PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    if( terminateBufferProcessor )
        PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );

    if( stream->stream_memory )
    {
        PaUtil_FreeAllAllocations( stream->stream_memory );
        PaUtil_DestroyAllocationGroup( stream->stream_memory );
    }
    PaUtil_FreeMemory( stream );
}

static PaError AddStream( PaJackStream *stream )
{
    PaError result = paNoError;
    PaJackHostApiRepresentation *hostApi = stream->hostApi;
    /* Add to queue over streams that should be processed */
    ASSERT_CALL( pthread_mutex_lock( &hostApi->mtx ), 0 );
    if( !hostApi->jackIsDown )
    {
        hostApi->toAdd = stream;
        /* Unlock mutex and await signal from processing thread */
        ASSERT_CALL( pthread_cond_wait( &hostApi->cond, &hostApi->mtx ), 0 );
    }
    ASSERT_CALL( pthread_mutex_unlock( &hostApi->mtx ), 0 );

    if( hostApi->jackIsDown )
        return paDeviceUnavailable;

    return result;
}

/* Remove stream from processing queue */
static PaError RemoveStream( PaJackStream *stream )
{
    PaError result = paNoError;
    PaJackHostApiRepresentation *hostApi = stream->hostApi;

    /* Add to queue over streams that should be processed */
    ASSERT_CALL( pthread_mutex_lock( &hostApi->mtx ), 0 );
    if( !hostApi->jackIsDown )
    {
        hostApi->toRemove = stream;
        /* Unlock mutex and await signal from processing thread */
        ASSERT_CALL( pthread_cond_wait( &hostApi->cond, &hostApi->mtx ), 0 );
    }
    ASSERT_CALL( pthread_mutex_unlock( &hostApi->mtx ), 0 );

    return result;
}

/* Add stream to processing queue */
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData )
{
    PaError result = paNoError;
    PaJackHostApiRepresentation *jackHostApi = (PaJackHostApiRepresentation*)hostApi;
    PaJackStream *stream = NULL;
    char *port_string = alloca( jack_port_name_size() );
    unsigned long regexSz = jack_client_name_size() + 3;
    char *regex_pattern = alloca( regexSz );
    const char **jack_ports = NULL;
    /* int jack_max_buffer_size = jack_get_buffer_size( jackHostApi->jack_client ); */
    int i;
    int inputChannelCount, outputChannelCount;
    const double jackSr = jack_get_sample_rate( jackHostApi->jack_client );
    PaSampleFormat inputSampleFormat = 0, outputSampleFormat = 0;
    int bpInitialized = 0, srInitialized = 0;   /* Initialized buffer processor and stream representation? */
    unsigned long off;

    if( !streamCallback )
    {
        /* we do not support blocking I/O */
        return paNullCallback;      /* A: Is this correct? */
    }
    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */
    if( (streamFlags & paPrimeOutputBuffersUsingStreamCallback) != 0 )
        return paInvalidFlag;   /* This implementation does not support buffer priming */

    if( framesPerBuffer != paFramesPerBufferUnspecified )
    {
        /* Jack operates with power of two buffers, and we don't support non-integer buffer adaption (yet) */
        /*UNLESS( !(framesPerBuffer & (framesPerBuffer - 1)), paBufferTooBig );*/  /* TODO: Add descriptive error code? */
    }

    /* Preliminary checks */

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

    /* ... check that the sample rate exactly matches the ONE acceptable rate
     * A: This rate isn't necessarily constant though? */

#define ABS(x) ( (x) > 0 ? (x) : -(x) )
    if( ABS(sampleRate - jackSr) > 1 )
       return paInvalidSampleRate;
#undef ABS

    UNLESS( stream = (PaJackStream*)PaUtil_AllocateMemory( sizeof(PaJackStream) ), paInsufficientMemory );
    ENSURE_PA( InitializeStream( stream, jackHostApi, inputChannelCount, outputChannelCount ) );

    PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
            &jackHostApi->callbackStreamInterface, streamCallback, userData );
    srInitialized = 1;
    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, jackSr );

    /* create the JACK ports.  We cannot connect them until audio
     * processing begins */

    /* Register a unique set of ports for this stream
     * TODO: Robust allocation of new port names */

    off = jackHostApi->inputBase;
    for( i = 0; i < inputChannelCount; i++ )
    {
        snprintf( port_string, jack_port_name_size(), "in_%lu", off + i );
        UNLESS( stream->local_input_ports[i] = jack_port_register(
              jackHostApi->jack_client, port_string,
              JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0 ), paInsufficientMemory );
    }
    jackHostApi->inputBase += inputChannelCount;

    off = jackHostApi->outputBase;
    for( i = 0; i < outputChannelCount; i++ )
    {
        snprintf( port_string, jack_port_name_size(), "out_%lu", off + i );
        UNLESS( stream->local_output_ports[i] = jack_port_register(
             jackHostApi->jack_client, port_string,
             JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0 ), paInsufficientMemory );
    }
    jackHostApi->outputBase += outputChannelCount;

    /* look up the jack_port_t's for the remote ports.  We could do
     * this at stream start time, but doing it here ensures the
     * name lookup only happens once. */

    if( inputChannelCount > 0 )
    {
        int err = 0;
        UNLESS( !(inputSampleFormat & paNonInterleaved), paSampleFormatNotSupported );
        
        /* ... remote output ports (that we input from) */
        snprintf( regex_pattern, regexSz, "%s:.*", hostApi->deviceInfos[ inputParameters->device ]->name );
        UNLESS( jack_ports = jack_get_ports( jackHostApi->jack_client, regex_pattern,
                                     NULL, JackPortIsOutput ), paUnanticipatedHostError );
        for( i = 0; i < inputChannelCount && jack_ports[i]; i++ )
        {
            if( (stream->remote_output_ports[i] = jack_port_by_name(
                 jackHostApi->jack_client, jack_ports[i] )) == NULL ) 
            {
                err = 1;
                break;
            }
        }
        free( jack_ports );
        UNLESS( !err, paInsufficientMemory );

        /* Fewer ports than expected? */
        UNLESS( i == inputChannelCount, paInternalError );
    }

    if( outputChannelCount > 0 )
    {
        int err = 0;
        UNLESS( !(outputSampleFormat & paNonInterleaved), paSampleFormatNotSupported );

        /* ... remote input ports (that we output to) */
        snprintf( regex_pattern, regexSz, "%s:.*", hostApi->deviceInfos[ outputParameters->device ]->name );
        UNLESS( jack_ports = jack_get_ports( jackHostApi->jack_client, regex_pattern,
                                     NULL, JackPortIsInput ), paUnanticipatedHostError );
        for( i = 0; i < outputChannelCount && jack_ports[i]; i++ )
        {
            if( (stream->remote_input_ports[i] = jack_port_by_name(
                 jackHostApi->jack_client, jack_ports[i] )) == 0 )
            {
                err = 1;
                break;
            }
        }
        free( jack_ports );
        UNLESS( !err , paInsufficientMemory );

        /* Fewer ports than expected? */
        UNLESS( i == outputChannelCount, paInternalError );
    }

    ENSURE_PA( PaUtil_InitializeBufferProcessor(
                  &stream->bufferProcessor,
                  inputChannelCount,
                  inputSampleFormat,
                  paFloat32,            /* hostInputSampleFormat */
                  outputChannelCount,
                  outputSampleFormat,
                  paFloat32,            /* hostOutputSampleFormat */
                  jackSr,
                  streamFlags,
                  framesPerBuffer,
                  0,                            /* Ignored */
                  paUtilUnknownHostBufferSize,  /* Buffer size may vary on JACK's discretion */
                  streamCallback,
                  userData ) );
    bpInitialized = 1;

    if( stream->num_incoming_connections > 0 )
        stream->streamRepresentation.streamInfo.inputLatency = jack_port_get_latency( stream->remote_output_ports[0] )
            + PaUtil_GetBufferProcessorInputLatency( &stream->bufferProcessor );
    if( stream->num_outgoing_connections > 0 )
        stream->streamRepresentation.streamInfo.outputLatency = jack_port_get_latency( stream->remote_input_ports[0] )
            + PaUtil_GetBufferProcessorOutputLatency( &stream->bufferProcessor );

    stream->streamRepresentation.streamInfo.sampleRate = jackSr;
    stream->t0 = jack_frame_time( jackHostApi->jack_client );   /* A: Time should run from Pa_OpenStream */

    ENSURE_PA( AddStream( stream ) );  /* Add to queue over opened streams */
    
    *s = (PaStream*)stream;

    return result;

error:
    if( stream )
        CleanUpStream( stream, srInitialized, bpInitialized );

    return result;
}

/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaJackStream *stream = (PaJackStream*)s;

    /* Remove this stream from the processing queue */
    ENSURE_PA( RemoveStream( stream ) );

    CleanUpStream( stream, 1, 1 );

error:
    return result;
}

static PaError RealProcess( PaJackStream *stream, jack_nframes_t frames )
{
    PaError result = paNoError;
    PaStreamCallbackTimeInfo timeInfo = {0,0,0};
    int chn;
    int framesProcessed;
    const double sr = jack_get_sample_rate( stream->jack_client );    /* Shouldn't change during the process callback */
    PaStreamCallbackFlags cbFlags = 0;

    /* If the user has returned !paContinue from the callback we'll want to flush the internal buffers,
     * when these are empty we can finally mark the stream as inactive */
    if( stream->callbackResult != paContinue &&
            PaUtil_IsBufferProcessorOutputEmpty( &stream->bufferProcessor ) )
    {
        int i;

        stream->is_active = 0;
        if( stream->streamRepresentation.streamFinishedCallback )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
        PA_DEBUG(( "%s: Callback finished\n", __FUNCTION__ ));

        /* Before returning we will silence the output */
        PA_DEBUG(( "Silencing the output\n" ));
        for ( i = 0; i < stream->num_outgoing_connections; ++i )
        {
            jack_default_audio_sample_t *buffer = jack_port_get_buffer( stream->local_output_ports[i], frames );
            memset( buffer, 0, sizeof (jack_default_audio_sample_t) * frames );
        }

        goto end;
    }
    
    timeInfo.currentTime = jack_frame_time( stream->jack_client ) / sr;
    if( stream->num_incoming_connections > 0 )
        timeInfo.inputBufferAdcTime = timeInfo.currentTime - jack_port_get_latency( stream->local_input_ports[0] )
            / sr;
    if( stream->num_outgoing_connections > 0 )
        timeInfo.outputBufferDacTime = timeInfo.currentTime + jack_port_get_latency( stream->local_output_ports[0] )
            / sr;

    PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

    if( stream->xrun )
    {
        /* XXX: Any way to tell which of these occurred? */
        cbFlags = paOutputUnderflow | paInputOverflow;
        stream->xrun = FALSE;
    }
    PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo,
            cbFlags );

    for( chn = 0; chn < stream->num_incoming_connections; chn++ )
    {
        jack_default_audio_sample_t *channel_buf;
        channel_buf = (jack_default_audio_sample_t*)
            jack_port_get_buffer( stream->local_input_ports[chn],
                    frames );

        PaUtil_SetNonInterleavedInputChannel( &stream->bufferProcessor,
                chn,
                channel_buf );
    }

    for( chn = 0; chn < stream->num_outgoing_connections; chn++ )
    {
        jack_default_audio_sample_t *channel_buf;
        channel_buf = (jack_default_audio_sample_t*)
            jack_port_get_buffer( stream->local_output_ports[chn],
                    frames );

        PaUtil_SetNonInterleavedOutputChannel( &stream->bufferProcessor,
                chn,
                channel_buf );
    }

    if( stream->num_incoming_connections > 0 )
        PaUtil_SetInputFrameCount( &stream->bufferProcessor, frames );
    if( stream->num_outgoing_connections > 0 )
        PaUtil_SetOutputFrameCount( &stream->bufferProcessor, frames );

    framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor,
            &stream->callbackResult );
    assert( framesProcessed == frames );

    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

end:
    return result;
}

static int JackCallback( jack_nframes_t frames, void *userData )
{
    PaError result = paNoError;
    PaJackHostApiRepresentation *hostApi = (PaJackHostApiRepresentation *)userData;
    /*PaJackStream *stream = (PaJackStream*)userData;*/
    int err;
    int queueModified = 0;
    PaJackStream *stream = NULL;
    const double jackSr = jack_get_sample_rate( hostApi->jack_client );
    int xrun = hostApi->xrun;
    hostApi->xrun = 0;

    assert( hostApi );

    /* See if we should alter the processing queue */

    if( (err = pthread_mutex_trylock( &hostApi->mtx )) == 0 )
    {
        if( hostApi->toAdd )
        {
            if( hostApi->processQueue )
            {
                PaJackStream *node = hostApi->processQueue;
                /* Advance to end of queue */
                while( node->next )
                    node = node->next;

                node->next = hostApi->toAdd;
            }
            else
                hostApi->processQueue = (PaJackStream *)hostApi->toAdd;

            /* If necessary, update stream state */
            if( hostApi->toAdd->streamRepresentation.streamInfo.sampleRate != jackSr )
                UpdateSampleRate( hostApi->toAdd, jackSr );

            hostApi->toAdd = NULL;
            queueModified = 1;
        }
        if( hostApi->toRemove )
        {
            int removed = 0;
            PaJackStream *node = hostApi->processQueue, *prev = NULL;
            assert( hostApi->processQueue );

            while( node )
            {
                if( node == hostApi->toRemove )
                {
                    if( prev )
                        prev->next = node->next;
                    else
                        hostApi->processQueue = (PaJackStream *)node->next;

                    removed = 1;
                    break;
                }

                prev = node;
                node = node->next;
            }
            UNLESS( removed, paInternalError );
            hostApi->toRemove = NULL;
            PA_DEBUG(( "%s: Removed stream from processing queue\n", __FUNCTION__ ));
            queueModified = 1;
        }

        if( queueModified )
        {
            /* Signal that we've done what was asked of us */
            ASSERT_CALL( pthread_cond_signal( &hostApi->cond ), 0 );
        }
        ASSERT_CALL( pthread_mutex_unlock( &hostApi->mtx ), 0 );
    }
    else
        assert( err == EBUSY );

    /* Process each stream */
    stream = hostApi->processQueue;
    for( ; stream; stream = stream->next )
    {
        /*  XXX: We should silence any output frames even if the stream isn't technically active?
        if( !stream->is_running )
            continue;
            */

        if( xrun )  /* Don't override if already set */
            stream->xrun = 1;

        /* See if this stream is to be started */
        if( stream->doStart )
        {
            /* If we can't obtain a lock, we'll try next time */
            int err = pthread_mutex_trylock( &stream->hostApi->mtx );
            if( !err )
            {
                stream->is_active = 1;
                stream->doStart = 0;
                PA_DEBUG(( "%s: Starting stream\n", __FUNCTION__ ));
                ASSERT_CALL( pthread_cond_signal( &stream->hostApi->cond ), 0 );
                ASSERT_CALL( pthread_mutex_unlock( &stream->hostApi->mtx ), 0 );

                stream->callbackResult = paContinue;
            }
            else
                assert( err == EBUSY );
        }
        else if( stream->doStop || stream->doAbort )    /* Should we stop/abort stream? */
        {
            if( stream->callbackResult == paContinue )     /* Ok, make it stop */
            {
                PA_DEBUG(( "%s: Stopping stream\n", __FUNCTION__ ));
                stream->callbackResult = stream->doStop ? paComplete : paAbort;
            }
            else if( !stream->is_active )   /* Ok, signal to the main thread that we've carried out the operation */
            {
                /* If we can't obtain a lock, we'll try next time */
                int err = pthread_mutex_trylock( &stream->hostApi->mtx );
                if( !err )
                {
                    stream->doStop = stream->doAbort = 0;
                    ASSERT_CALL( pthread_cond_signal( &stream->hostApi->cond ), 0 );
                    ASSERT_CALL( pthread_mutex_unlock( &stream->hostApi->mtx ), 0 );
                }
                else
                    assert( err == EBUSY );
            }
        }

        if( stream->is_active )
            ENSURE_PA( RealProcess( stream, frames ) );
    }

    /*
    if( stream->t0 == -1 )
    {
        if( stream->num_outgoing_connections == 0 )
        {
        */
            /* TODO: how to handle stream time for capture-only operation? */
    /*
        }
        else
        {
        */
            /* the beginning time needs to be initialized */
    /*
            stream->t0 = jack_frame_time( stream->jack_client ) -
                         jack_frames_since_cycle_start( stream->jack_client) +
                         jack_port_get_total_latency( stream->jack_client,
                                                      stream->local_output_ports[0] );
        }
    }
    */

    return 0;
error:
    return -1;
}

static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaJackStream *stream = (PaJackStream*)s;
    int i;

    /* Ready the processor */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    /* connect the ports */

    /* NOTE: I would rather use jack_port_connect which uses jack_port_t's
     * instead of port names, but it is not implemented yet. */
    if( stream->num_incoming_connections > 0 )
    {
        for( i = 0; i < stream->num_incoming_connections; i++ )
            UNLESS( !jack_connect( stream->jack_client,
                    jack_port_name( stream->remote_output_ports[i] ),
                    jack_port_name( stream->local_input_ports[i] ) ), paUnanticipatedHostError );
    }

    if( stream->num_outgoing_connections > 0 )
    {
        for( i = 0; i < stream->num_outgoing_connections; i++ )
            UNLESS( !jack_connect( stream->jack_client,
                    jack_port_name( stream->local_output_ports[i] ),
                    jack_port_name( stream->remote_input_ports[i] ) ), paUnanticipatedHostError );
    }

    stream->xrun = FALSE;

    /* Enable processing */

    stream->is_running = TRUE;

    ASSERT_CALL( pthread_mutex_lock( &stream->hostApi->mtx ), 0 );
    stream->doStart = 1;
    ASSERT_CALL( pthread_cond_wait( &stream->hostApi->cond, &stream->hostApi->mtx ), 0 );
    ASSERT_CALL( pthread_mutex_unlock( &stream->hostApi->mtx ), 0 );
    UNLESS( stream->is_active, paInternalError );
    PA_DEBUG(( "%s: Stream started\n", __FUNCTION__ ));

error:
    return result;
}

static PaError RealStop( PaJackStream *stream, int abort )
{
    PaError result = paNoError;
    int i;

    ASSERT_CALL( pthread_mutex_lock( &stream->hostApi->mtx ), 0 );
    if( abort )
        stream->doAbort = 1;
    else
        stream->doStop = 1;
    ASSERT_CALL( pthread_cond_wait( &stream->hostApi->cond, &stream->hostApi->mtx ), 0 );
    ASSERT_CALL( pthread_mutex_unlock( &stream->hostApi->mtx ), 0 );
    UNLESS( !stream->is_active, paInternalError );
    
    stream->is_running = FALSE;
    PA_DEBUG(( "%s: Stream stopped\n", __FUNCTION__ ));

    /* Disconnect ports belonging to this stream */

    if( !stream->hostApi->jackIsDown )  /* XXX: Well? */
    {
        if( stream->num_incoming_connections > 0 )
        {
            for( i = 0; i < stream->num_incoming_connections; i++ )
                UNLESS( !jack_disconnect( stream->jack_client,
                            jack_port_name( stream->remote_output_ports[i] ),
                            jack_port_name( stream->local_input_ports[i] ) ), paUnanticipatedHostError );
        }
        if( stream->num_outgoing_connections > 0 )
        {
            for( i = 0; i < stream->num_outgoing_connections; i++ )
                UNLESS( !jack_disconnect( stream->jack_client,
                            jack_port_name( stream->local_output_ports[i] ),
                            jack_port_name( stream->remote_input_ports[i] ) ), paUnanticipatedHostError );
        }
    }

error:
    return result;
}

static PaError StopStream( PaStream *s )
{
    assert(s);
    return RealStop( (PaJackStream *)s, 0 );
}

static PaError AbortStream( PaStream *s )
{
    assert(s);
    return RealStop( (PaJackStream *)s, 1 );
}

static PaError IsStreamStopped( PaStream *s )
{
    PaJackStream *stream = (PaJackStream*)s;
    return stream->is_running == FALSE;
}


static PaError IsStreamActive( PaStream *s )
{
    PaJackStream *stream = (PaJackStream*)s;
    return stream->is_active == TRUE;
}


static PaTime GetStreamTime( PaStream *s )
{
    PaJackStream *stream = (PaJackStream*)s;

    /* A: Is this relevant??
    * TODO: what if we're recording-only? */
    return (jack_frame_time( stream->jack_client ) - stream->t0) / (PaTime)jack_get_sample_rate( stream->jack_client );
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaJackStream *stream = (PaJackStream*)s;
    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}
