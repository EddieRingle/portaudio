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

 @note Started as a copy of pa_skeleton.c (v 1.1.2.35 2003/09/20).
 Nothing to do with the old V18 pa_sgi version: using the newer AL calls now and 
 pthreads instead of sproc. A fresh start.
 
 Tested: - pa_devs                ok, but where does that list of samplerates come from?
         - pa_fuzz                ok
         - patest_sine            test has to be adapted: (usleep > 1000000)?
         - patest_leftright       ok
         - patest_sine_formats    ok
 
 Todo:  - Find out why Pa_sleep doesn't work (probably us > 1000000).
        - Set queue sizes and latencies.
        - Implement blocking i/o properly.
*/

#include <string.h>         /* strlen() */
#include <stdio.h>          /* printf() */
#include <math.h>           /* fabs()   */

#include <dmedia/audio.h>   /* IRIX AL (audio library). */
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
    TODO: Limit standard supported samplerates for digital i/o! Where does this list come from
          anyway (showed by bin/pa_devs)?
          Set latencies.
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

    DBUG(("PaSGI_Initialize() started.\n"));

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
                                       
        y[0].param = AL_NAME;
        y[0].value.ptr = devName;
        y[0].sizeIn = maxDevNameChars;  /* Including terminating null char. */
        y[1].param = AL_RATE;
        y[2].param = AL_CHANNELS;
        y[3].param = AL_TYPE;           /* Subtype of AL_INPUT_DEVICE_TYPE or AL_OUTPUT_DEVICE_TYPE? */
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

            deviceInfo->defaultLowInputLatency   = 0.;  /* IMPLEMENT ME */
            deviceInfo->defaultLowOutputLatency  = 0.;  /* IMPLEMENT ME */
            deviceInfo->defaultHighInputLatency  = 0.;  /* IMPLEMENT ME */
            deviceInfo->defaultHighOutputLatency = 0.;  /* IMPLEMENT ME */

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

    DBUG(("Terminate() started.\n"));

    /* Clean up any resources not handled by the allocation group. */

    if( SGIHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( SGIHostApi->allocations );
        PaUtil_DestroyAllocationGroup( SGIHostApi->allocations );
    }
    PaUtil_FreeMemory( SGIHostApi );
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


    /* suppress unused variable warnings */
    (void) sampleRate;
    (void) inputSampleFormat;
    (void) outputSampleFormat;

    return paFormatIsSupported;
}


typedef struct PaSGIportBuffer  /* Auxilary struct, used for inputs as well as outputs. */
{
    ALport  port;               /* NULL means AL port closed. */
    void*   buffer;             /* NULL means not allocated.  */
}
    PaSGIportBuffer;

typedef struct PaSGIStream      /* Stream data structure specifically for this implementation. */
{
    PaUtilStreamRepresentation  streamRepresentation;
    PaUtilCpuLoadMeasurer       cpuLoadMeasurer;
    PaUtilBufferProcessor       bufferProcessor;
    unsigned long               framesPerHostCallback;  /* Implementation specific data. */
    PaSGIportBuffer             portBuffIn,
                                portBuffOut;
    unsigned char               stopSoon,               /* RT flags. */
                                stopNow,
                                isActive;
    pthread_t                   thread;
}
    PaSGIStream;


