/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 * OSS implementation by:
 *   Douglas Repetto
 *   Phil Burk
 *   Dominic Mazzoni
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
#include <string.h>
#include <math.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <alloca.h>
#include <malloc.h>
#include <assert.h>

#ifdef __linux__
# include <linux/soundcard.h>
# define DEVICE_NAME_BASE            "/dev/dsp"
#else
# include <machine/soundcard.h> /* JH20010905 */
# define DEVICE_NAME_BASE            "/dev/audio"
#endif

#include "portaudio.h"
#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"
#include "../pa_unix/pa_unix_util.h"

/* TODO: add error text handling
#define PA_UNIX_OSS_ERROR( errorCode, errorText ) \
    PaUtil_SetLastHostErrorInfo( paInDevelopment, errorCode, errorText )
*/

static int sysErr_;

/* Check return value of system call, and map it to PaError */
#define ENSURE_(expr, code) \
    do { \
        if( UNLIKELY( (sysErr_ = (expr)) < 0 ) ) \
        { \
            /* PaUtil_SetLastHostErrorInfo should only be used in the main thread */ \
            /*
            if( (code) == paUnanticipatedHostError pthread_self() == mainThread_ ) \
            { \
                PaUtil_SetLastHostErrorInfo( paALSA, aErr_, snd_strerror( aErr_ ) ); \
            } \
            */ \
            PaUtil_DebugPrint(( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
            result = (code); \
            goto error; \
        } \
    } while( 0 );

/* PaOSSHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    PaHostApiIndex hostApiIndex;
}
PaOSSHostApiRepresentation;

typedef struct PaOSS_DeviceList {
    PaDeviceInfo *deviceInfo;
    struct PaOSS_DeviceList *next;
}
PaOSS_DeviceList;

/* prototypes for functions declared in this file */

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
static PaError BuildDeviceList( PaOSSHostApiRepresentation *hostApi );


/** Initialize the OSS API implementation
 * This function will initialize host API datastructures and query host devices for information.
 *
 * Aspect DeviceCapabilities: Enumeration of host API devices is initiated from here
 * Aspect FreeResources: If an error is encountered under way we have to free each resource allocated in this function,
 * this happens with the usual "error" label.
 */
PaError PaOSS_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    PaOSSHostApiRepresentation *ossHostApi = NULL;

    PA_DEBUG(("PaOSS_Initialize\n"));

    PA_UNLESS( ossHostApi = (PaOSSHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaOSSHostApiRepresentation) ),
            paInsufficientMemory );
    PA_UNLESS( ossHostApi->allocations = PaUtil_CreateAllocationGroup(), paInsufficientMemory );
    ossHostApi->hostApiIndex = hostApiIndex;

    /* Initialize host API structure */
    *hostApi = &ossHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paOSS;
    (*hostApi)->info.name = "OSS";
    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PA_ENSURE( BuildDeviceList( ossHostApi ) );

    PaUtil_InitializeStreamInterface( &ossHostApi->callbackStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &ossHostApi->blockingStreamInterface, CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error:
    if( ossHostApi )
    {
        if( ossHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( ossHostApi->allocations );
            PaUtil_DestroyAllocationGroup( ossHostApi->allocations );
        }
                
        PaUtil_FreeMemory( ossHostApi );
    }
    return result;
}

#ifndef AFMT_S16_NE
#define AFMT_S16_NE  Get_AFMT_S16_NE()
/*********************************************************************
 * Some versions of OSS do not define AFMT_S16_NE. So check CPU.
 * PowerPC is Big Endian. X86 is Little Endian.
 */
static int Get_AFMT_S16_NE( void )
{
    long testData = 1; 
    char *ptr = (char *) &testData;
    int isLittle = ( *ptr == 1 ); /* Does address point to least significant byte? */
    return isLittle ? AFMT_S16_LE : AFMT_S16_BE;
}
#endif

static PaError SetFormat( const char *callingFunctionName, int deviceHandle,
                        const char *deviceName, int inputChannelCount, int outputChannelCount,
                        double *sampleRate )
{
    int format;
    int rate;
    int temp;

    /* Attempt to set format to 16-bit */
    
    format = AFMT_S16_NE;
    if (ioctl(deviceHandle, SNDCTL_DSP_SETFMT, &format) == -1) {
       PA_DEBUG(("%s: could not set format: %s\n", callingFunctionName, deviceName ));
       return paSampleFormatNotSupported;
    }
    if (format != AFMT_S16_NE) {
       PA_DEBUG(("%s: device does not support AFMT_S16_NE: %s\n", callingFunctionName, deviceName ));
       return paSampleFormatNotSupported;
    }

    /* try to set the number of channels */

    if (inputChannelCount > 0) {
       temp = inputChannelCount;
       
       if( ioctl(deviceHandle, SNDCTL_DSP_CHANNELS, &temp) < 0 ) {
          PA_DEBUG(("%s: Couldn't set device %s to %d channels\n", callingFunctionName, deviceName, inputChannelCount ));
          return paSampleFormatNotSupported;
       }
    }
    
    if (outputChannelCount > 0) {
       temp = outputChannelCount;
       
       if( ioctl(deviceHandle, SNDCTL_DSP_CHANNELS, &temp) < 0 ) {
          PA_DEBUG(("%s: Couldn't set device %s to %d channels\n", callingFunctionName, deviceName, outputChannelCount ));          
          return paSampleFormatNotSupported;
       }
    }

    /* try to set the sample rate */

    rate = (int)(*sampleRate);
    if( ioctl( deviceHandle, SNDCTL_DSP_SPEED, &rate ) == -1 )
    {
        PA_DEBUG(("%s: Device %s, couldn't set sample rate to %d\n",
              callingFunctionName, deviceName, (int)*sampleRate ));
        return paInvalidSampleRate;
    }

    /* reject if there's no sample rate within 1% of the one requested */
    if( (fabs( *sampleRate - rate ) / *sampleRate) > 0.01 )
    {
        PA_DEBUG(("%s: Device %s, wanted %d, closest sample rate was %d\n",
              callingFunctionName, deviceName, (int)*sampleRate, rate ));                 
        return paInvalidSampleRate;
    }

    *sampleRate = rate;

    return paNoError;
}

PaError PaUtil_InitializeDeviceInfo( PaDeviceInfo *deviceInfo, const char *name, PaHostApiIndex hostApiIndex, int maxInputChannels,
        int maxOutputChannels, PaTime defaultLowInputLatency, PaTime defaultLowOutputLatency, PaTime defaultHighInputLatency,
        PaTime defaultHighOutputLatency, double defaultSampleRate, PaUtilAllocationGroup *allocations  )
{
    PaError result = paNoError;
    
    deviceInfo->structVersion = 2;
    if( allocations )
    {
        size_t len = strlen( name ) + 1;
        PA_UNLESS( deviceInfo->name = PaUtil_GroupAllocateMemory( allocations, len ), paInsufficientMemory );
        strncpy( (char *)deviceInfo->name, name, len );
    }
    else
        deviceInfo->name = name;

    deviceInfo->hostApi = hostApiIndex;
    deviceInfo->maxInputChannels = maxInputChannels;
    deviceInfo->maxOutputChannels = maxOutputChannels;
    deviceInfo->defaultLowInputLatency = defaultLowInputLatency;
    deviceInfo->defaultLowOutputLatency = defaultLowOutputLatency;
    deviceInfo->defaultHighInputLatency = defaultHighInputLatency;
    deviceInfo->defaultHighOutputLatency = defaultHighOutputLatency;
    deviceInfo->defaultSampleRate = defaultSampleRate;

error:
    return result;
}

/** Query OSS device
 * This is where PaDeviceInfo objects are constructed and filled in with relevant information.
 *
 * Aspect DeviceCapabilities: The inferred device capabilities are recorded in a PaDeviceInfo object that is constructed
 * in place.
 */
static PaError QueryDevice( char *deviceName, PaOSSHostApiRepresentation *ossApi, PaDeviceInfo **deviceInfo )
{
    PaError result = paNoError;
    int tempDevHandle;
    int numChannels, maxNumChannels;
    int sampleRate;
    int format;
    PaTime defaultLowInputLatency, defaultLowOutputLatency, defaultHighInputLatency, defaultHighOutputLatency;

    /* douglas:
       we have to do this querying in a slightly different order. apparently
       some sound cards will give you different info based on their settins. 
       e.g. a card might give you stereo at 22kHz but only mono at 44kHz.
       the correct order for OSS is: format, channels, sample rate
    */

    if ( (tempDevHandle = open( deviceName, O_WRONLY|O_NONBLOCK ))  == -1 )
    {
        PA_DEBUG(("QueryDevice: could not open %s\n", deviceName ));
        return paDeviceUnavailable;
    }

    /* Attempt to set format to 16-bit */
    format = AFMT_S16_NE;
    if( ioctl( tempDevHandle, SNDCTL_DSP_SETFMT, &format ) == -1 ) {
       PA_DEBUG(("QueryDevice: could not set format: %s\n", deviceName ));
       result = paSampleFormatNotSupported;
       goto error;
    }
    if( format != AFMT_S16_NE ) {
       PA_DEBUG(("QueryDevice: device does not support AFMT_S16_NE: %s\n", deviceName ));
       result = paSampleFormatNotSupported;
       goto error;
    }

    /* Negotiate for the maximum number of channels for this device. PLB20010927
     * Consider up to 16 as the upper number of channels.
     * Variable maxNumChannels should contain the actual upper limit after the call.
     * Thanks to John Lazzaro and Heiko Purnhagen for suggestions.
     */
    maxNumChannels = 0;
    for( numChannels = 1; numChannels <= 16; numChannels++ )
    {
        int temp = numChannels;
        PA_DEBUG(("QueryDevice: use SNDCTL_DSP_CHANNELS, numChannels = %d\n", numChannels ))
        if( ioctl( tempDevHandle, SNDCTL_DSP_CHANNELS, &temp ) < 0 )
        {
            /* ioctl() failed so bail out if we already have stereo */
            if( numChannels > 2 ) break;
        }
        else
        {
            /* ioctl() worked but bail out if it does not support numChannels.
             * We don't want to leave gaps in the numChannels supported.
             */
            if( (numChannels > 2) && (temp != numChannels) ) break;
            PA_DEBUG(("QueryDevice: temp = %d\n", temp ))
            if( temp > maxNumChannels ) maxNumChannels = temp; /* Save maximum. */
        }
    }

    /* The above negotiation may fail for an old driver so try this older technique. */
    if( maxNumChannels < 1 )
    {
        int stereo = 1;
        if( ioctl( tempDevHandle, SNDCTL_DSP_STEREO, &stereo ) < 0 )
        {
            maxNumChannels = 1;
        }
        else
        {
            maxNumChannels = (stereo) ? 2 : 1;
        }
        PA_DEBUG(("QueryDevice: use SNDCTL_DSP_STEREO, maxNumChannels = %d\n", maxNumChannels ))
    }

    PA_DEBUG(("QueryDevice: maxNumChannels = %d\n", maxNumChannels))

    /* FIXME - for now, assume maxInputChannels = maxOutputChannels.
     *    Eventually do separate queries for O_WRONLY and O_RDONLY
    */

    /* During channel negotiation, the last ioctl() may have failed. This can
     * also cause sample rate negotiation to fail. Hence the following, to return
     * to a supported number of channels. SG20011005 */
    {
        int temp = maxNumChannels;
        if( temp > 2 ) temp = 2; /* use most reasonable default value */
        ioctl( tempDevHandle, SNDCTL_DSP_CHANNELS, &temp );
    }

    /* Get supported sample rate closest to 44100 Hz */
    sampleRate = 44100;
    if( ioctl( tempDevHandle, SNDCTL_DSP_SPEED, &sampleRate ) == -1 )
    {
        result = paUnanticipatedHostError;
        goto error;
    }

    /* TODO */
    defaultLowInputLatency = defaultLowOutputLatency = 512. / sampleRate;
    defaultHighInputLatency = defaultHighOutputLatency = 2048. / sampleRate;
    PA_UNLESS( *deviceInfo = PaUtil_GroupAllocateMemory( ossApi->allocations, sizeof (PaDeviceInfo) ), paInsufficientMemory );
    PA_ENSURE( PaUtil_InitializeDeviceInfo( *deviceInfo, deviceName, ossApi->hostApiIndex, maxNumChannels, maxNumChannels,
                defaultLowInputLatency, defaultLowOutputLatency, defaultHighInputLatency, defaultHighOutputLatency, sampleRate,
                ossApi->allocations ) );

error:
    /* We MUST close the handle here or we won't be able to reopen it later!!!  */
    close( tempDevHandle );

    return result;
}

/** Query host devices
 * Loop over host devices and query their capabilitiesu
 *
 * Aspect DeviceCapabilities: This function calls QueryDevice on each device entry and receives a filled in PaDeviceInfo object
 * per device, these are placed in the host api representation's deviceInfos array.
 */
static PaError BuildDeviceList( PaOSSHostApiRepresentation *ossApi )
{
    PaError result = paNoError;
    PaUtilHostApiRepresentation *commonApi = &ossApi->inheritedHostApiRep;
    int i;
    int numDevices = 0, maxDeviceInfos = 1;
    PaDeviceInfo **deviceInfos = NULL;

    /* These two will be set to the first working input and output device, respectively */
    commonApi->info.defaultInputDevice = paNoDevice;
    commonApi->info.defaultOutputDevice = paNoDevice;

    /* Find devices by calling QueryDevice on each
     * potential device names.  When we find a valid one,
     * add it to a linked list.
     * A: Can there only be 10 devices? */

    for( i = 0; i < 10; i++ )
    {
       char deviceName[32];
       PaDeviceInfo *deviceInfo;
       int testResult;

       if( i == 0 )
          snprintf(deviceName, sizeof (deviceName), "%s", DEVICE_NAME_BASE);
       else
          snprintf(deviceName, sizeof (deviceName), "%s%d", DEVICE_NAME_BASE, i);

       PA_DEBUG(("PaOSS BuildDeviceList: trying device %s\n", deviceName ));
       if( (testResult = QueryDevice( deviceName, ossApi, &deviceInfo )) != paNoError )
       {
           PA_UNLESS( testResult != paInsufficientMemory, paInsufficientMemory );
           PA_DEBUG(("PaOSS BuildDeviceList: QueryDevice returned %d\n", testResult ));
           continue;
       }

       ++numDevices;
       if( !deviceInfos || numDevices > maxDeviceInfos )
       {
           maxDeviceInfos *= 2;
           PA_UNLESS( deviceInfos = (PaDeviceInfo **) realloc( deviceInfos, maxDeviceInfos * sizeof (PaDeviceInfo *) ),
                   paInsufficientMemory );
       }

       deviceInfos[numDevices - 1] = deviceInfo;
       if( commonApi->info.defaultInputDevice == paNoDevice && deviceInfo->maxInputChannels > 0 )
           commonApi->info.defaultInputDevice = i;
       if( commonApi->info.defaultOutputDevice == paNoDevice && deviceInfo->maxOutputChannels > 0 )
           commonApi->info.defaultOutputDevice = i;
    }

    /* Make an array of PaDeviceInfo pointers out of the linked list */

    PA_DEBUG(("PaOSS BuildDeviceList: Total number of devices found: %d\n", numDevices));

    commonApi->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
        ossApi->allocations, sizeof(PaDeviceInfo*) * numDevices );
    memcpy( commonApi->deviceInfos, deviceInfos, numDevices * sizeof (PaDeviceInfo *) );
    free( deviceInfos );

    commonApi->info.deviceCount = numDevices;

error:
    return result;
}

