/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library. 
 * Latest Version at: http://www.portaudio.com.
 * Silicon Graphics (SGI) IRIX implementation by Pieter Suurmond.
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
 *
 */
/** @file
 @brief SGI IRIX AL implementation (according to V19 API version 2.0).

 @note This file started as a copy of pa_skeleton.c (v 1.1.2.35 2003/09/20), it
 has nothing to do with the old V18 pa_sgi version: this implementation uses the
 newer IRIX AL calls and uses pthreads instead of sproc.

 On IRIX, one may type './configure' followed by 'gmake' from the portaudio root 
 directory to build the static and shared libraries, as well as all the tests.

 On IRIX 6.5, using 'make' instead of 'gmake' may cause Makefile to fail. (This 
 happens on my machine: make does not understand syntax with 2 colons on a line,
 like this:
               $(TESTS): bin/%: [snip]

 Maybe this is due to an old make version(?), my only solution is: use gmake.
 Anyway, all the tests compile well now, with GCC 3.3, as well as with MIPSpro 7.2.1.
 CPU-measurements now also runs (thanks to Arve, who implemented PaUtil_GetTime()).
 Tested:
        - paqa_devs              ok, but at a certain point digital i/o fails:
                                     TestAdvance: INPUT, device = 2, rate = 32000, numChannels = 1, format = 1
                                     Possibly, this is an illegal sr or number of channels for digital i/o.
        - paqa_errs              13 of the tests run ok, but 5 of the tests give weird results.
        + patest1                ok.
        + patest_buffer          ok.
        + patest_callbackstop    ok now (no coredumps any longer).
        - patest_clip            ok, but hear no difference between dithering turned OFF and ON.
        + patest_hang            ok.
        + patest_latency         ok.
        + patest_leftright       ok.
        + patest_maxsines        ok.
        + patest_many            ok.
        + patest_multi_sine      ok.
        + patest_pink            ok.
        - patest_prime           ok, but we should work on 'playback with priming'!
        - patest_read_record     ok, but playback stops a little earlier than 5 seconds it seems(?).
        + patest_record          ok.
        + patest_ringmix         ok.
        + patest_saw             ok.
        + patest_sine            ok.
        + patest_sine8           ok.
        - patest_sine_formats    ok, FLOAT32 + INT16 + INT18 are OK, but UINT8 IS NOT OK!
        + patest_sine_time       ok, reported latencies seem to match the suggested (max) latency.
        + patest_start_stop      ok, but under/overflow errors of course in the AL queue monitor.
        + patest_stop            ok.
        - patest_sync            ok?
        + patest_toomanysines    ok, CPU load measurement works fine!
        - patest_underflow       ok? (stopping after SleepTime = 91: err=Stream is stopped)
        - patest_wire            ok.
        + patest_write_sine      ok.
        + pa_devs                ok.
        + pa_fuzz                ok.
        + pa_minlat              ok.
        
 Todo: - Prefilling with silence, only when requested (it now always prefills).
       - Underrun and overflow flags.
       - Make a complete new version to support 'sproc'-applications.
    
 Note: Even when mono-output is requested, with ALv7, the audio library opens
       a outputs stereo. One can observe this in SGI's 'Audio Queue Monitor'.

*/

#include <string.h>         /* strlen() */
#include <stdio.h>          /* printf() */
#include <math.h>           /* fabs()   */

#include <dmedia/audio.h>   /* IRIX AL (audio library). Link with -laudio. */
#include <dmedia/dmedia.h>  /* IRIX DL (digital media library), solely for */
                            /* function dmGetUST(). Link with -ldmedia.    */
#include <errno.h>          /* To catch 'oserror' after AL-calls. */
#include <pthread.h>        /* POSIX threads. */
#include <unistd.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

                            /* Uncomment for diagnostics: */
#define DBUG(x) { printf x; fflush(stdout); }


/* prototypes for functions declared in this file */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

PaError PaSGI_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

#ifdef __cplusplus
}
#endif /* __cplusplus */


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
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, const void *buffer, unsigned long frames );
static signed long GetStreamReadAvailable( PaStream* stream );
static signed long GetStreamWriteAvailable( PaStream* stream );


/* IMPLEMENT ME: a macro like the following one should be used for reporting
 host errors */
#define PA_SGI_SET_LAST_HOST_ERROR( errorCode, errorText ) \
    PaUtil_SetLastHostErrorInfo( paInDevelopment, errorCode, errorText )

/* PaSGIHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation   inheritedHostApiRep;
    PaUtilStreamInterface         callbackStreamInterface;
    PaUtilStreamInterface         blockingStreamInterface;
    PaUtilAllocationGroup*        allocations;
                                                    /* implementation specific data goes here. */
    ALvalue*                      sgiDeviceIDs;     /* Array of AL resource device numbers.    */
 /* PaHostApiIndex                hostApiIndex;        Hu? As in the linux and oss files? */
}
PaSGIHostApiRepresentation;