/*
    Called by OpenStream() once or twice. Argument alc should point to an already allocated 
    AL configuration structure. First, the number of channels and the sampleformat is configured.
    The configuration is then bound to the specified AL device. Then an AL port is opened.
    Finally, the samplerate of the device is altered (or at least set again).
*/
static PaError set_sgi_device(ALvalue*                  sgiDeviceIDs,
                              const PaStreamParameters* pa_params,      /* read device and channels. */                             
                              PaSampleFormat            pasfmt,         /* Don't read from pa_params!. */
                              ALconfig                  alc,
                              char*                     direction,      /* "r" or "w". */
                              char*                     name,
                              long                      framesPerHostBuffer,
                              double*                   samplerate,     /* Also write back here.  */
                              PaSGIportBuffer*          portBuff)       /* Receive pointers here. */
{
    int     bytesPerFrame, sgiDevice;
    ALpv    pvs[2];

    if (!pa_params)                   /* Not an error, just not full duplex, skip all. */
        return paNoError;
    if (!pa_params->channelCount)
        return paNoError;
    /*----------------------- CONFIGURE CHANNELS: -------------------------------------*/
    if (alSetChannels(alc, pa_params->channelCount))          /* Returns 0 on success. */
        {
        if (oserror() == AL_BAD_CHANNELS) return paInvalidChannelCount;
        return paUnanticipatedHostError;
        }
    bytesPerFrame = pa_params->channelCount;          /* Is multiplied by width below. */
    /*----------------------- CONFIGURE SAMPLEFORMAT: ---------------------------------*/
    if (pasfmt == paFloat32)
        {
        if (alSetSampFmt(alc, AL_SAMPFMT_FLOAT))
            {
            if (oserror() == AL_BAD_SAMPFMT) return paSampleFormatNotSupported;
            return paUnanticipatedHostError;
            }
        bytesPerFrame *= 4;             /* No need to set width for floats. */
        }
    else
        {
        if (alSetSampFmt(alc, AL_SAMPFMT_TWOSCOMP))
            {
            if (oserror() == AL_BAD_SAMPFMT) return paSampleFormatNotSupported;
            return paUnanticipatedHostError;
            }
        if (pasfmt == paInt8)
            {
            if (alSetWidth(alc, AL_SAMPLE_8))
                {
                if (oserror() == AL_BAD_WIDTH) return paSampleFormatNotSupported;
                return paUnanticipatedHostError;
                }
            /* bytesPerFrame *= 1; */
            }
        else if (pasfmt == paInt16)
            {
            if (alSetWidth(alc, AL_SAMPLE_16))
                {
                if (oserror() == AL_BAD_WIDTH) return paSampleFormatNotSupported;
                return paUnanticipatedHostError;
                }
            bytesPerFrame *= 2;
            }
        else if (pasfmt == paInt24)
            {
            if (alSetWidth(alc, AL_SAMPLE_24))
                {
                if (oserror() == AL_BAD_WIDTH) return paSampleFormatNotSupported;
                return paUnanticipatedHostError;
                }
            bytesPerFrame *= 3;
            }
        else return paSampleFormatNotSupported;
        }
    
    /*----------------------- ALLOCATE HOST BUFFER: -----------------------------------*/
    portBuff->buffer = PaUtil_AllocateMemory((long)bytesPerFrame * framesPerHostBuffer);
    if (!portBuff->buffer)                         /* Caller is responsible for clean- */
        return paInsufficientMemory;               /* up and closing after failures!   */
    /*----------------------- BIND CONFIGURATION TO DEVICE: ---------------------------*/
    sgiDevice = sgiDeviceIDs[pa_params->device].i;
    if (alSetDevice(alc, sgiDevice)) /* Try to switch the hardware. */
        {
        int alErr = oserror();
        DBUG(("Failed to configure device: %s.\n", alGetErrorString(alErr)));
        if (alErr == AL_BAD_DEVICE) return paInvalidDevice;
        return paUnanticipatedHostError;
        }
    /*----------------------- OPEN PORT: ----------------------------------------------*/
    portBuff->port = alOpenPort(name, direction, alc);
    if (!portBuff->port)
        {
        DBUG(("alOpenPort(r) failed: %s.\n", alGetErrorString(oserror())));
        return paUnanticipatedHostError;
        }                                                     /* Maybe set SR earlier? */
    /*----------------------- SET SAMPLERATE: -----------------------------------------*/
    pvs[0].param    = AL_MASTER_CLOCK;       /* Attempt to set a crystal-based sample- */
    pvs[0].value.i  = AL_CRYSTAL_MCLK_TYPE;  /* rate on input or output device.        */
    pvs[1].param    = AL_RATE;
    pvs[1].value.ll = alDoubleToFixed(*samplerate);
    if (2 != alSetParams(sgiDevice, pvs, 2))
        {
        DBUG(("alSetParams() failed: %s.\n", alGetErrorString(oserror())));
        return paInvalidSampleRate;
        }        
    if (1 != alGetParams(sgiDevice, &pvs[1], 1))      /* SEE WHAT WE REALY SET IT TO. */
        {
        DBUG(("alGetParams('AL_RATE') failed: %s.\n", alGetErrorString(oserror())));
        return paUnanticipatedHostError;
        }
    *samplerate = alFixedToDouble(pvs[1].value.ll);   /* And return that to caller. */
    if (*samplerate < 0)
        {
        DBUG(("Samplerate could not be determined (name='%s').\n", name));
        return paUnanticipatedHostError;
        }
    /* DBUG(("set_sgi_device() succeeded.\n")); */
    return paNoError;
}