static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaOSSHostApiRepresentation *ossHostApi = (PaOSSHostApiRepresentation*)hostApi;

    if( ossHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( ossHostApi->allocations );
        PaUtil_DestroyAllocationGroup( ossHostApi->allocations );
    }

    PaUtil_FreeMemory( ossHostApi );
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    PaDeviceIndex device;
    PaDeviceInfo *deviceInfo;
    PaError result = paNoError;
    char *deviceName;
    int inputChannelCount, outputChannelCount;
    int tempDevHandle = 0;
    int flags;
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

    if (inputChannelCount == 0 && outputChannelCount == 0)
        return paInvalidChannelCount;

    /* if full duplex, make sure that they're the same device */

    if (inputChannelCount > 0 && outputChannelCount > 0 &&
        inputParameters->device != outputParameters->device)
        return paInvalidDevice;

    /* if full duplex, also make sure that they're the same number of channels */

    if (inputChannelCount > 0 && outputChannelCount > 0 &&
        inputChannelCount != outputChannelCount)
       return paInvalidChannelCount;

    /* open the device so we can do more tests */
    
    if (inputChannelCount > 0) {
        result = PaUtil_DeviceIndexToHostApiDeviceIndex(&device, inputParameters->device, hostApi);
        if (result != paNoError)
            return result;
    }
    else {
        result = PaUtil_DeviceIndexToHostApiDeviceIndex(&device, outputParameters->device, hostApi);
        if (result != paNoError)
            return result;
    }

    deviceInfo = hostApi->deviceInfos[device];
    deviceName = (char *)deviceInfo->name;
    
    flags = O_NONBLOCK;
    if (inputChannelCount > 0 && outputChannelCount > 0)
       flags |= O_RDWR;
    else if (inputChannelCount > 0)
       flags |= O_RDONLY;
    else
       flags |= O_WRONLY;

    if ( (tempDevHandle = open(deviceInfo->name, flags))  == -1 )
    {
        PA_DEBUG(("PaOSS IsFormatSupported: could not open %s\n", deviceName ));
        return paDeviceUnavailable;
    }

    /* SetFormat will do the rest of the checking for us */

    if ((result = SetFormat("PaOSS IsFormatSupported", tempDevHandle,
                                  deviceName, inputChannelCount, outputChannelCount,
                                  &sampleRate)) != paNoError)
    {
       goto error;
    }

    /* everything succeeded! */

    close(tempDevHandle);             

    return paFormatIsSupported;

 error:
    if (tempDevHandle)
        close(tempDevHandle);         

    return paSampleFormatNotSupported;
}