/*
    Initialises sgiDeviceIDs array.
*/
PaError PaSGI_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError                     result = paNoError;
    int                         i, deviceCount, def_in, def_out;
    PaSGIHostApiRepresentation* SGIHostApi;
    PaDeviceInfo*               deviceInfoArray;    
    static const short          numParams = 4;            /* Array with name, samplerate, channels */
    ALpv                        y[numParams];             /* and type.                             */
    static const short          maxDevNameChars = 32;     /* Including the terminating null char.  */
    char                        devName[maxDevNameChars]; /* Too lazy for dynamic alloc.           */

    /* DBUG(("PaSGI_Initialize() started.\n")); */
    SGIHostApi = (PaSGIHostApiRepresentation*)PaUtil_AllocateMemory(sizeof(PaSGIHostApiRepresentation));
    if( !SGIHostApi )
        {
        result = paInsufficientMemory;
        goto cleanup;
        }
    SGIHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !SGIHostApi->allocations )
        {
        result = paInsufficientMemory;
        goto cleanup;
        }
    *hostApi = &SGIHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paInDevelopment;            /* Change to correct type id? */
    (*hostApi)->info.name = "SGI IRIX AL";
    (*hostApi)->info.defaultInputDevice  = paNoDevice;  /* Set later. */
    (*hostApi)->info.defaultOutputDevice = paNoDevice;  /* Set later.  */
    (*hostApi)->info.deviceCount = 0;                   /* We 'll increment in the loop below. */
    
    /* Determine the total number of input and output devices (thanks to Gary Scavone). */
    deviceCount = alQueryValues(AL_SYSTEM, AL_DEVICES, 0, 0, 0, 0);
    if (deviceCount < 0)        /* Returns -1 in case of failure. */
        {
        DBUG(("AL error counting devices: %s\n", alGetErrorString(oserror())));
        result = paDeviceUnavailable;       /* Is this the right error return code? */
        goto cleanup;
        }
    if (deviceCount > 0)
        {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
                                  SGIHostApi->allocations, sizeof(PaDeviceInfo*) * deviceCount);
        if (!(*hostApi)->deviceInfos)
            {
            result = paInsufficientMemory;
            goto cleanup;
            }
        /* Allocate all device info structs in a contiguous block. */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
                          SGIHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount);
        if (!deviceInfoArray)
            {
            result = paInsufficientMemory;
            goto cleanup;
            }                                               /* Store all AL device IDs in an array. */
        SGIHostApi->sgiDeviceIDs = (ALvalue*)PaUtil_GroupAllocateMemory(SGIHostApi->allocations,
                                                                        deviceCount * sizeof(ALvalue));
        if (!SGIHostApi->sgiDeviceIDs)
            {
            result = paInsufficientMemory;
            goto cleanup;
            }
        /* Same query again, but now store all IDs in array sgiDeviceIDs (still no qualifiers). */
        if (deviceCount != alQueryValues(AL_SYSTEM, AL_DEVICES, SGIHostApi->sgiDeviceIDs, deviceCount, 0, 0))
            {
            DBUG(("Hu!? the number of devices suddenly changed!\n"));
            result = paUnanticipatedHostError;  /* Hu?, the number of devices suddenly changed! */
            goto cleanup;
            }
        y[0].param = AL_DEFAULT_INPUT;
        y[1].param = AL_DEFAULT_OUTPUT;
        if (2 != alGetParams(AL_SYSTEM, y, 2)) /* Get params global to the AL system. */
            {
            DBUG(("AL error: could not get default i/o!\n"));
            result = paUnanticipatedHostError;
            goto cleanup;
            }
        def_in  = y[0].value.i;         /* Remember both AL devices for a while. */
        def_out = y[1].value.i;
        y[0].param     = AL_NAME;
        y[0].value.ptr = devName;
        y[0].sizeIn    = maxDevNameChars; /* Including terminating null char. */
        y[1].param     = AL_RATE;
        y[2].param     = AL_CHANNELS;
        y[3].param     = AL_TYPE;       /* Subtype of AL_INPUT_DEVICE_TYPE or AL_OUTPUT_DEVICE_TYPE? */
        for (i=0; i < deviceCount; ++i) /* Fill allocated deviceInfo structs. */
            {
            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = hostApiIndex;
                                                /* Retrieve name, samplerate, channels and type. */
            if (numParams != alGetParams(SGIHostApi->sgiDeviceIDs[i].i, y, numParams))
                {
                result = paUnanticipatedHostError;
                goto cleanup;
                }
            deviceInfo->name = (char*)PaUtil_GroupAllocateMemory(SGIHostApi->allocations, strlen(devName) + 1);
            if (!deviceInfo->name)
                {
                result = paInsufficientMemory;
                goto cleanup;
                }
            strcpy((char*)deviceInfo->name, devName);

            /* Determine whether the received number of channels belongs to an input or for output device. */
            if (alIsSubtype(AL_INPUT_DEVICE_TYPE, y[3].value.i))
                {
                deviceInfo->maxInputChannels  = y[2].value.i;
                deviceInfo->maxOutputChannels = 0;
                }
            else if (alIsSubtype(AL_OUTPUT_DEVICE_TYPE, y[3].value.i))
                {
                deviceInfo->maxInputChannels  = 0;
                deviceInfo->maxOutputChannels = y[2].value.i;
                }
            else /* Should never occur. */
                {
                DBUG(("AL device '%s' is neither input nor output!\n", deviceInfo->name));
                result = paUnanticipatedHostError;
                goto cleanup;
                }
            
            /* Determine if this device is the default (in or out). If so, assign. */
            if (def_in == SGIHostApi->sgiDeviceIDs[i].i)
                {
                if ((*hostApi)->info.defaultInputDevice != paNoDevice)
                    {
                    DBUG(("Default input already assigned!\n"));
                    result = paUnanticipatedHostError;
                    goto cleanup;
                    }
                (*hostApi)->info.defaultInputDevice = i;
                /* DBUG(("Default input assigned to pa device %d (%s).\n", i, deviceInfo->name)); */
                }
            else if (def_out == SGIHostApi->sgiDeviceIDs[i].i)
                {
                if ((*hostApi)->info.defaultOutputDevice != paNoDevice)
                    {
                    DBUG(("Default output already assigned!\n"));
                    result = paUnanticipatedHostError;
                    goto cleanup;
                    }
                (*hostApi)->info.defaultOutputDevice = i;
                /* DBUG(("Default output assigned to pa device %d (%s).\n", i, deviceInfo->name)); */
                }

            deviceInfo->defaultLowInputLatency   = 0.100;  /* 100 milliseconds ok? */
            deviceInfo->defaultLowOutputLatency  = 0.100;
            deviceInfo->defaultHighInputLatency  = 0.500;  /* 500 milliseconds a reasonable value? */
            deviceInfo->defaultHighOutputLatency = 0.500;

            deviceInfo->defaultSampleRate = alFixedToDouble(y[1].value.ll); /* Read current sr. */
            (*hostApi)->deviceInfos[i] = deviceInfo;
            ++(*hostApi)->info.deviceCount;
            }
        }
    (*hostApi)->Terminate         = Terminate;
    (*hostApi)->OpenStream        = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface(&SGIHostApi->callbackStreamInterface, CloseStream, StartStream,
                                     StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                     GetStreamTime, GetStreamCpuLoad,
                                     PaUtil_DummyRead, PaUtil_DummyWrite,
                                     PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface(&SGIHostApi->blockingStreamInterface, CloseStream, StartStream,
                                     StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                     GetStreamTime, PaUtil_DummyGetCpuLoad,
                                     ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );
cleanup:        
    if (result != paNoError)
        {
        if (SGIHostApi)
            {
            if (SGIHostApi->allocations)
                {
                PaUtil_FreeAllAllocations(SGIHostApi->allocations);
                PaUtil_DestroyAllocationGroup(SGIHostApi->allocations);
                }
            PaUtil_FreeMemory(SGIHostApi);
            }
        }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaSGIHostApiRepresentation *SGIHostApi = (PaSGIHostApiRepresentation*)hostApi;

    /* DBUG(("Terminate() started.\n")); */
    /* Clean up any resources not handled by the allocation group. */
    if( SGIHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( SGIHostApi->allocations );
        PaUtil_DestroyAllocationGroup( SGIHostApi->allocations );
    }
    PaUtil_FreeMemory( SGIHostApi );
}