/*
    Called by OpenStream() if it fails and by CloseStream. Only used here, in this file.
    Fields MUST be set to NULL or to a valid value, prior to call.
*/
static void streamCleanupAndClose(PaSGIStream* stream)
{
    if (stream->portBuffIn.port)          alClosePort(stream->portBuffIn.port);   /* Close AL ports.  */
    if (stream->portBuffIn.buffer)  PaUtil_FreeMemory(stream->portBuffIn.buffer); /* Release buffers. */
    if (stream->portBuffOut.port)         alClosePort(stream->portBuffOut.port);
    if (stream->portBuffOut.buffer) PaUtil_FreeMemory(stream->portBuffOut.buffer);
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
    unsigned long               framesPerHostBuffer = framesPerBuffer; /* these may not be equivalent for all implementations */
    int                         inputChannelCount, outputChannelCount;
    PaSampleFormat              inputSampleFormat, outputSampleFormat;
    PaSampleFormat              hostInputSampleFormat, hostOutputSampleFormat;
    ALconfig                    alc = NULL;
    double                      sr_in, sr_out;
    static const PaSampleFormat irixFormats = (paInt8 | paInt16 | paInt24 | paFloat32);
                                /* Constant used by PaUtil_SelectClosestAvailableFormat(). Because
                                   IRIX AL does not provide a way to query for possible formats
                                   for a given device, interface or port. I'll just add together the formats 
                                   I know that are supported in general by IRIX AL at the year 2003:
        AL_SAMPFMT_TWOSCOMP with AL_SAMPLE_8(=paInt8), AL_SAMPLE_16(=paInt16) or AL_SAMPLE_24(=paInt24);
        AL_SAMPFMT_FLOAT(=paFloat32); AL_SAMPFMT_DOUBLE(=paFloat64); IRIX misses unsigned 8 and 32 bit signed ints.
                                */
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
        }
    else
        {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = 0; /* Surpress uninitialised warning. */
        }
    if (outputParameters)
        {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
        if (outputParameters->device == paUseHostApiSpecificDeviceSpecification) /* Like input (above). */
            return paInvalidDevice;
        if (outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels)
            return paInvalidChannelCount;
        if (outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat(irixFormats, outputSampleFormat);
        }
    else
        {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = 0; /* Surpress uninitialised warning. */
        }
    /*
        IMPLEMENT ME:
        (Following two checks are taken care of by PaUtil_InitializeBufferProcessor() FIXME - checks needed?)

            + check that input device can support inputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            + check that output device can support outputSampleFormat, or that
                we have the capability to convert from outputSampleFormat to
                a native format

            - if a full duplex stream is requested, check that the combination
                of input and output parameters is supported (not an issue on IRIX? we'll notice when we open port?...)

            - check that the device supports sampleRate (we'll notice when we try to set, or earlier, at config)

            - validate suggestedInputLatency and suggestedOutputLatency parameters,
                use default values where necessary
    */

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */

    stream = (PaSGIStream*)PaUtil_AllocateMemory( sizeof(PaSGIStream) );
    if (!stream)
        { result = paInsufficientMemory; goto cleanup; }
    stream->portBuffIn.port    = (ALport)NULL;       /* Ports closed.   */
    stream->portBuffIn.buffer  =         NULL;       /* No buffers yet. */
    stream->portBuffOut.port   = (ALport)NULL;
    stream->portBuffOut.buffer =         NULL;

    if (streamCallback)
        PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation,
                                              &SGIHostApi->callbackStreamInterface, streamCallback, userData);
    else
        PaUtil_InitializeStreamRepresentation(&stream->streamRepresentation,
                                              &SGIHostApi->blockingStreamInterface, streamCallback, userData);
    alc = alNewConfig();    /* Create a config. This defaults to stereo, 16-bit integer data. */
    if (!alc)               /* Call alFreeConfig() when done with it. */
        { result = paInsufficientMemory; goto cleanup; }
    sr_in  = 
    sr_out = sampleRate;
    result = set_sgi_device(SGIHostApi->sgiDeviceIDs,   /* Array for alSetDevice and others. */
                            inputParameters,            /* Reads channelCount and device. */    
                            hostInputSampleFormat,      /* For alSetSampFmt and alSetWidth. */                            
                            alc,                        /* For AL calls. */
                            "r",                        /* Direction "r" for reading. */
                            "portaudio input",          /* Name string. */
                            framesPerHostBuffer,
                            &sr_in,                     /* Receive actual rate after setting it. */
                            &stream->portBuffIn);       /* Receive ALport and input host buffer. */
    if (result != paNoError)
        goto cleanup;
    result = set_sgi_device(SGIHostApi->sgiDeviceIDs,
                            outputParameters,
                            hostOutputSampleFormat,
                            alc,
                            "w",                        /* "w" for reading. */
                            "portaudio ouput",
                            framesPerHostBuffer,
                            &sr_out,
                            &stream->portBuffOut);
    if (result != paNoError)
        goto cleanup;
    if (fabs(sr_in - sr_out) > 0.01) /* Let us make sure both are the same. */
        {
        DBUG(("Strange samplerate difference between input and output devices!\n"));
        result = paUnanticipatedHostError;
        goto cleanup;
        }
    sampleRate = sr_in;             /* == sr_out.      Overwrite argument with actual sample rate. */
                                    /* Following fields should contain estimated or actual values: */
    stream->streamRepresentation.streamInfo.inputLatency  = 0.;
    stream->streamRepresentation.streamInfo.outputLatency = 0.;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    PaUtil_InitializeCpuLoadMeasurer(&stream->cpuLoadMeasurer, sampleRate);
    /*
        Assume a fixed host buffer size in this example, but the buffer processor
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
    stream->isActive = 0;
    stream->stopSoon = 0;
    stream->stopNow  = 0;
    *s = (PaStream*)stream;     /* Pass object to caller. */