/* PaOSSStream - a stream data structure specifically for this implementation */

typedef struct PaOSSStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    int deviceHandle;

    int stopSoon;
    int stopNow;
    int isActive;
    int isStopped;
    int isThreadValid;

    int inputChannelCount;
    int outputChannelCount;

    pthread_t thread;

    void *inputBuffer;
    void *outputBuffer;

    int lastPosPtr;
    double lastStreamBytes;

    int framesProcessed;

    double sampleRate;

    unsigned long framesPerHostCallback;
}
PaOSSStream;

typedef enum {
    StreamMode_In,
    StreamMode_Out
} StreamMode;

static PaError ValidateParameters( const PaStreamParameters *parameters, const PaDeviceInfo *deviceInfo, StreamMode mode )
{
    int maxChans;

    assert( parameters );

    if( parameters->device == paUseHostApiSpecificDeviceSpecification )
    {
        return paInvalidDevice;
    }

    maxChans = (mode == StreamMode_In ? deviceInfo->maxInputChannels :
        deviceInfo->maxOutputChannels);
    if( parameters->channelCount > maxChans )
    {
        return paInvalidChannelCount;
    }

    return paNoError;
}

/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

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
    PaOSSHostApiRepresentation *ossHostApi = (PaOSSHostApiRepresentation*)hostApi;
    PaOSSStream *stream = 0;
    const PaDeviceInfo *deviceInfo;
    audio_buf_info bufinfo;
    int bytesPerHostBuffer;
    int flags;
    int deviceHandle = 0;
    const char *deviceName;
    unsigned long framesPerHostBuffer;
    int inputChannelCount = 0, outputChannelCount = 0;
    PaSampleFormat inputSampleFormat = paInt16, outputSampleFormat = paInt16;
    PaSampleFormat hostInputSampleFormat = paInt16, hostOutputSampleFormat = paInt16;
    const PaDeviceInfo *inputDeviceInfo = 0, *outputDeviceInfo = 0;

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */

    if( inputParameters )
    {
        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */
        inputDeviceInfo = hostApi->deviceInfos[inputParameters->device];
        PA_ENSURE( ValidateParameters( inputParameters, inputDeviceInfo, StreamMode_In ) );

        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
        hostInputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( paInt16, inputSampleFormat );
    }

    if( outputParameters )
    {
        outputDeviceInfo = hostApi->deviceInfos[outputParameters->device];
        PA_ENSURE( ValidateParameters( outputParameters, outputDeviceInfo, StreamMode_Out ) );

        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
        hostOutputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( paInt16, outputSampleFormat );
    }

    PA_UNLESS( inputChannelCount > 0 || outputChannelCount > 0, paInvalidChannelCount );

    /* if full duplex, make sure that they're the same device */
    if( inputChannelCount > 0 && outputChannelCount > 0 )
    {
        if( inputParameters->device != outputParameters->device )
            return paBadIODeviceCombination;
    /* if full duplex, also make sure that they're the same number of channels */
        if( inputChannelCount != outputChannelCount )
           return paInvalidChannelCount;
    }

    /* note that inputParameters and outputParameters device indices are
     * already in host format */
    deviceInfo = inputChannelCount > 0 ? inputDeviceInfo : outputDeviceInfo;
    deviceName = deviceInfo->name;

    flags = O_NONBLOCK;
    if (inputChannelCount > 0 && outputChannelCount > 0)
       flags |= O_RDWR;
    else if (inputChannelCount > 0)
       flags |= O_RDONLY;
    else
       flags |= O_WRONLY;

    /* open first in nonblocking mode, in case it's busy... */
    ENSURE_( deviceHandle = open(deviceInfo->name, flags), paDeviceUnavailable );
    {
        /* Then make it blocking */
        int fflags = fcntl( deviceHandle, F_GETFL );
        ENSURE_( fcntl( deviceHandle, F_SETFL, fflags & ~O_NONBLOCK ), paUnanticipatedHostError );
    }

    PA_ENSURE( SetFormat( "PaOSS OpenStream", deviceHandle, deviceName, inputChannelCount, outputChannelCount,
                                  &sampleRate ) );

    /* Compute number of frames per host buffer - if we can't retrieve the
     * value, use the user's value instead 
     */
    /*
    if( framesPerBuffer == paFramesPerBufferUnspecified )
    {
         PA_MIN( inputParameters->suggestedLatency, outputParameters->suggestedLatency );
        framesPerHostBuffer =
    }
    */
    
    if( ioctl( deviceHandle, SNDCTL_DSP_GETBLKSIZE, &bytesPerHostBuffer ) == 0 )
    {
        framesPerHostBuffer = bytesPerHostBuffer / 2 / (inputChannelCount>0? inputChannelCount: outputChannelCount);
    }
    else
        framesPerHostBuffer = framesPerBuffer;

    /* Allocate stream and fill in structure */

    PA_UNLESS( stream = (PaOSSStream*)PaUtil_AllocateMemory( sizeof(PaOSSStream) ), paInsufficientMemory );

    if( streamCallback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &ossHostApi->callbackStreamInterface, streamCallback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &ossHostApi->blockingStreamInterface, streamCallback, userData );
    }    

    stream->streamRepresentation.streamInfo.inputLatency = 0.;
    stream->streamRepresentation.streamInfo.outputLatency = 0.;

    if (inputChannelCount > 0) {
        if( ioctl( deviceHandle, SNDCTL_DSP_GETISPACE, &bufinfo ) == 0 )
            stream->streamRepresentation.streamInfo.inputLatency =
                (bufinfo.fragsize * bufinfo.fragstotal) / sampleRate;
    }

    if (outputChannelCount > 0) {
        if( ioctl( deviceHandle, SNDCTL_DSP_GETOSPACE, &bufinfo ) == 0 )
            stream->streamRepresentation.streamInfo.outputLatency =
                (bufinfo.fragsize * bufinfo.fragstotal) / sampleRate;
    }    

    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    /* we assume a fixed host buffer size in this example, but the buffer processor
        can also support bounded and unknown host buffer sizes by passing 
        paUtilBoundedHostBufferSize or paUtilUnknownHostBufferSize instead of
        paUtilFixedHostBufferSize below. */
        
    PA_ENSURE(  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              inputChannelCount, inputSampleFormat, hostInputSampleFormat,
              outputChannelCount, outputSampleFormat, hostOutputSampleFormat,
              sampleRate, streamFlags, framesPerBuffer,
              framesPerHostBuffer, paUtilFixedHostBufferSize,
              streamCallback, userData ) );

    stream->framesPerHostCallback = framesPerHostBuffer;

    stream->stopSoon = 0;
    stream->stopNow = 0;
    stream->isActive = 0;
    stream->isStopped = 1;
    /*stream->thread = 0;*/
    stream->isThreadValid = 0;
    stream->lastPosPtr = 0;
    stream->lastStreamBytes = 0;
    stream->sampleRate = sampleRate;
    stream->framesProcessed = 0;
    stream->deviceHandle = deviceHandle;

    if (inputChannelCount > 0)
        stream->inputBuffer = PaUtil_AllocateMemory( 2 * framesPerHostBuffer * inputChannelCount );
    else
        stream->inputBuffer = NULL;

    if (outputChannelCount > 0)
        stream->outputBuffer = PaUtil_AllocateMemory( 2 * framesPerHostBuffer * outputChannelCount );
    else
        stream->outputBuffer = NULL;

    stream->inputChannelCount = inputChannelCount;
    stream->outputChannelCount = outputChannelCount;

    *s = (PaStream*)stream;

    result = paNoError;

    return result;