/*
    Check if samplerate is supported for this output device. Called once
    or twice by function IsFormatSupported() and one time by OpenStream().
*/
static PaError sr_supported(int al_device, double sr)
{
    int         e;
    PaError     result;
    ALparamInfo pinfo;
    long long   lsr;    /* 64 bit fixed point internal AL samplerate. */
    
    if (alGetParamInfo(al_device, AL_RATE, &pinfo))
        {
        e = oserror();
        DBUG(("alGetParamInfo(AL_RATE) failed: %s.\n", alGetErrorString(e)));
        if (e == AL_BAD_RESOURCE)
            result = paInvalidDevice;
        else
            result = paUnanticipatedHostError;
        }
    else
        {
        lsr = alDoubleToFixed(sr);  /* Within the range? */
        if ((pinfo.min.ll <= lsr) && (lsr <= pinfo.max.ll))
            result = paFormatIsSupported;
        else
            result = paInvalidSampleRate;
        }
    /* DBUG(("sr_supported()=%d.\n", result)); */
    return result;
}


/*
    See common/portaudio.h (suggestedLatency field is ignored).
*/
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    PaSGIHostApiRepresentation* SGIHostApi = (PaSGIHostApiRepresentation*)hostApi;
    int inputChannelCount, outputChannelCount, result;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    
    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
        /* Unless alternate device specification is supported, reject the use of
           paUseHostApiSpecificDeviceSpecification. */
        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;
        /* Check that input device can support inputChannelCount. */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;
        /* Validate inputStreamInfo. */
        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        /* Check if samplerate is supported for this input device. */
        result = sr_supported(SGIHostApi->sgiDeviceIDs[inputParameters->device].i, sampleRate);
        if (result != paFormatIsSupported)
            return result;
    }
    else
    {
        inputChannelCount = 0;
    }
    if( outputParameters ) /* As with input above. */
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;
        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        /* Check if samplerate is supported for this output device. */
        result = sr_supported(SGIHostApi->sgiDeviceIDs[outputParameters->device].i, sampleRate);
        if (result != paFormatIsSupported)
            return result;
    }
    else
    {
        outputChannelCount = 0;
    }
    /*  IMPLEMENT ME:
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
    /* suppress unused variable warnings */
    (void) inputSampleFormat;
    (void) outputSampleFormat;
    return paFormatIsSupported;
}

/** Auxilary struct, embedded twice in the struct below, for inputs and outputs. */
typedef struct PaSGIhostPortBuffer
{
            /** NULL means IRIX AL port closed. */
    ALport  port;
            /** NULL means memory not allocated. */
    void*   buffer;
}
    PaSGIhostPortBuffer;

/** Stream data structure specifically for this IRIX AL implementation. */
typedef struct PaSGIStream
{
    PaUtilStreamRepresentation  streamRepresentation;
    PaUtilCpuLoadMeasurer       cpuLoadMeasurer;
    PaUtilBufferProcessor       bufferProcessor;
    unsigned long               framesPerHostCallback;
                                /** Host buffers and AL ports. */
    PaSGIhostPortBuffer         hostPortBuffIn,
                                hostPortBuffOut;
                                /** Stream state may be 0 or 1 or 2, but never 3. */
    unsigned char               state;
                                /** Request to stop or abort (by parent or by child itself (user callback result)). */
    unsigned char               stopAbort;
    pthread_t                   thread;
}
    PaSGIStream;

/** Stream can be in only one of the following three states: stopped (1), active (2), or
    callback finshed (0). To prevent 'state 3' from occurring, Setting and testing of the
    state bits is done atomically.
*/
#define PA_SGI_STREAM_FLAG_FINISHED_ (0) /* After callback finished or cancelled queued buffers. */
#define PA_SGI_STREAM_FLAG_STOPPED_  (1) /* Set by OpenStream(), StopStream() and AbortStream(). */
#define PA_SGI_STREAM_FLAG_ACTIVE_   (2) /* Set by StartStream. Reset by OpenStream(),           */
                                         /* StopStream() and AbortStream().                      */

/** Stop requests, via the 'stopAbort' field can be either 1, meaning 'stop' or 2, meaning 'abort'.
    When both occur at the same time, 'abort' takes precedence, even after a first 'stop'.
*/
#define PA_SGI_REQ_CONT_    (0)         /* Reset by OpenStream(), StopStream and AbortStream. */
#define PA_SGI_REQ_STOP_    (1)         /* Set by StopStream(). */
#define PA_SGI_REQ_ABORT_   (2)         /* Set by AbortStream(). */