cleanup:
    if (alc)                    /* We no longer need configuration. */
        alFreeConfig(alc);
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

/*
    POSIX thread that performs i/o and calls client's callback, spawned by StartStream().
*/
static void* PaSGIpthread(void *userData)
{
    PaSGIStream* stream = (PaSGIStream*)userData;
   
    stream->isActive = 1;
    while (!(stream->stopNow | stream->stopSoon))
        {
        PaStreamCallbackTimeInfo timeInfo = {0,0,0}; /* IMPLEMENT ME */
        int callbackResult;
        unsigned long framesProcessed;
        
        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
        /* IMPLEMENT ME: - generate timing information
                         - handle buffer slips
        */
        /* If you need to byte swap or shift inputBuffer to convert it into a pa format, do it here. */
        PaUtil_BeginBufferProcessing(&stream->bufferProcessor, &timeInfo,
                                     0 /* IMPLEMENT ME: pass underflow/overflow flags when necessary */);
                                     
        if (stream->portBuffIn.port)                        /* Equivalent to (inputChannelCount > 0) */
            {                /* We are sure about the amount to transfer (PaUtil_Set before alRead). */
            PaUtil_SetInputFrameCount(&stream->bufferProcessor, 0 /* 0 means take host buffer size */);
            PaUtil_SetInterleavedInputChannels(&stream->bufferProcessor,
                    0, /* first channel of inputBuffer is channel 0 */
                    stream->portBuffIn.buffer,
                    0 ); /* 0 - use inputChannelCount passed to init buffer processor */
            /* Read interleaved samples from ALport (alReadFrames() may block the first time?). */
            alReadFrames(stream->portBuffIn.port, stream->portBuffIn.buffer, stream->framesPerHostCallback);
            }
        if (stream->portBuffOut.port)
            {
            PaUtil_SetOutputFrameCount(&stream->bufferProcessor, 0 /* 0 means take host buffer size */);
            PaUtil_SetInterleavedOutputChannels(&stream->bufferProcessor,
                    0, /* first channel of outputBuffer is channel 0 */
                    stream->portBuffOut.buffer,
                    0 ); /* 0 - use outputChannelCount passed to init buffer processor */
            }
        /*
            You must pass a valid value of callback result to PaUtil_EndBufferProcessing()
            in general you would pass paContinue for normal operation, and
            paComplete to drain the buffer processor's internal output buffer.
            You can check whether the buffer processor's output buffer is empty
            using PaUtil_IsBufferProcessorOuputEmpty( bufferProcessor )
        */
        callbackResult = paContinue;
        framesProcessed = PaUtil_EndBufferProcessing(&stream->bufferProcessor, &callbackResult);
        /*
            If you need to byte swap or shift outputBuffer to convert it to host format, do it here.
        */
        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
        if( callbackResult == paContinue )
            {
            /* nothing special to do */
            }
        else if( callbackResult == paAbort )
            {
            DBUG(("CallbackResult == paAbort (finish playback immediately).\n"));
            /* once finished, call the finished callback */
            if (stream->streamRepresentation.streamFinishedCallback != 0 )
                stream->streamRepresentation.streamFinishedCallback(stream->streamRepresentation.userData);
            break;
            }
        else /* paComplete or some other non-zero value. */
            {
            DBUG(("CallbackResult != 0 (finish playback after last buffer).\n"));
            /* once finished, call the finished callback */
            if (stream->streamRepresentation.streamFinishedCallback != 0 )
                stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
            stream->stopSoon = 1;
            }
        /* Write interleaved samples to SGI device (like unix_oss, AFTER checking callback result). */
        if (stream->portBuffOut.port)
            alWriteFrames(stream->portBuffOut.port, stream->portBuffOut.buffer, stream->framesPerHostCallback);
        }
    stream->isActive = 0;
    return NULL;
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream(PaStream* s)
{
    PaError       result = paNoError;
    PaSGIStream*  stream = (PaSGIStream*)s;

    streamCleanupAndClose(stream); /* Frees i/o buffers and closes AL ports. */
    
    PaUtil_TerminateBufferProcessor(&stream->bufferProcessor);
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation);
    PaUtil_FreeMemory(stream);

    return result;
}