error:
    if( stream )
        PaUtil_FreeMemory( stream );

    if( deviceHandle )
        close( deviceHandle );

    return result;
}

static void *PaOSS_AudioThreadProc(void *userData)
{
    PaOSSStream *stream = (PaOSSStream*)userData;

    PA_DEBUG(("PaOSS AudioThread: %d in, %d out\n", stream->inputChannelCount, stream->outputChannelCount));

    while( (stream->stopNow == 0) && (stream->stopSoon == 0) ) {
        PaStreamCallbackTimeInfo timeInfo = {0,0,0}; /* TODO: IMPLEMENT ME */
        int callbackResult;
        unsigned long framesProcessed;
        int bytesRequested;
        int bytesRead, bytesWritten;
        int delta;
        int result;
        count_info info;

        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );
    
        PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo,
                0 /* @todo pass underflow/overflow flags when necessary */ );
        
        /*
          depending on whether the host buffers are interleaved, non-interleaved
          or a mixture, you will want to call PaUtil_SetInterleaved*Channels(),
          PaUtil_SetNonInterleaved*Channel() or PaUtil_Set*Channel() here.
        */

        if ( stream->inputChannelCount > 0 )
        {
            bytesRequested = stream->framesPerHostCallback * 2 * stream->inputChannelCount;
            bytesRead = read( stream->deviceHandle, stream->inputBuffer, bytesRequested );
            
            PaUtil_SetInputFrameCount( &stream->bufferProcessor, bytesRead/(2*stream->inputChannelCount));
            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor,
                                                0, /* first channel of inputBuffer is channel 0 */
                                                stream->inputBuffer,
                                                0 ); /* 0 - use inputChannelCount passed to init buffer processor */
        }

        if ( stream->outputChannelCount > 0 )
        {
           PaUtil_SetOutputFrameCount( &stream->bufferProcessor, 0 /* default to host buffer size */ );
           PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                                                0, /* first channel of outputBuffer is channel 0 */
                                                stream->outputBuffer,
                                                0 ); /* 0 - use outputChannelCount passed to init buffer processor */
        }

        callbackResult = paContinue;
        framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );

        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
        
        if( callbackResult == paContinue )
        {
           /* nothing special to do */
        }
        else if( callbackResult == paAbort )
        {
            /* once finished, call the finished callback */
            if( stream->streamRepresentation.streamFinishedCallback != 0 )
                stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );

            stream->isActive = 0;
            return NULL; /* return from the loop */
        }
        /*
         * all other conditions should behave like paComplete to maximize backwards compatibility.
         * (see notes for proposal 010)
         *
         */
        else /*if ( callbackResult == paComplete )*/
        {
            /* User callback has asked us to stop with paComplete or other non-zero value */
           
            /* once finished, call the finished callback */
            if( stream->streamRepresentation.streamFinishedCallback != 0 )
                stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );

            stream->stopSoon = 1;
        }

        if ( stream->outputChannelCount > 0 ) {
            /* write output samples AFTER we've checked the callback result code */

            bytesRequested = stream->framesPerHostCallback * 2 * stream->outputChannelCount;
            bytesWritten = write( stream->deviceHandle, stream->outputBuffer, bytesRequested );

            /* TODO: handle bytesWritten != bytesRequested (slippage?) */
        }

        /* Update current stream time (using a double so that
           we don't wrap around like info.bytes does) */
        if( stream->outputChannelCount > 0 )
            result = ioctl( stream->deviceHandle, SNDCTL_DSP_GETOPTR, &info );
        else
            result = ioctl( stream->deviceHandle, SNDCTL_DSP_GETIPTR, &info );

        if (result == 0) {
            delta = ( info.bytes - stream->lastPosPtr ) & 0x000FFFFF;
            stream->lastStreamBytes += delta;
            stream->lastPosPtr = info.bytes;
        }

        stream->framesProcessed += stream->framesPerHostCallback;
    }

    stream->isActive = 0;
    return NULL;
}