/** Called by OpenStream() once or twice. First, the number of channels and the sampleformat 
    is configured. The configuration is then bound to the specified AL device. Then an AL port 
    is opened. Finally, the samplerate of the device is altered (or at least set again).
    Writes actual latency in *pa_params, and samplerate *samplerate.

    @param alc should point to an already allocated AL configuration structure.
    @param pa_params may be NULL and pa_params->channelCount may also be null, in both 
           cases the function immediately returns.
    @return paNoError if configuration was skipped or if it succeeded.
*/
static PaError set_sgi_device(ALvalue*                  sgiDeviceIDs,   /* Array built by PaSGI_Initialize(). */
                              const PaStreamParameters* pa_params,      /* read device and channels. */                             
                              PaSampleFormat            pasfmt,         /* Don't read from pa_params!. */
                              char*                     direction,      /* "r" or "w". */
                              char*                     name,
                              long                      framesPerHostBuffer,
                              double*                   samplerate,     /* Also writes back here. */
                              int*                      iq_size,        /* Receive actual internal queue size in frames. */
                              PaSGIhostPortBuffer*      hostPortBuff)   /* Receive pointers here. */
{
    int       bytesPerFrame, sgiDevice, alErr, d, dd, default_iq_size;
    ALpv      pvs[2];
    ALconfig  alc = NULL;
    PaError   result = paNoError;

    if (!pa_params)
        goto cleanup;                  /* Not errors, just not full duplex, skip all. */
    if (!pa_params->channelCount)
        goto cleanup;
    alc = alNewConfig();    /* Create default config. This defaults to stereo, 16-bit integer data. */
    if (!alc)               /* Call alFreeConfig() later, when done with it. */
        { result = paInsufficientMemory;  goto cleanup; }
    /*----------------------- CONFIGURE NUMBER OF CHANNELS: ---------------------------*/
    if (alSetChannels(alc, pa_params->channelCount))          /* Returns 0 on success. */
        {
        if (oserror() == AL_BAD_CHANNELS)
            { result = paInvalidChannelCount;  goto cleanup; }
        result = paUnanticipatedHostError; goto cleanup;
        }
    bytesPerFrame = pa_params->channelCount;          /* Is multiplied by width below. */
    /*----------------------- CONFIGURE SAMPLE FORMAT: --------------------------------*/
    if (pasfmt == paFloat32)
        {
        if (alSetSampFmt(alc, AL_SAMPFMT_FLOAT))
            {
            if (oserror() == AL_BAD_SAMPFMT)
                { result = paSampleFormatNotSupported; goto cleanup; }
            result = paUnanticipatedHostError; goto cleanup;
            }
        bytesPerFrame *= 4;             /* No need to set width for floats. */
        }
    else
        {
        if (alSetSampFmt(alc, AL_SAMPFMT_TWOSCOMP))
            {
            if (oserror() == AL_BAD_SAMPFMT)
                { result = paSampleFormatNotSupported; goto cleanup; }
            result = paUnanticipatedHostError; goto cleanup;
            }
        if (pasfmt == paInt8)
            {
            if (alSetWidth(alc, AL_SAMPLE_8))
                {
                if (oserror() == AL_BAD_WIDTH)
                    { result = paSampleFormatNotSupported; goto cleanup; }
                result = paUnanticipatedHostError; goto cleanup;
                }
            /* bytesPerFrame *= 1; */
            }
        else if (pasfmt == paInt16)
            {
            if (alSetWidth(alc, AL_SAMPLE_16))
                {
                if (oserror() == AL_BAD_WIDTH)
                    { result = paSampleFormatNotSupported; goto cleanup; }
                result = paUnanticipatedHostError; goto cleanup;
                }
            bytesPerFrame *= 2;
            }
        else if (pasfmt == paInt24)
            {
            if (alSetWidth(alc, AL_SAMPLE_24))
                {
                if (oserror() == AL_BAD_WIDTH)
                    { result = paSampleFormatNotSupported; goto cleanup; }
                result = paUnanticipatedHostError; goto cleanup;
                }
            bytesPerFrame *= 3;         /* OR 4 ??????????????????????????????????! */
            }
        else return paSampleFormatNotSupported;
        }
    /*----------------------- SET INTERNAL AL QUEUE SIZE: ------------------------------*/
    /* AL API doesn't provide a means for querying minimum and maximum buffer sizes.    */
    /* If the requested size fails, try a value closer to IRIX AL's default queue size. */
    default_iq_size = alGetQueueSize(alc);
    if (default_iq_size < 0)
        {
        DBUG(("Could not determine default internal queue size: %s.\n", alGetErrorString(oserror())));
        result = paUnanticipatedHostError; goto cleanup;
        }
    /* DBUG(("%s: suggested latency %.3f seconds\n", direction, pa_params->suggestedLatency)); */

    *iq_size = (int)(0.5 + (pa_params->suggestedLatency * (*samplerate)));  /* Based on REQUESTED sr! */
    if (*iq_size < (framesPerHostBuffer << 1))
        {
        DBUG(("Setting minimum queue size.\n"));
        *iq_size = (framesPerHostBuffer << 1); /* Make sure minimum is twice framesPerHostBuffer. */
        }
    d = *iq_size - default_iq_size; /* Determine whether we'll decrease or increase after failure. */
    while (alSetQueueSize(alc, *iq_size))                                     /* In sample frames. */
        {
        if (oserror() != AL_BAD_QSIZE)                              /* Stop at AL_BAD_CONFIG. */
            { result = paUnanticipatedHostError; goto cleanup; }
        dd = *iq_size - default_iq_size;                            /* Stop when even the default size failed   */
        if (((d >= 0) && (dd <= 0)) ||                              /* (dd=0), or when difference flipped sign. */
            ((d <= 0) && (dd >= 0)))       
            { result = paUnanticipatedHostError; goto cleanup; }
        DBUG(("Failed to set internal queue size to %d frames, ", *iq_size));
        if (d > 0)
            *iq_size -= framesPerHostBuffer;    /* Try lesser multiple framesPerHostBuffer. */
        else
            *iq_size += framesPerHostBuffer;    /* Try larger multiple framesPerHostBuffer. */
        DBUG(("trying %d frames...\n", *iq_size));
        }
    /* DBUG(("%s: alSetQueueSize(%d)\n", direction, *iq_size)); */

    /*----------------------- ALLOCATE HOST BUFFER: ------------------------------------*/
    hostPortBuff->buffer = PaUtil_AllocateMemory((long)bytesPerFrame * framesPerHostBuffer);
    if (!hostPortBuff->buffer)                            /* Caller is responsible for clean- */
        { result = paInsufficientMemory;; goto cleanup; } /* up and closing after failures!   */
    /*----------------------- BIND CONFIGURATION TO DEVICE: ----------------------------*/
    sgiDevice = sgiDeviceIDs[pa_params->device].i;
    if (alSetDevice(alc, sgiDevice)) /* Try to switch the hardware. */
        {
        alErr = oserror();
        DBUG(("Failed to configure device: %s.\n", alGetErrorString(alErr)));
        if (alErr == AL_BAD_DEVICE)
            { result = paInvalidDevice; goto cleanup; }
        result = paUnanticipatedHostError; goto cleanup;
        }
    /*----------------------- OPEN PORT: ----------------------------------------------*/
    hostPortBuff->port = alOpenPort(name, direction, alc);
    if (!hostPortBuff->port)
        {
        DBUG(("alOpenPort(r) failed: %s.\n", alGetErrorString(oserror())));
        result = paUnanticipatedHostError; goto cleanup;
        }

    if (direction[0] == 'w')             /* Pre-fill with requested latency amount. */
        alZeroFrames(hostPortBuff->port, *iq_size - framesPerHostBuffer);
    
                                                              /* Maybe set SR earlier? */
    /*----------------------- SET SAMPLERATE: -----------------------------------------*/
    pvs[0].param    = AL_MASTER_CLOCK;       /* Attempt to set a crystal-based sample- */
    pvs[0].value.i  = AL_CRYSTAL_MCLK_TYPE;  /* rate on input or output device.        */
    pvs[1].param    = AL_RATE;
    pvs[1].value.ll = alDoubleToFixed(*samplerate);
    if (2 != alSetParams(sgiDevice, pvs, 2))
        {
        DBUG(("alSetParams() failed: %s.\n", alGetErrorString(oserror())));
        result = paInvalidSampleRate; goto cleanup;
        }
    /*----------------------- GET ACTUAL SAMPLERATE: ---------------------------*/
    if (1 != alGetParams(sgiDevice, &pvs[1], 1))      /* SEE WHAT WE REALY SET IT TO. */
        {
        DBUG(("alGetParams('AL_RATE') failed: %s.\n", alGetErrorString(oserror())));
        result = paUnanticipatedHostError; goto cleanup;
        }
    *samplerate = alFixedToDouble(pvs[1].value.ll);   /* And return that to caller. */
    if (*samplerate < 0)
        {
        DBUG(("Samplerate could not be determined (name='%s').\n", name));
        result = paUnanticipatedHostError; goto cleanup;
        }
    /* DBUG(("set_sgi_device() succeeded.\n")); */
cleanup:
    if (alc)    /* We no longer need configuration. */
        alFreeConfig(alc);
    return result;
}