static PaError StartStream(PaStream *s)
{
    PaError         result = paNoError;
    PaSGIStream*    stream = (PaSGIStream*)s;

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
        }
    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError         result = paNoError;
    PaSGIStream*    stream = (PaSGIStream*)s;

    stream->stopSoon = 1;
    if (stream->bufferProcessor.streamCallback) /* Only for callback streams. */
        {
        if (pthread_join(stream->thread, NULL))
            {
            DBUG(("pthread_join() failed!\n"));
            result = paUnanticipatedHostError;
            }
        }
    stream->stopSoon = 0;
    DBUG(("PaSGI StopStream().\n"));
    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaSGIStream *stream = (PaSGIStream*)s;

    stream->stopNow = 1;
    if (stream->bufferProcessor.streamCallback) /* Only for callback streams. */
        {
        if (pthread_join(stream->thread, NULL))
            {
            DBUG(("pthread_join() failed!\n"));
            result = paUnanticipatedHostError;
            }
        }
    stream->stopNow = 0;
    DBUG(("PaSGI StopStream().\n"));
    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaSGIStream *stream = (PaSGIStream*)s;
    return (!stream->isActive);
}


static PaError IsStreamActive( PaStream *s )
{
    PaSGIStream *stream = (PaSGIStream*)s;
    return (stream->isActive);
}


static PaTime GetStreamTime( PaStream *s )
{
    PaSGIStream *stream = (PaSGIStream*)s;

    /* suppress unused variable warnings */
    (void) stream;
    
    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
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
    PaSGIStream *stream = (PaSGIStream*)s;

    /* Byte swapping and conversion?......   */
    /* Read interleaved samples from device. */
    alReadFrames(stream->portBuffIn.port, buffer, frames);
    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaSGIStream *stream = (PaSGIStream*)s;

    /* Write interleaved samples to device. */
    alWriteFrames(stream->portBuffOut.port, (void*)buffer, frames);
    /* Byte swapping and conversion?......  */
    
    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaSGIStream *stream = (PaSGIStream*)s;

    return (signed long)alGetFilled(stream->portBuffIn.port);
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaSGIStream *stream = (PaSGIStream*)s;

    return (signed long)alGetFillable(stream->portBuffOut.port);
}

/*------------------ for people with bad long- and short-term bio-mem: -----------
  To download (co means checkout) 'v19-devel' branch from portaudio's CVS server:
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs co -r v19-devel portaudio
  Then 'cd' to the 'portaudio' directory that should have been created.
  Login as 'pieter' and commit edits (requires password):
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs login
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs commit -m 'V19 fix for self-finishing callback.' -r v19-devel pa_sgi/pa_sgi.c
    cvs -d:pserver:pieter@www.portaudio.com:/home/cvs logout
  To see if someone else worked on something:
    cvs -d:pserver:anonymous@www.portaudio.com:/home/cvs update -r v19-devel
*/