/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaOSSStream *stream = (PaOSSStream*)s;

    close(stream->deviceHandle);

    if ( stream->inputBuffer )
        PaUtil_FreeMemory( stream->inputBuffer );
    if ( stream->outputBuffer )
        PaUtil_FreeMemory( stream->outputBuffer );

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

    return result;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaOSSStream *stream = (PaOSSStream*)s;
    int presult;

    stream->isActive = 1;
    stream->isStopped = 0;
    stream->lastPosPtr = 0;
    stream->lastStreamBytes = 0;
    stream->framesProcessed = 0;

    PA_DEBUG(("PaOSS StartStream\n"));

    /* only use the thread for callback streams */
    if( stream->bufferProcessor.streamCallback ) {
            presult = pthread_create( &stream->thread,
                             NULL /*pthread_attr_t * attr*/,
                             PaOSS_AudioThreadProc, (void *)stream );
            stream->isThreadValid = 1;
    }
    
    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaOSSStream *stream = (PaOSSStream*)s;

    stream->stopSoon = 1;

    /* only use the thread for callback streams */
    if( stream->bufferProcessor.streamCallback && stream->isThreadValid )
        pthread_join( stream->thread, NULL );

    stream->isThreadValid = 0;
    stream->stopSoon = 0;
    stream->stopNow = 0;
    stream->isActive = 0;
    stream->isStopped = 1;

    PA_DEBUG(("PaOSS StopStream: Stopped stream\n"));

    return result;
}