/**
    Called by OpenStream() if it fails and by CloseStream. Only used here, in this file.
    Fields MUST be set to NULL or to a valid value, prior to call.
*/
static void streamCleanupAndClose(PaSGIStream* stream)
{
    if (stream->hostPortBuffIn.port)    alClosePort(stream->hostPortBuffIn.port);         /* Close AL ports.  */
    if (stream->hostPortBuffIn.buffer)  PaUtil_FreeMemory(stream->hostPortBuffIn.buffer); /* Release buffers. */
    if (stream->hostPortBuffOut.port)   alClosePort(stream->hostPortBuffOut.port);
    if (stream->hostPortBuffOut.buffer) PaUtil_FreeMemory(stream->hostPortBuffOut.buffer);
}


/* See pa_hostapi.h for a list of validity guarantees made about OpenStream parameters. */
static PaError OpenStream(struct PaUtilHostApiRepresentation* hostApi,
                          PaStream**                          s,
                          const PaStreamParameters*           inputParameters,
                          const PaStreamParameters*           outputParameters,
                          double                              sampleRate,       /* Common to both i and o. */
                          unsigned long                       framesPerBuffer,
                          PaStreamFlags                       streamFlags,
                          PaStreamCallback*                   streamCallback,
                          void*                               userData)
{
    PaError                     result = paNoError;
    PaSGIHostApiRepresentation* SGIHostApi = (PaSGIHostApiRepresentation*)hostApi;
    PaSGIStream*                stream = 0;
    unsigned long               framesPerHostBuffer = framesPerBuffer; /* These may not be equivalent for all implementations! */
    int                         inputChannelCount, outputChannelCount, qf_in, qf_out;
    PaSampleFormat              inputSampleFormat, outputSampleFormat,
                                hostInputSampleFormat, hostOutputSampleFormat;
    double                      sr_in, sr_out;
    static const PaSampleFormat irixFormats = (paInt8 | paInt16 | paInt24 | paFloat32);
    /* Constant used by PaUtil_SelectClosestAvailableFormat(). Because IRIX AL does not
       provide a way to query for possible formats for a given device, interface or port,
       just add together the formats we know that are supported in general by IRIX AL 
       (at the end of the year 2003): AL_SAMPFMT_TWOSCOMP with AL_SAMPLE_8(=paInt8),
       AL_SAMPLE_16(=paInt16) or AL_SAMPLE_24(=paInt24); AL_SAMPFMT_FLOAT(=paFloat32); 
       AL_SAMPFMT_DOUBLE(=paFloat64); IRIX misses unsigned 8 and 32 bit signed ints.
    */
    /* DBUG(("OpenStream() started.\n")); */
    if (inputParameters)
        {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
        /* Unless alternate device specification is supported, reject the use of paUseHostApiSpecificDeviceSpecification. */
        if (inputParameters->device == paUseHostApiSpecificDeviceSpecification) /* DEVICE CHOOSEN BY CLIENT. */
            return paInvalidDevice;
        /* Check that input device can support inputChannelCount. */
        if (inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels)
            return paInvalidChannelCount;
        /* Validate inputStreamInfo. */
        if (inputParameters->hostApiSpecificStreamInfo)
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat(irixFormats, inputSampleFormat);
        /* Check if samplerate is supported for this input device. */
        result = sr_supported(SGIHostApi->sgiDeviceIDs[inputParameters->device].i, sampleRate);
        if (result != paFormatIsSupported)
            return result;
        }
    else
        {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paInt16; /* Surpress 'uninitialised var' warnings.       */
        }                                                    /* PaUtil_InitializeBufferProcessor is called   */
    if (outputParameters)                                    /* with these as args. Apparently, the latter 2 */
        {                                                    /* are not actually used when ChannelCount = 0. */
        outputChannelCount = outputParameters->channelCount;        
        outputSampleFormat = outputParameters->sampleFormat;
        if (outputParameters->device == paUseHostApiSpecificDeviceSpecification) /* Like input (above). */
            return paInvalidDevice;
        if (outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels)
            return paInvalidChannelCount;
        if (outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat(irixFormats, outputSampleFormat);
        /* Check if samplerate is supported for this output device. */
        result = sr_supported(SGIHostApi->sgiDeviceIDs[outputParameters->device].i, sampleRate);
        if (result != paFormatIsSupported)
            return result;
        }
    else
        {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paInt16; /* Surpress 'uninitialised var' warning. */
        }
    /*  It is guarenteed that inputParameters and outputParameters will never be both NULL.
        IMPLEMENT ME:
          + Check that input device can support inputSampleFormat, or that
            we have the capability to convert from outputSampleFormat to
            a native format. <taken care of by PaUtil_InitializeBufferProcessor().>
          + Check that output device can support outputSampleFormat, or that
            we have the capability to convert from outputSampleFormat to
            a native format. <taken care of by PaUtil_InitializeBufferProcessor().>
          + If a full duplex stream is requested, check that the combination
            of input and output parameters is supported. <In the IRIX AL, input and output devices are
            separate devices anyway (at least on Indy, running IRIX 6.5) so this is not an issue and
            if it is an impossible combination, we'll notice when we open the ports.>
          - Validate suggestedInputLatency and suggestedOutputLatency parameters,
            use default values where necessary.
    */

    if( (streamFlags & paPlatformSpecificFlags) != 0 )  /* Validate platform specific flags.  */
        return paInvalidFlag;                           /* Unexpected platform specific flag. */

    stream = (PaSGIStream*)PaUtil_AllocateMemory( sizeof(PaSGIStream) );
    if (!stream)
        { result = paInsufficientMemory; goto cleanup; }

    stream->hostPortBuffIn.port    = (ALport)NULL;       /* Ports closed.   */
    stream->hostPortBuffIn.buffer  =         NULL;       /* No buffers yet. */
    stream->hostPortBuffOut.port   = (ALport)NULL;
    stream->hostPortBuffOut.buffer =         NULL;

    if (streamCallback)
        PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation,
               &SGIHostApi->callbackStreamInterface, streamCallback, userData);
    else
        PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation,
               &SGIHostApi->blockingStreamInterface, streamCallback, userData);

    sr_in  = sr_out = sampleRate;
    qf_in  = qf_out = framesPerHostBuffer;
    /*----------------------------------------------------------------------------*/
    result = set_sgi_device(SGIHostApi->sgiDeviceIDs,   /* Array for alSetDevice and others. */
                            inputParameters,            /* Reads channelCount and device. */    
                            hostInputSampleFormat,      /* For alSetSampFmt and alSetWidth. */                            
                            "r",                        /* Direction "r" for reading. */
                            "portaudio in",             /* Name string. */
                            framesPerHostBuffer,
                            &sr_in,                     /* Receive actual rate after setting it. */
                            &qf_in,                     /* Actual queue size is received here!   */
                            &stream->hostPortBuffIn);   /* Receive ALport and input host buffer. */
    if (result != paNoError)
        goto cleanup;
    /* DBUG(("INPUT CONFIGURED.\n")); */
    /*----------------------------------------------------------------------------*/
    result = set_sgi_device(SGIHostApi->sgiDeviceIDs,
                            outputParameters,
                            hostOutputSampleFormat,
                            "w",                        /* "w" for writing. */
                            "portaudio out",
                            framesPerHostBuffer,
                            &sr_out,
                            &qf_out,
                            &stream->hostPortBuffOut);  /* ALWAYS PREFILLS. */
    if (result != paNoError)
        goto cleanup;
    /* DBUG(("OUTPUT CONFIGURED.\n")); */
    /* Pre-fill with silence (not necessarily 0) to realise the requested output latency.
    if (stream->hostPortBuffOut.port)            // Should never block. Always returns 0.
        alZeroFrames(stream->hostPortBuffOut.port, qf_out - framesPerHostBuffer);
     */
    /* Wait for the input buffer to fill to realise the requested input latency.
    if (stream->hostPortBuffIn.port)
        while (alGetFilled(stream->hostPortBuffIn.port) < (qf_in - framesPerHostBuffer))
            ;
     */
    /*----------------------------------------------------------------------------*/
    if (fabs(sr_in - sr_out) > 0.001)           /* Make sure both are the 'same'. */
        {
        DBUG(("Strange samplerate difference between input and output devices!\n"));
        result = paUnanticipatedHostError;
        goto cleanup;
        }
    sampleRate = sr_in;     /* == sr_out. */
                            /* Following fields set to estimated or actual values: */
    stream->streamRepresentation.streamInfo.inputLatency  = ((double)(qf_in  - framesPerHostBuffer)) * sr_in;  /* 0.0 if output only. */
    stream->streamRepresentation.streamInfo.outputLatency = ((double)(qf_out - framesPerHostBuffer)) * sr_out; /* 0.0 if input only.  */
    stream->streamRepresentation.streamInfo.sampleRate    = sampleRate;

    PaUtil_InitializeCpuLoadMeasurer(&stream->cpuLoadMeasurer, sampleRate);
    /*
        Assume a fixed host buffer size here. But the buffer processor
        can also support bounded and unknown host buffer sizes by passing 
        paUtilBoundedHostBufferSize or paUtilUnknownHostBufferSize instead of
        paUtilFixedHostBufferSize below. See pa_common/pa_process.h.
    */
    result = PaUtil_InitializeBufferProcessor(&stream->bufferProcessor,
                    inputChannelCount,   inputSampleFormat,  hostInputSampleFormat,
                    outputChannelCount,  outputSampleFormat, hostOutputSampleFormat,
                    sampleRate,          streamFlags,        framesPerBuffer,
                    framesPerHostBuffer, paUtilFixedHostBufferSize,
                    streamCallback,      userData);
    if (result != paNoError)
        { DBUG(("PaUtil_InitializeBufferProcessor()=%d!\n", result)); goto cleanup; }

    stream->framesPerHostCallback = framesPerHostBuffer;
    stream->state                 = PA_SGI_STREAM_FLAG_STOPPED_; /* After opening the stream is in the stopped state. */
    stream->stopAbort             = PA_SGI_REQ_CONT_;            /* 0. */
    
    *s = (PaStream*)stream;     /* Pass object to caller. */
