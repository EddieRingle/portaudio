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
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
/*static PaTime GetStreamInputLatency( PaStream *stream );*/
/*static PaTime GetStreamOutputLatency( PaStream *stream );*/
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );


/*
 * Data specific to this API
 */

typedef struct
{
    PaUtilHostApiRepresentation commonHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;

    PaUtilAllocationGroup *deviceInfoMemory;

    jack_client_t *jack_client;
    PaHostApiIndex hostApiIndex;

    pthread_mutex_t jackMtx;
    double jackSampleRate;
}
PaJackHostApiRepresentation;

#define MAX_CLIENTS 100
#define TRUE 1
#define FALSE 0

/*
 * Functions specific to this API
 */

static PaError BuildDeviceList( PaJackHostApiRepresentation *jackApi );
static int JackCallback( jack_nframes_t frames, void *userData );
static int JackSrCallback( jack_nframes_t nframes, void *arg )
{
    PaJackHostApiRepresentation *jackApi = (PaJackHostApiRepresentation *)arg;
    ASSERT_CALL( pthread_mutex_lock( &jackApi->jackMtx ), 0 );
    jackApi->jackSampleRate = nframes;
    ASSERT_CALL( pthread_mutex_unlock( &jackApi->jackMtx ), 0 );

    return 0;
}


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
    int num_clients = 0;
    int port_index, client_index, i;
    double globalSampleRate;
    regex_t port_regex;

    commonApi->info.defaultInputDevice = paNoDevice;
    commonApi->info.defaultOutputDevice = paNoDevice;
    commonApi->info.deviceCount = 0;

    /* since we are rebuilding the list of devices, free all memory
     * associated with the previous list */
    PaUtil_FreeAllAllocations( jackApi->deviceInfoMemory );

    /* We can only retrieve the list of clients indirectly, by first
     * asking for a list of all ports, then parsing the port names
     * according to the client_name:port_name convention (which is
     * enforced by jackd)
     * A: If jack_get_ports returns NULL, there's nothing for us to do */
    UNLESS( (jack_ports = jack_get_ports( jackApi->jack_client, "", "", 0 )) && jack_ports[0], paNoError );

    /* Parse the list of ports, using a regex to grab the client names */
    regcomp( &port_regex, "^[^:]*", REG_EXTENDED );

    /* Build a list of clients from the list of ports */
    for( port_index = 0; jack_ports[port_index] != NULL; port_index++ )
    {
        int client_seen;
        regmatch_t match_info;
        char *tmp_client_name = alloca(jack_client_name_size());
        const char *port = jack_ports[port_index];

        /* extract the client name from the port name, using a regex
         * that parses the clientname:portname syntax */
        regexec( &port_regex, port, 1, &match_info, 0 );
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
    free( jack_ports );

    /* Now we have a list of clients, which will become the list of
     * PortAudio devices. */

    /* there is one global sample rate all clients must conform to */

    globalSampleRate = jack_get_sample_rate( jackApi->jack_client );
    UNLESS( commonApi->deviceInfos = (PaDeviceInfo**)MALLOC( sizeof(PaDeviceInfo*) *
                                                     num_clients ), paInsufficientMemory );

    assert( commonApi->info.deviceCount == 0 );

    /* Create a PaDeviceInfo structure for every client */
    char *regex_pattern = alloca(jack_client_name_size() + 3);
    for( client_index = 0; client_index < num_clients; client_index++ )
    {
        PaDeviceInfo *curDevInfo;

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
                jack_port_get_latency( p ) / jack_get_sample_rate( jackApi->jack_client );
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
                jack_port_get_latency( p ) / jack_get_sample_rate( jackApi->jack_client );
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
    return paNoError;
}
#undef MALLOC

PaError PaJack_Initialize( PaUtilHostApiRepresentation **hostApi,
                           PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    PaJackHostApiRepresentation *jackHostApi;
    *hostApi = NULL;    /* Initialize to NULL */

    UNLESS( jackHostApi = (PaJackHostApiRepresentation*)
        PaUtil_AllocateMemory( sizeof(PaJackHostApiRepresentation) ), paInsufficientMemory );
    jackHostApi->deviceInfoMemory = NULL;

    ASSERT_CALL( pthread_mutex_init( &jackHostApi->jackMtx, NULL ), 0 );

    /* Try to become a client of the JACK server.  If we cannot do
     * this, than this API cannot be used. */

    jackHostApi->jack_client = jack_client_new( "PortAudio client" );
    if( jackHostApi->jack_client == 0 )
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

    jackHostApi->jackSampleRate = jack_get_sample_rate( jackHostApi->jack_client );
    jack_set_sample_rate_callback( jackHostApi->jack_client, JackSrCallback, jackHostApi );

    /* Register functions */

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &jackHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, StopStream,   /* Abort makes no difference from Stop in our case */
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    return result;

error:
    if( jackHostApi )
    {
        if( jackHostApi->jack_client )
            jack_client_close( jackHostApi->jack_client );

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

    ASSERT_CALL( pthread_mutex_destroy( &jackHostApi->jackMtx ), 0 );

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
    int is_running;
    int is_active;

    int xrun;   /* Received xrun notification from JACK? */

    jack_nframes_t t0;
    unsigned long total_frames_sent;

    PaUtilAllocationGroup *stream_memory;
}
PaJackStream;

/*!
 * Free resources associated with stream, and eventually stream itself.
 *
 * Frees allocated memory, and closes opened pcms.
 */
static void CleanUpStream( PaJackStream *stream )
{
    int i;
    assert( stream );

    for( i = 0; i < stream->num_incoming_connections; ++i )
    {
        if( stream->local_input_ports[i] )
        {
            ASSERT_CALL( jack_port_unregister( stream->jack_client, stream->local_input_ports[i] ), 0 );
            free( stream->local_input_ports[i]);
        }
        free( stream->remote_output_ports[i] );
    }
    for( i = 0; i < stream->num_outgoing_connections; ++i )
    {
        if( stream->local_output_ports[i] )
        {
            ASSERT_CALL( jack_port_unregister(stream->jack_client, stream->local_output_ports[i] ), 0 );
            free( stream->local_output_ports[i]);
        }
        free( stream->remote_input_ports[i] );
    }

    if( stream->stream_memory )
    {
        PaUtil_FreeAllAllocations( stream->stream_memory );
        PaUtil_DestroyAllocationGroup( stream->stream_memory );
    }
    PaUtil_FreeMemory( stream );
}

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
    PaJackStream *stream = 0;
    char port_string[100];
    char *regex_pattern = alloca( jack_client_name_size() );
    const char **jack_ports;
    int jack_max_buffer_size = jack_get_buffer_size( jackHostApi->jack_client );
    int i;
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat = 0, outputSampleFormat = 0;

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

    /* the client has no say over the frames per callback */

    if( framesPerBuffer == paFramesPerBufferUnspecified )
        framesPerBuffer = jack_max_buffer_size;
    else    /* TODO: Block adaptation? */
        framesPerBuffer = jack_max_buffer_size;

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

    /* ... check that the sample rate exactly matches the ONE acceptable rate */

#define ABS(x) ( (x) > 0 ? (x) : -(x) )
    if( ABS(sampleRate - jack_get_sample_rate( jackHostApi->jack_client )) > 1 )
       return paInvalidSampleRate;
#undef ABS

    /* Allocate memory for structuures */

#define MALLOC(size) \
    (PaUtil_GroupAllocateMemory( stream->stream_memory, (size) ))

    UNLESS( stream = (PaJackStream*)PaUtil_AllocateMemory( sizeof(PaJackStream) ), paInsufficientMemory );
    UNLESS( stream->stream_memory = PaUtil_CreateAllocationGroup(), paInsufficientMemory );
    stream->jack_client = jackHostApi->jack_client;

    UNLESS( stream->local_input_ports =
        (jack_port_t**) MALLOC(sizeof(jack_port_t*) * inputChannelCount ), paInsufficientMemory );
    memset( stream->local_input_ports, 0, sizeof(jack_port_t*) * inputChannelCount );
    UNLESS( stream->local_output_ports =
        (jack_port_t**) MALLOC( sizeof(jack_port_t*) * outputChannelCount ), paInsufficientMemory );
    memset( stream->local_output_ports, 0, sizeof(jack_port_t*) * outputChannelCount );
    UNLESS( stream->remote_output_ports =
        (jack_port_t**) MALLOC( sizeof(jack_port_t*) * inputChannelCount ), paInsufficientMemory );
    memset( stream->remote_input_ports, 0, sizeof(jack_port_t*) * inputChannelCount );
    UNLESS( stream->remote_input_ports =
        (jack_port_t**) MALLOC( sizeof(jack_port_t*) * outputChannelCount ), paInsufficientMemory );
    memset( stream->remote_output_ports, 0, sizeof(jack_port_t*) * outputChannelCount );

    stream->num_incoming_connections = inputChannelCount;
    stream->num_outgoing_connections = outputChannelCount;

    PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
            &jackHostApi->callbackStreamInterface, streamCallback, userData );
    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, jack_get_sample_rate( stream->jack_client ) );

    /* create the JACK ports.  We cannot connect them until audio
     * processing begins */

    for( i = 0; i < inputChannelCount; i++ )
    {
        sprintf( port_string, "in_%d", i );
        UNLESS( stream->local_input_ports[i] = jack_port_register(
              jackHostApi->jack_client, port_string,
              JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0 ), paInsufficientMemory );
    }

    for( i = 0; i < outputChannelCount; i++ )
    {
        sprintf( port_string, "out_%d", i );
        UNLESS( stream->local_output_ports[i] = jack_port_register(
             jackHostApi->jack_client, port_string,
             JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0 ), paInsufficientMemory );
    }

    /* look up the jack_port_t's for the remote ports.  We could do
     * this at stream start time, but doing it here ensures the
     * name lookup only happens once. */

    if( inputChannelCount > 0 )
    {
        UNLESS( !(inputSampleFormat & paNonInterleaved), paSampleFormatNotSupported );
        
        /* ... remote output ports (that we input from) */
        sprintf( regex_pattern, "%s:.*", hostApi->deviceInfos[ inputParameters->device ]->name );
        UNLESS( jack_ports = jack_get_ports( jackHostApi->jack_client, regex_pattern,
                                     NULL, JackPortIsOutput), paUnanticipatedHostError );
        for( i = 0; i < inputChannelCount && jack_ports[i]; i++ )
        {
            UNLESS( stream->remote_output_ports[i] = jack_port_by_name(
                 jackHostApi->jack_client, jack_ports[i] ), paInsufficientMemory );
        }
        free( jack_ports );

        if( i < inputChannelCount )
        {
            /* we found fewer ports than we expected */
            ENSURE_PA( paInternalError );
        }
    }

    if( outputChannelCount > 0 )
    {
        UNLESS( !(outputSampleFormat & paNonInterleaved), paSampleFormatNotSupported );

        /* ... remote input ports (that we output to) */
        sprintf( regex_pattern, "%s:.*", hostApi->deviceInfos[ outputParameters->device ]->name );
        UNLESS( jack_ports = jack_get_ports( jackHostApi->jack_client, regex_pattern,
                                     NULL, JackPortIsInput), paUnanticipatedHostError );
        for( i = 0; i < outputChannelCount && jack_ports[i]; i++ )
        {
            UNLESS( stream->remote_input_ports[i] = jack_port_by_name(
                 jackHostApi->jack_client, jack_ports[i] ), paInsufficientMemory );
        }
        free( jack_ports );

        if( i < outputChannelCount )
        {
            /* we found fewer ports than we expected */
            ENSURE_PA( paInternalError );
        }
    }

    ENSURE_PA( PaUtil_InitializeBufferProcessor(
                  &stream->bufferProcessor,
                  inputChannelCount,
                  inputSampleFormat,
                  paFloat32,            /* hostInputSampleFormat */
                  outputChannelCount,
                  outputSampleFormat,
                  paFloat32,            /* hostOutputSampleFormat */
                  jack_get_sample_rate( jackHostApi->jack_client ),
                  streamFlags,
                  framesPerBuffer,
                  jack_max_buffer_size,
                  paUtilFixedHostBufferSize,
                  streamCallback,
                  userData ) );

    if( stream->num_incoming_connections > 0 )
        stream->streamRepresentation.streamInfo.inputLatency = jack_port_get_latency( stream->remote_output_ports[0] )
            + PaUtil_GetBufferProcessorInputLatency( &stream->bufferProcessor );
    if( stream->num_outgoing_connections > 0 )
        stream->streamRepresentation.streamInfo.outputLatency = jack_port_get_latency( stream->remote_input_ports[0] )
            + PaUtil_GetBufferProcessorInputLatency( &stream->bufferProcessor );

    stream->streamRepresentation.streamInfo.sampleRate = jack_get_sample_rate( jackHostApi->jack_client );

    stream->is_running = stream->is_active = FALSE;
    stream->t0 = jack_frame_time( jackHostApi->jack_client );   /* A: Time should run from Pa_OpenStream */
    stream->total_frames_sent = 0;

    UNLESS( !jack_set_process_callback( jackHostApi->jack_client, JackCallback, stream ), paUnanticipatedHostError );

    *s = (PaStream*)stream;

    return result;