static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaOSSStream *stream = (PaOSSStream*)s;

    stream->stopNow = 1;

    /* only use the thread for callback streams */
    if( stream->bufferProcessor.streamCallback && stream->isThreadValid )
        pthread_join( stream->thread, NULL );

    stream->isThreadValid = 0;
    stream->stopSoon = 0;
    stream->stopNow = 0;
    stream->isActive = 0;
    stream->isStopped = 1;

    PA_DEBUG(("PaOSS AbortStream: Stopped stream\n"));

    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaOSSStream *stream = (PaOSSStream*)s;

    return (stream->isStopped);
}


static PaError IsStreamActive( PaStream *s )
{
    PaOSSStream *stream = (PaOSSStream*)s;

    return (stream->isActive);
}


static PaTime GetStreamTime( PaStream *s )
{
    PaOSSStream *stream = (PaOSSStream*)s;
    count_info info;
    int delta;

    if( stream->outputChannelCount > 0 ) {
        if (ioctl( stream->deviceHandle, SNDCTL_DSP_GETOPTR, &info) == 0) {
            delta = ( info.bytes - stream->lastPosPtr ) & 0x000FFFFF;
            return ( stream->lastStreamBytes + delta) / ( stream->outputChannelCount * 2 ) / stream->sampleRate;
        }
    }
    else {
        if (ioctl( stream->deviceHandle, SNDCTL_DSP_GETIPTR, &info) == 0) {
            delta = (info.bytes - stream->lastPosPtr) & 0x000FFFFF;
            return ( stream->lastStreamBytes + delta) / ( stream->inputChannelCount * 2 ) / stream->sampleRate;
        }
    }

    /* the ioctl failed, but we can still give a coarse estimate */

    return stream->framesProcessed / stream->sampleRate;
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaOSSStream *stream = (PaOSSStream*)s;

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
    PaOSSStream *stream = (PaOSSStream*)s;
    int bytesRequested, bytesRead;
    unsigned long framesRequested;
    void *userBuffer;

    /* If user input is non-interleaved, PaUtil_CopyInput will manipulate the channel pointers,
     * so we copy the user provided pointers */
    if( stream->bufferProcessor.userInputIsInterleaved )
        userBuffer = buffer;
    else /* Copy channels into local array */
    {
        int numBytes = sizeof (void *) * stream->inputChannelCount;
        if( (userBuffer = alloca( numBytes )) == NULL )
            return paInsufficientMemory;
        memcpy( (void *)userBuffer, buffer, sizeof (void *) * stream->inputChannelCount );
    }

    while( frames )
    {
	if( frames > stream->framesPerHostCallback )
	    framesRequested = stream->framesPerHostCallback;
	else
	    framesRequested = frames;

	bytesRequested = framesRequested * 2 * stream->inputChannelCount;
	bytesRead = read( stream->deviceHandle, stream->inputBuffer, bytesRequested );
	if ( bytesRequested != bytesRead )
	    return paUnanticipatedHostError;

	PaUtil_SetInputFrameCount( &stream->bufferProcessor, stream->framesPerHostCallback );
	PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0, stream->inputBuffer, stream->inputChannelCount );
        PaUtil_CopyInput( &stream->bufferProcessor, &userBuffer, framesRequested );
	frames -= framesRequested;
    }
    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaOSSStream *stream = (PaOSSStream*)s;
    int bytesRequested, bytesWritten;
    unsigned long framesConverted;
    const void *userBuffer;

    /* If user output is non-interleaved, PaUtil_CopyOutput will manipulate the channel pointers,
     * so we copy the user provided pointers */
    if( stream->bufferProcessor.userOutputIsInterleaved )
        userBuffer = buffer;
    else /* Copy channels into local array */
    {
        int numBytes = sizeof (void *) * stream->outputChannelCount;
        if( (userBuffer = alloca( numBytes )) == NULL )
            return paInsufficientMemory;
        memcpy( (void *)userBuffer, buffer, sizeof (void *) * stream->outputChannelCount );
    }

    while( frames )
    {
	PaUtil_SetOutputFrameCount( &stream->bufferProcessor, stream->framesPerHostCallback );
	PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0, stream->outputBuffer, stream->outputChannelCount );

	framesConverted = PaUtil_CopyOutput( &stream->bufferProcessor, &userBuffer, frames );
	frames -= framesConverted;

	bytesRequested = framesConverted * 2 * stream->outputChannelCount;
	bytesWritten = write( stream->deviceHandle, stream->outputBuffer, bytesRequested );

	if ( bytesRequested != bytesWritten )
	    return paUnanticipatedHostError;
    }
    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaOSSStream *stream = (PaOSSStream*)s;
    audio_buf_info info;

    if ( ioctl(stream->deviceHandle, SNDCTL_DSP_GETISPACE, &info) == 0)
    {
        int bytesAvailable = info.fragments * info.fragsize;
        return ( bytesAvailable / 2 / stream->inputChannelCount );
    }
    else
        return 0; /* TODO: is this right for "don't know"? */
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaOSSStream *stream = (PaOSSStream*)s;

    audio_buf_info info;

    if ( ioctl(stream->deviceHandle, SNDCTL_DSP_GETOSPACE, &info) == 0)
    {
        int bytesAvailable = info.fragments * info.fragsize;
        return ( bytesAvailable / 2 / stream->outputChannelCount );
    }
    else
        return 0; /* TODO: is this right for "don't know"? */
}