cleanup:
    if (result != paNoError)    /* Always set result when jumping to cleanup after failure. */
        {
        if (stream)
            {
            streamCleanupAndClose(stream); /* Frees i/o buffers and closes AL ports. */
            PaUtil_FreeMemory(stream);
            }
        }
    return result;
}

/**
    POSIX thread that performs the actual i/o and calls the client's callback, spawned by StartStream().
*/
static void* PaSGIpthread(void *userData)
{
    PaSGIStream*  stream = (PaSGIStream*)userData;
    int           callbackResult = paContinue;
    double        nanosec_per_frame;

    stream->state = PA_SGI_STREAM_FLAG_ACTIVE_; /* Parent thread also sets active, but we make no assumption */
                                                /* about who does this first (the parent thread, probably).  */
    nanosec_per_frame = 1000000000.0 / stream->streamRepresentation.streamInfo.sampleRate;
    while (!stream->stopAbort)                  /* Exit loop immediately when 'stop' or 'abort' are raised.  */
        {
        PaStreamCallbackTimeInfo  timeInfo;
        unsigned long             framesProcessed;
        stamp_t                   fn, t, fnd, td;   /* Unsigned 64 bit. */
        
        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
        /* IMPLEMENT ME: - handle buffer slips. */
        if (stream->hostPortBuffIn.port)
            {
            /*  Get device sample frame number associated with next audio sample frame
                we're going to read from this port. */
            alGetFrameNumber(stream->hostPortBuffIn.port, &fn);
            /*  Get some recent pair of (frame number, time) from the audio device to 
                which our port is connected. time is 'UST' which is given in nanoseconds 
                and shared with the other audio devices and with other media. */
            alGetFrameTime(stream->hostPortBuffIn.port, &fnd, &td);
            /*  Calculate UST associated with fn, the next sample frame we're going to read or
                write. Because this is signed arithmetic, code works for both inputs and outputs. */
            t = td + (stamp_t) ((double)(fn - fnd) * nanosec_per_frame);
            /*  If port is not in underflow or overflow state, we can alReadFrames() or 
                alWriteFrames() here and know that t is the time associated with the first 
                sample frame of the buffer we read or write. */
            timeInfo.inputBufferAdcTime = ((PaTime)t) / 1000000000.0;
            
            /* Read interleaved samples from ALport (I think it will block only the first time). */
            alReadFrames(stream->hostPortBuffIn.port, stream->hostPortBuffIn.buffer, stream->framesPerHostCallback);
            }
        if (stream->hostPortBuffOut.port)
            {
            alGetFrameNumber(stream->hostPortBuffOut.port, &fn);
            alGetFrameTime(stream->hostPortBuffOut.port, &fnd, &td);
            t = td + (stamp_t) ((double)(fn - fnd) * nanosec_per_frame);
            timeInfo.outputBufferDacTime = ((PaTime)t) / 1000000000.0;
            }
        dmGetUST((unsigned long long*)(&t));                /* Receive time in nanoseconds in t. */
        timeInfo.currentTime = ((PaTime)t) / 1000000000.0;

        /* If you need to byte swap or shift inputBuffer to convert it into a pa format, do it here. */
        PaUtil_BeginBufferProcessing(&stream->bufferProcessor, &timeInfo,
                                     0 /* IMPLEMENT ME: pass underflow/overflow flags when necessary */);
                                     
        if (stream->hostPortBuffIn.port)                    /* Equivalent to (inputChannelCount > 0) */
            {                /* We are sure about the amount to transfer (PaUtil_Set before alRead). */
            PaUtil_SetInputFrameCount(&stream->bufferProcessor, 0 /* 0 means take host buffer size */);
            PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor,
                    0, /* first channel of inputBuffer is channel 0 */
                    stream->hostPortBuffIn.buffer,
                    0 ); /* 0 - use inputChannelCount passed to init buffer processor */
            }
        if (stream->hostPortBuffOut.port)
            {
            PaUtil_SetOutputFrameCount(&stream->bufferProcessor, 0 /* 0 means take host buffer size */);
            PaUtil_SetInterleavedOutputChannels(&stream->bufferProcessor,
                    0, /* first channel of outputBuffer is channel 0 */
                    stream->hostPortBuffOut.buffer,
                    0 ); /* 0 - use outputChannelCount passed to init buffer processor */
            }
        /*
            You must pass a valid value of callback result to PaUtil_EndBufferProcessing()
            in general you would pass paContinue for normal operation, and
            paComplete to drain the buffer processor's internal output buffer.
            You can check whether the buffer processor's output buffer is empty
            using PaUtil_IsBufferProcessorOuputEmpty( bufferProcessor )
        */
        framesProcessed = PaUtil_EndBufferProcessing(&stream->bufferProcessor, &callbackResult);
        /*
            If you need to byte swap or shift outputBuffer to convert it to host format, do it here.
        */
        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
        if (callbackResult != paContinue)
            {                                              /* Once finished, call the finished callback. */
            if (stream->streamRepresentation.streamFinishedCallback)
                stream->streamRepresentation.streamFinishedCallback(stream->streamRepresentation.userData);
            if (callbackResult == paAbort)
                {
                /* DBUG(("CallbackResult == paAbort (finish playback immediately).\n")); */
                stream->stopAbort = PA_SGI_REQ_ABORT_;
                break; /* Don't play the last buffer returned. */
                }
            else /* paComplete or some other non-zero value. */
                {
                /* DBUG(("CallbackResult != 0 (finish playback after last buffer).\n")); */
                stream->stopAbort = PA_SGI_REQ_STOP_;
                }
            }
        /* Write interleaved samples to SGI device (like unix_oss, AFTER checking callback result). */
        if (stream->hostPortBuffOut.port)
            alWriteFrames(stream->hostPortBuffOut.port, stream->hostPortBuffOut.buffer, stream->framesPerHostCallback);
        }
    if (stream->hostPortBuffOut.port) /* Drain output buffer(s), as long as we don't see an 'abort' request. */
        {
        while ((!(stream->stopAbort & PA_SGI_REQ_ABORT_)) &&    /* Assume _STOP_ is set (or meant). */
               (alGetFilled(stream->hostPortBuffOut.port) > 1)) /* In case of _ABORT_ we quickly leave (again). */
            ; /* Don't provide any new (not even silent) samples, but let an underrun [almost] occur. */
        }
    if (callbackResult != paContinue)
        stream->state = PA_SGI_STREAM_FLAG_FINISHED_;
    return NULL;
}