error:
    if( stream )
        CleanUpStream( stream );

    return result;

#undef MALLOC
}

/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaJackStream *stream = (PaJackStream*)s;

    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    CleanUpStream( stream );

    return result;
}


/* Zero out buffer from begin */
static void SilenceBuffers( jack_port_t **ports, int numChannels, unsigned long begin, unsigned long frames )
{
    int i;
    for ( i = 0; i < numChannels; ++i )
    {
        jack_default_audio_sample_t *buffer = jack_port_get_buffer( ports[i], frames );
        memset( buffer + begin, 0, sizeof (jack_default_audio_sample_t) * frames - begin );
    }
}

static int JackCallback( jack_nframes_t frames, void *userData )
{
    PaJackStream *stream = (PaJackStream*)userData;
    PaStreamCallbackTimeInfo timeInfo = {0,0,0};
    int callbackResult;
    int chn;
    int framesProcessed;
    double sr = jack_get_sample_rate( stream->jack_client );
    PaStreamCallbackFlags cbFlags = 0;
    static int hasFinished = FALSE;

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

    /* Call streamFinishedCallback once we reach the inactive state */
    if( hasFinished )
    {
        if( stream->streamRepresentation.streamFinishedCallback )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
        hasFinished = FALSE;
    }

    /* Since there seems to be no way of signaling a stop from the JACK callback we will stay in a no-op
     * state after paComplete/paAbort has been returned from the user callback, untill the user explicitly
     * calls Pa_StopStream/Pa_AbortStream */
    if (!stream->is_active)
    {
        SilenceBuffers( stream->local_output_ports, stream->num_outgoing_connections, 0, frames );

        return 0;
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

    callbackResult = paContinue;
    framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor,
            &callbackResult );

    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
    stream->total_frames_sent += frames;

    assert( callbackResult == paContinue || callbackResult == paAbort || callbackResult == paComplete );
    if( callbackResult == paAbort || callbackResult == paComplete )
    {
        stream->is_active = FALSE;
        SilenceBuffers( stream->local_output_ports, stream->num_outgoing_connections, framesProcessed, frames );
        hasFinished = TRUE;
    }

    return 0;
}

static int JackXRunCb(void *arg) {
    PaJackStream *stream = (PaJackStream *)arg;
    assert( stream );
    stream->xrun = TRUE;
    return 0;
}

static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaJackStream *stream = (PaJackStream*)s;
    int i, activated = 0;

    /* Ready the processor */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    /* start the audio thread */
    UNLESS( !jack_set_xrun_callback( stream->jack_client, JackXRunCb, stream ), paUnanticipatedHostError );
    UNLESS( !jack_activate( stream->jack_client ), paUnanticipatedHostError );
    activated = 1;

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

    stream->is_running = TRUE;
    stream->is_active = TRUE;

    return result;
error:
    if( activated )
        ASSERT_CALL( jack_deactivate( stream->jack_client ), 0 );
    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaJackStream *stream = (PaJackStream*)s;

    /* note: this automatically disconnects all ports, since a deactivated
     * client is not allowed to have any ports connected */
    UNLESS( !jack_deactivate( stream->jack_client ), paUnanticipatedHostError );

    stream->is_running = FALSE;
    stream->is_active = FALSE;

    /* TODO: block until playback complete */

error:
    return result;
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