/*
    When CloseStream() is called, the multi-api layer ensures
    that the stream has already been stopped or aborted.
*/
static PaError CloseStream(PaStream* s)
{
    PaError       result = paNoError;
    PaSGIStream*  stream = (PaSGIStream*)s;

    /* DBUG(("SGI CloseStream() started.\n")); */
    streamCleanupAndClose(stream); /* Releases i/o buffers and closes AL ports. */
    PaUtil_TerminateBufferProcessor(&stream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation(&stream->streamRepresentation);
    PaUtil_FreeMemory(stream);
    return result;
}


static PaError StartStream(PaStream *s)
{
    PaError       result = paNoError;
    PaSGIStream*  stream = (PaSGIStream*)s;

    PaUtil_ResetBufferProcessor(&stream->bufferProcessor); /* See pa_common/pa_process.h. */
    if (stream->bufferProcessor.streamCallback)
        {                                       /* only when callback is used */
        if (pthread_create(&stream->thread,
                           NULL,                /* pthread_attr_t * attr */
                           PaSGIpthread,        /* Function to spawn.    */
                           (void*)stream))      /* Pass stream as arg.   */
            {
            DBUG(("pthread_create() failed!\n"));
            result = paUnanticipatedHostError;
            }
        else
            {
            stream->state = PA_SGI_STREAM_FLAG_ACTIVE_; /* Set active before returning from this function. */
            }
        }
    else
        stream->state = PA_SGI_STREAM_FLAG_ACTIVE_; /* Apparently, setting active for blocking i/o is */
    return result;                                  /* necessary (for patest_write_sine for example). */
}


static PaError StopStream( PaStream *s )
{
    PaError         result = paNoError;
    PaSGIStream*    stream = (PaSGIStream*)s;
    
    /* DBUG(("SGI StopStream() started.\n")); */
    if (stream->bufferProcessor.streamCallback) /* Only for callback streams. */
        {
        stream->stopAbort = PA_SGI_REQ_STOP_;   /* Signal and wait for the thread to drain output buffers. */
        if (pthread_join(stream->thread, NULL)) /* When succesful, stream->state */
            {                                   /* is still ACTIVE, or FINISHED. */
            DBUG(("pthread_join() failed!\n"));
            result = paUnanticipatedHostError;
            }
        else  /* Transition from ACTIVE or FINISHED to STOPPED. */
            stream->state = PA_SGI_STREAM_FLAG_STOPPED_;
        stream->stopAbort = PA_SGI_REQ_CONT_; /* For possible next start. */
        }
/*  else
        stream->state = PA_SGI_STREAM_FLAG_STOPPED_;  Is this necessary for blocking i/o? */
    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaSGIStream *stream = (PaSGIStream*)s;

    /* DBUG(("SGI AbortStream() started.\n")); */
    if (stream->bufferProcessor.streamCallback) /* Only for callback streams. */
        {
        stream->stopAbort = PA_SGI_REQ_ABORT_;
        if (pthread_join(stream->thread, NULL))
            {
            DBUG(("pthread_join() failed!\n"));
            result = paUnanticipatedHostError;
            }
        else  /* Transition from ACTIVE or FINISHED to STOPPED. */
            stream->state = PA_SGI_STREAM_FLAG_STOPPED_;
        stream->stopAbort = PA_SGI_REQ_CONT_; /* For possible next start. */
        }
/*  else
        stream->state = PA_SGI_STREAM_FLAG_STOPPED_;  Is this necessary for blocking i/o? */
    return result;
}


static PaError IsStreamStopped( PaStream *s )   /* Not just the opposite of stream->active! */
{                                               /* In the 'callback finished' state, it     */
    PaSGIStream *stream = (PaSGIStream*)s;      /* should return zero instead of nonzero!   */
    return (stream->state & PA_SGI_STREAM_FLAG_STOPPED_);
}


static PaError IsStreamActive( PaStream *s )
{
    PaSGIStream *stream = (PaSGIStream*)s;
    return (stream->state & PA_SGI_STREAM_FLAG_ACTIVE_);
}


static PaTime GetStreamTime( PaStream *s )
{
    stamp_t       t;
    
    (void) s; /* Suppress unused argument warning. */
    dmGetUST((unsigned long long*)(&t)); /* Receive time in nanoseconds in t. */
    return (PaTime)t / 1000000000.0;
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaSGIStream *stream = (PaSGIStream*)s;

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
    PaSGIStream*    stream = (PaSGIStream*)s;
    int             n;

printf("stream->framesPerHostCallback=%ld.\n", stream->framesPerHostCallback);
fflush(stdout);

    while (frames)
        {
        if (frames > stream->framesPerHostCallback) n = stream->framesPerHostCallback;
        else                                        n = frames;
        /* Read interleaved samples from SGI device. */
        alReadFrames(stream->hostPortBuffIn.port,           /* Port already opened by OpenStream(). */
                     stream->hostPortBuffIn.buffer, n);     /* Already allocated by OpenStream().   */
                                                            /* alReadFrames() always returns 0.     */
        PaUtil_SetInputFrameCount(&stream->bufferProcessor, 0); /* 0 means take host buffer size */
        PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor,
                                           0,   /* first channel of inputBuffer is channel 0 */
                                           stream->hostPortBuffIn.buffer,
                                           0 ); /* 0 means use inputChannelCount passed at init. */
        /* Copy samples from host input channels set up by the PaUtil_SetInterleavedInputChannels 
           to a user supplied buffer. */
printf("frames=%ld, buffer=%ld\n", frames, (long)buffer);
fflush(stdout);
        PaUtil_CopyInput(&stream->bufferProcessor, &buffer, n);
        frames -= n;
        }
printf("DONE: frames=%ld, buffer=%ld\n", frames, (long)buffer);
    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaSGIStream*    stream = (PaSGIStream*)s;
    unsigned long   n;
    while (frames)
        {
        PaUtil_SetOutputFrameCount(&stream->bufferProcessor, 0); /* 0 means take host buffer size */
        PaUtil_SetInterleavedOutputChannels(&stream->bufferProcessor,
                                            0,   /* first channel of inputBuffer is channel 0 */
                                            stream->hostPortBuffOut.buffer,
                                            0 ); /* 0 means use inputChannelCount passed at init. */
        /* Copy samples from user supplied buffer to host input channels set up by
           PaUtil_SetInterleavedOutputChannels. Copies the minimum of the number of user frames 
           (specified by the frameCount parameter) and the number of host frames (specified in 
           a previous call to SetOutputFrameCount()). */
        n = PaUtil_CopyOutput(&stream->bufferProcessor, &buffer, frames);
        /* Write interleaved samples to SGI device. */
        alWriteFrames(stream->hostPortBuffOut.port, stream->hostPortBuffOut.buffer, n);
        frames -= n;                                           /* alWriteFrames always returns 0. */
        }
    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    return (signed long)alGetFilled(((PaSGIStream*)s)->hostPortBuffIn.port);
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    return (signed long)alGetFillable(((PaSGIStream*)s)->hostPortBuffOut.port);
}


/* CVS reminder:
   To download the 'v19-devel' branch from portaudio's CVS server for the first time, type:
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs checkout -r v19-devel portaudio
   Then 'cd' to the 'portaudio' directory that should have been created.
   Example that logs in as 'pieter' and commit edits (will require password):
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs login
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs commit -m 'blabla.' -r v19-devel pa_sgi/pa_sgi.c
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs logout
   To see if someone else worked on something:
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs update -r v19-devel
   To see logs:
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs log pa_common/pa_skeleton.c
*/
