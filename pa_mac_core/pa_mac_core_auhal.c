/*
 * This is the AUHAL implementation of portaudio. Hopefully this will
 * one day replace pa_mac_core.
 *
 * Written by Bjorn Roche, from PA skeleton code, with lots of big chunks
 *  copied straight from code by Dominic Mazzoni (who wrote a HAL implementation).
 *
 * Dominic's code was based on code by Phil Burk, Darren Gibbs,
 * Gord Peters, Stephane Letz, and Greg Pfiel.
 *
 * Bjorn Roche grants all rights to this code to the maintainers of
 * PortAudio, so that they may redistribute and modify the code and
 * licenses as appropriate without asking him.
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

/*FIXME: Are these doxygen tags? Maybe they need some work*/
/** @file pa_mac_core
 @brief AUHAL implementation of PortAudio
*/

#include <string.h> /* strlen(), memcmc() etc. */

#include <AudioUnit/AudioUnit.h>


#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"
#include "../pablio/ringbuffer.h"

#ifndef MIN
#define MIN(a, b)  (((a)<(b))?(a):(b))
#endif

#ifndef MAX
#define MAX(a, b)  (((a)<(b))?(b):(a))
#endif

/* prototypes for functions declared in this file */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

PaError PaMacCore_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

#ifdef __cplusplus
}
#endif /* __cplusplus */

#define ERR(mac_error) PaMacCore_SetError(mac_error, __LINE__, 1 )
#define WARNING(mac_error) PaMacCore_SetError(mac_error, __LINE__, 0 )

/* Help keep track of AUHAL element numbers */
#define INPUT_ELEMENT  (1)
#define OUTPUT_ELEMENT (0)

/* Normal level of debugging */
/*
 */
#define MAC_CORE_DEBUG
#ifdef MAC_CORE_DEBUG
# define DBUG(MSG) do { printf("||PaMacCore (AUHAL)|| "); printf MSG ; fflush(stdout); } while(0)
#else
# define DBUG(MSG)
#endif

/* Very verbose debugging */
/*
 */
#define MAC_CORE_VERBOSE_DEBUG
#ifdef MAC_CORE_VERBOSE_DEBUG
# define VDBUG(MSG) do { printf("||PaMacCore (AUHAL)|| "); printf MSG ; fflush(stdout); } while(0)
#else
# define VDBUG(MSG)
#endif

/* Some utilities that sort of belong here, but were getting too unweildy */
#include "pa_mac_core_utilities.c"
/* Special purpose ring buffer just for pa_mac_core input processing. */
/* #include "pa_mac_core_input_ring_buffer.c" */
#include "../pablio/ringbuffer.c"


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
static OSStatus AudioIOProc( void *inRefCon,
                               AudioUnitRenderActionFlags *ioActionFlags,
                               const AudioTimeStamp *inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList *ioData );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, const void *buffer, unsigned long frames );
static signed long GetStreamReadAvailable( PaStream* stream );
static signed long GetStreamWriteAvailable( PaStream* stream );
/* PaMacAUHAL - host api datastructure specific to this implementation */
typedef struct
{
    PaUtilHostApiRepresentation inheritedHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    /* implementation specific data goes here */
    long devCount;
    AudioDeviceID *devIds; /*array of all audio devices*/
    AudioDeviceID defaultIn;
    AudioDeviceID defaultOut;
}
PaMacAUHAL;

/* stream data structure specifically for this implementation */
typedef struct PaMacCoreStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    /* implementation specific data goes here */
    bool bufferProcessorIsInitialized;
    AudioUnit inputUnit;
    AudioUnit outputUnit;
    AudioDeviceID inputDevice;
    AudioDeviceID outputDevice;
    size_t userInChan;
    size_t userOutChan;
    size_t inputFramesPerBuffer;
    size_t outputFramesPerBuffer;
    /*We use this ring buffer when input and out devs are different.*/
    RingBuffer inputRingBuffer;
    /*we need to preallocate an inputBuffer for reading data.*/
    AudioBufferList inputAudioBufferList;
    AudioTimeStamp startTime;
    volatile bool isTimeSet;
    volatile PaStreamCallbackFlags xrunFlags;
    volatile enum {
       STOPPED          = 0, /* playback is completely stopped,
                                and the user has called StopStream(). */
       CALLBACK_STOPPED = 1, /* callback has requested stop,
                                but user has not yet called StopStream(). */
       STOPPING         = 2, /* The stream is in the process of closing.
                                This state is just used internally;
                                externally it is indistinguishable from
                                ACTIVE.*/
       ACTIVE           = 3  /* The stream is active and running. */
    } state;
    double sampleRate;
}
PaMacCoreStream;

static PaError OpenAndSetupOneAudioUnit(
                                   const PaStreamParameters *inStreamParams,
                                   const PaStreamParameters *outStreamParams,
                                   const unsigned long requestedFramesPerBuffer,
                                   unsigned long *actualInputFramesPerBuffer,
                                   unsigned long *actualOutputFramesPerBuffer,
                                   const PaMacAUHAL *auhalHostApi,
                                   AudioUnit *audioUnit,
                                   AudioDeviceID *audioDevice,
                                   const double sampleRate,
                                   void *refCon );

/* for setting errors. */
#define PA_AUHAL_SET_LAST_HOST_ERROR( errorCode, errorText ) \
    PaUtil_SetLastHostErrorInfo( paInDevelopment, errorCode, errorText )




/*currently, this is only used in initialization, but it might be modified
  to be used when the list of devices changes.*/
static PaError gatherDeviceInfo(PaMacAUHAL *auhalHostApi)
{
    UInt32 size;
    UInt32 propsize;
    /* -- free any previous allocations -- */
    if( auhalHostApi->devIds )
        PaUtil_GroupFreeMemory(auhalHostApi->allocations, auhalHostApi->devIds);
    auhalHostApi->devIds = NULL;

    /* -- figure out how many devices there are -- */
    AudioHardwareGetPropertyInfo( kAudioHardwarePropertyDevices,
                                  &propsize,
                                  NULL );
    auhalHostApi->devCount = propsize / sizeof( AudioDeviceID );

    DBUG( ( "Found %ld device(s).\n", auhalHostApi->devCount ) );

    /* -- copy the device IDs -- */
    auhalHostApi->devIds = (AudioDeviceID *)PaUtil_GroupAllocateMemory(
                             auhalHostApi->allocations,
                             propsize );
    if( !auhalHostApi->devIds )
        return paInsufficientMemory;
    AudioHardwareGetProperty( kAudioHardwarePropertyDevices,
                                  &propsize,
                                  auhalHostApi->devIds );
#ifdef MAC_CORE_VERBOSE_DEBUG
    {
       int i;
       for( i=0; i<auhalHostApi->devCount; ++i )
          printf( "Device %d\t: %ld\n", i, auhalHostApi->devIds[i] );
    }
#endif

    size = sizeof(AudioDeviceID);
    auhalHostApi->defaultIn  = kAudioDeviceUnknown;
    auhalHostApi->defaultOut = kAudioDeviceUnknown;
    /* FEEDBACK: these calls could fail, in which case default in and out will
                 be unknown devices or could be undefined. Do I need to be more
                 rigorous here? */
    AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice,
                     &size,
                     &auhalHostApi->defaultIn);
    AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice,
                     &size,
                     &auhalHostApi->defaultOut);
    VDBUG( ( "Default in : %ld\n", auhalHostApi->defaultIn  ) );
    VDBUG( ( "Default out: %ld\n", auhalHostApi->defaultOut ) );

    return paNoError;
}

static PaError GetChannelInfo( PaMacAUHAL *auhalHostApi,
                               PaDeviceInfo *deviceInfo,
                               AudioDeviceID macCoreDeviceId,
                               int isInput)
{
    UInt32 propSize;
    PaError err = paNoError;
    UInt32 i;
    int numChannels = 0;
    AudioBufferList *buflist;
    UInt32 frameLatency;

    /* Get the number of channels from the stream configuration.
       Fail if we can't get this. */

    err = ERR(AudioDeviceGetPropertyInfo(macCoreDeviceId, 0, isInput, kAudioDevicePropertyStreamConfiguration, &propSize, NULL));
    if (err)
        return err;

    buflist = PaUtil_AllocateMemory(propSize);
    err = ERR(AudioDeviceGetProperty(macCoreDeviceId, 0, isInput, kAudioDevicePropertyStreamConfiguration, &propSize, buflist));
    if (err)
        return err;

    for (i = 0; i < buflist->mNumberBuffers; ++i)
        numChannels += buflist->mBuffers[i].mNumberChannels;

    if (isInput)
        deviceInfo->maxInputChannels = numChannels;
    else
        deviceInfo->maxOutputChannels = numChannels;

    /* Get the latency.  Don't fail if we can't get this. */
    /* default to something reasonable */
    deviceInfo->defaultLowInputLatency = .01;
    deviceInfo->defaultHighInputLatency = .01;
    propSize = sizeof(UInt32);
    err = WARNING(AudioDeviceGetProperty(macCoreDeviceId, 0, isInput, kAudioDevicePropertyLatency, &propSize, &frameLatency));
    if (!err) {
        double secondLatency = frameLatency / deviceInfo->defaultSampleRate;
        if (isInput) {
            deviceInfo->defaultLowInputLatency = secondLatency;
            deviceInfo->defaultHighInputLatency = secondLatency;
        }
        else {
            deviceInfo->defaultLowOutputLatency = secondLatency;
            deviceInfo->defaultHighOutputLatency = secondLatency;
        }
    }
    return paNoError;
}

static PaError InitializeDeviceInfo( PaMacAUHAL *auhalHostApi,
                                     PaDeviceInfo *deviceInfo,
                                     AudioDeviceID macCoreDeviceId,
                                     PaHostApiIndex hostApiIndex )
{
    Float64 sampleRate;
    char *name;
    PaError err = paNoError;
    UInt32 propSize;

    memset(deviceInfo, 0, sizeof(deviceInfo));

    deviceInfo->structVersion = 2;
    deviceInfo->hostApi = hostApiIndex;

    /* Get the device name.  Fail if we can't get it. */
    err = ERR(AudioDeviceGetPropertyInfo(macCoreDeviceId, 0, 0, kAudioDevicePropertyDeviceName, &propSize, NULL));
    if (err)
        return err;

    name = PaUtil_GroupAllocateMemory(auhalHostApi->allocations,propSize);
    if ( !name )
        return paInsufficientMemory;
    err = ERR(AudioDeviceGetProperty(macCoreDeviceId, 0, 0, kAudioDevicePropertyDeviceName, &propSize, name));
    if (err)
        return err;
    deviceInfo->name = name;

    /* Try to get the default sample rate.  Don't fail if we can't get this. */
    propSize = sizeof(Float64);
    err = ERR(AudioDeviceGetProperty(macCoreDeviceId, 0, 0, kAudioDevicePropertyNominalSampleRate, &propSize, &sampleRate));
    if (err)
        deviceInfo->defaultSampleRate = 0.0;
    else
        deviceInfo->defaultSampleRate = sampleRate;

    /* Get the maximum number of input and output channels.  Fail if we can't get this. */

    err = GetChannelInfo(auhalHostApi, deviceInfo, macCoreDeviceId, 1);
    if (err)
        return err;

    err = GetChannelInfo(auhalHostApi, deviceInfo, macCoreDeviceId, 0);
    if (err)
        return err;

    return paNoError;
}

PaError PaMacCore_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i;
    PaMacAUHAL *auhalHostApi;
    PaDeviceInfo *deviceInfoArray;

    auhalHostApi = (PaMacAUHAL*)PaUtil_AllocateMemory( sizeof(PaMacAUHAL) );
    if( !auhalHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    auhalHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !auhalHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    auhalHostApi->devIds = NULL;
    auhalHostApi->devCount = 0;

    /* get the info we need about the devices */
    result = gatherDeviceInfo( auhalHostApi );
    if( result != paNoError )
       goto error;

    *hostApi = &auhalHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paCoreAudio;
    /* -- FIXME: when this works, change to "Core Audio" -- */
    (*hostApi)->info.name = "Core Audio (AUHAL)";

    (*hostApi)->info.defaultInputDevice = paNoDevice;
    (*hostApi)->info.defaultOutputDevice = paNoDevice;

    (*hostApi)->info.deviceCount = 0;  

    if( auhalHostApi->devCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
                auhalHostApi->allocations, sizeof(PaDeviceInfo*) * auhalHostApi->devCount);
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
                auhalHostApi->allocations, sizeof(PaDeviceInfo) * auhalHostApi->devCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i=0; i < auhalHostApi->devCount; ++i )
        {
            int err;
            err = InitializeDeviceInfo( auhalHostApi, &deviceInfoArray[i],
                                      auhalHostApi->devIds[i],
                                      hostApiIndex );
            if (err == paNoError)
            { /* copy some info and set the defaults */
                (*hostApi)->deviceInfos[(*hostApi)->info.deviceCount] = &deviceInfoArray[i];
                if (auhalHostApi->devIds[i] == auhalHostApi->defaultIn)
                    (*hostApi)->info.defaultInputDevice = (*hostApi)->info.deviceCount;
                if (auhalHostApi->devIds[i] == auhalHostApi->defaultOut)
                    (*hostApi)->info.defaultOutputDevice = (*hostApi)->info.deviceCount;
                (*hostApi)->info.deviceCount++;
            }
            else
            { /* there was an error. we need to shift the devices down, so we ignore this one */
                int j;
                auhalHostApi->devCount--;
                for( j=i; j<auhalHostApi->devCount; ++j )
                   auhalHostApi->devIds[j] = auhalHostApi->devIds[j+1];
                i--;
            }
        }
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &auhalHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped,
                                      IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &auhalHostApi->blockingStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped,
                                      IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream,
                                      GetStreamReadAvailable,
                                      GetStreamWriteAvailable );

    return result;

error:
    if( auhalHostApi )
    {
        if( auhalHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( auhalHostApi->allocations );
            PaUtil_DestroyAllocationGroup( auhalHostApi->allocations );
        }
                
        PaUtil_FreeMemory( auhalHostApi );
    }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaMacAUHAL *auhalHostApi = (PaMacAUHAL*)hostApi;

    /*
        IMPLEMENT ME:
            - clean up any resources not handled by the allocation group
        TODO: Double check that everything is handled by alloc group
    */

    if( auhalHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( auhalHostApi->allocations );
        PaUtil_DestroyAllocationGroup( auhalHostApi->allocations );
    }

    PaUtil_FreeMemory( auhalHostApi );
}


static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
 
    /** These first checks are standard PA checks. We do some fancier checks
        later. */
    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if( inputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;
            
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

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if( outputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;
            
        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support outputChannelCount */
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
 
    /* FEEDBACK */
    /*        I think the only way to check a given format SR combo is     */
    /*        to try opening it. This could be disruptive, is that Okay?   */
    /*        The alternative is to just read off available sample rates,  */
    /*        but this will not work %100 of the time (eg, a device that   */
    /*        supports N output at one rate but only N/2 at a higher rate.)*/

    /* The following code opens the device with the requested parameters to
       see if it works. */
    {
       PaError err;
       PaStream *s;
       err = OpenStream( hostApi, &s, inputParameters, outputParameters,
                           sampleRate, 1024, 0, (PaStreamCallback *)1, NULL );
       if( err != paNoError && err != paInvalidSampleRate )
          DBUG( ( "OpenStream @ %g returned: %d: %s\n",
                  (float) sampleRate, err, Pa_GetErrorText( err ) ) );
       if( err ) 
          return err;
       err = CloseStream( s );
       if( err ) {
          /* FEEDBACK: is this more serious? should we assert? */
          DBUG( ( "WARNING: could not close Stream. %d: %s\n",
                  err, Pa_GetErrorText( err ) ) );
       }
    }

    return paFormatIsSupported;
}

static PaError OpenAndSetupOneAudioUnit(
                                   const PaStreamParameters *inStreamParams,
                                   const PaStreamParameters *outStreamParams,
                                   const unsigned long requestedFramesPerBuffer,
                                   unsigned long *actualInputFramesPerBuffer,
                                   unsigned long *actualOutputFramesPerBuffer,
                                   const PaMacAUHAL *auhalHostApi,
                                   AudioUnit *audioUnit,
                                   AudioDeviceID *audioDevice,
                                   const double sampleRate,
                                   void *refCon )
{
    ComponentDescription desc;
    Component comp;
    /*An Apple TN suggests using CAStreamBasicDescription, but that is C++*/
    AudioStreamBasicDescription desiredFormat;
    OSErr result = noErr;
    PaError paResult = paNoError;
    int line;
    UInt32 callbackKey;
    AURenderCallbackStruct rcbs;

    /* -- handle the degenerate case  -- */
    if( !inStreamParams && !outStreamParams ) {
       *audioUnit = NULL;
       *audioDevice = kAudioDeviceUnknown;
       return paNoError;
    }

    /*
     * The HAL AU is a Mac OS style "component".
     * the first few steps deal with that.
     * Later steps work on a combination of Mac OS
     * components and the slightly lower level
     * HAL.
     */

    /* -- describe the output type AudioUnit -- */
    /*  Note: for the default AudioUnit, we could use the
     *  componentSubType value kAudioUnitSubType_DefaultOutput;
     *  but I don't think that's relevant here.
     */
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags        = 0;
    desc.componentFlagsMask    = 0;
    /* -- find the component -- */
    comp = FindNextComponent( NULL, &desc );
    if( !comp )
    {
       DBUG( ( "AUHAL component not found." ) );
       *audioUnit = NULL;
       *audioDevice = kAudioDeviceUnknown;
       return paUnanticipatedHostError;
    }
    /* -- open it -- */
    result = OpenAComponent( comp, audioUnit );
    if( result )
    {
       DBUG( ( "Failed to open AUHAL component." ) );
       *audioUnit = NULL;
       *audioDevice = kAudioDeviceUnknown;
       return ERR( result );
    }
    /* -- prepare a little error handling logic / hackery -- */
#define ERR_WRAP(mac_err) do { result = mac_err ; line = __LINE__ ; if ( result != noErr ) goto error ; } while(0)
    /* -- if there is input, we have to explicitly enable input -- */
    if( inStreamParams )
    {
       UInt32 enableIO;
       enableIO = 1;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                 kAudioOutputUnitProperty_EnableIO,
                 kAudioUnitScope_Input,
                 INPUT_ELEMENT,
                 &enableIO,
                 sizeof(enableIO) ) );
    }
    /* -- if there is no output, we must explicitly disable output -- */
    if( !outStreamParams )
    {
       UInt32 enableIO;
       enableIO = 0;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                 kAudioOutputUnitProperty_EnableIO,
                 kAudioUnitScope_Output,
                 OUTPUT_ELEMENT,
                 &enableIO,
                 sizeof(enableIO) ) );
    }
    /* -- set the devices -- */
    /* make sure input and output are the same device if we are doing input and
       output. */
    if( inStreamParams && outStreamParams )
       assert( outStreamParams->device == inStreamParams->device );
    if( inStreamParams )
    {
       *audioDevice = auhalHostApi->devIds[inStreamParams->device] ;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                    kAudioOutputUnitProperty_CurrentDevice,
                    kAudioUnitScope_Global,
                    INPUT_ELEMENT,
                    audioDevice,
                    sizeof(AudioDeviceID) ) );
    }
    if( outStreamParams )
    {
       *audioDevice = auhalHostApi->devIds[outStreamParams->device] ;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                    kAudioOutputUnitProperty_CurrentDevice,
                    kAudioUnitScope_Global,
                    OUTPUT_ELEMENT,
                    audioDevice,
                    sizeof(AudioDeviceID) ) );
    }

    /* -- set format -- */
    bzero( &desiredFormat, sizeof(desiredFormat) );
    desiredFormat.mSampleRate       = sampleRate;
    desiredFormat.mFormatID         = kAudioFormatLinearPCM ;
    desiredFormat.mFormatFlags      = kAudioFormatFlagsNativeFloatPacked;
    desiredFormat.mFramesPerPacket  = 1;
    desiredFormat.mBitsPerChannel   = sizeof( float ) * 8;

    result = 0;
    /*  set device format first */
    if( inStreamParams ) {
       paResult = setBestSampleRateForDevice(*audioDevice,FALSE,sampleRate);
       if( paResult ) goto error;
       paResult = setBestFramesPerBuffer( *audioDevice, FALSE,
                                          requestedFramesPerBuffer,
                                          actualInputFramesPerBuffer );
       if( paResult ) goto error;
       if( actualInputFramesPerBuffer && actualOutputFramesPerBuffer )
          *actualOutputFramesPerBuffer = *actualInputFramesPerBuffer ;
    }
    if( outStreamParams && !inStreamParams ) {
       paResult = setBestSampleRateForDevice(*audioDevice,TRUE,sampleRate);
       if( paResult ) goto error;
       paResult = setBestFramesPerBuffer( *audioDevice, TRUE,
                                          requestedFramesPerBuffer,
                                          actualOutputFramesPerBuffer );
       if( paResult ) goto error;
    }
    /* now set the format on the Audio Units. */
    /* In the case of output, the hardware sample rate may not match the
     * sample rate we want, but the AudioUnit will convert. */
    if( inStreamParams )
    {
       desiredFormat.mBytesPerPacket=sizeof(float)*inStreamParams->channelCount;
       desiredFormat.mBytesPerFrame =sizeof(float)*inStreamParams->channelCount;
       desiredFormat.mChannelsPerFrame = inStreamParams->channelCount;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_StreamFormat,
                            kAudioUnitScope_Output,
                            INPUT_ELEMENT,
                            &desiredFormat,
                            sizeof(AudioStreamBasicDescription) ) );
    }
    if( outStreamParams )
    {
       desiredFormat.mBytesPerPacket=sizeof(float)*outStreamParams->channelCount;
       desiredFormat.mBytesPerFrame =sizeof(float)*outStreamParams->channelCount;
       desiredFormat.mChannelsPerFrame = outStreamParams->channelCount;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_StreamFormat,
                            kAudioUnitScope_Input,
                            OUTPUT_ELEMENT,
                            &desiredFormat,
                            sizeof(AudioStreamBasicDescription) ) );
    }
    /* set the maximumFramesPerSlice */
    /* not doing this causes real problems
       (eg. the callback might not be called). The idea of setting both this
       and the frames per buffer on the device is that we'll be most likely
       to actually get the frame size we requested in the callback. */
    if( outStreamParams ) {
       UInt32 size = sizeof( *actualOutputFramesPerBuffer );
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Input,
                            OUTPUT_ELEMENT,
                            actualOutputFramesPerBuffer,
                            sizeof(unsigned long) ) );
       ERR_WRAP( AudioUnitGetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Global,
                            OUTPUT_ELEMENT,
                            actualOutputFramesPerBuffer,
                            &size ) );
    }
    if( inStreamParams ) {
       /*UInt32 size = sizeof( *actualInputFramesPerBuffer );*/
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Output,
                            INPUT_ELEMENT,
                            actualInputFramesPerBuffer,
                            sizeof(unsigned long) ) );
/* Don't know why this causes problems
       ERR_WRAP( AudioUnitGetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Global, //Output,
                            INPUT_ELEMENT,
                            actualInputFramesPerBuffer,
                            &size ) );
*/
    }

    /* -- set IOProc (callback) -- */
    callbackKey = outStreamParams ? kAudioUnitProperty_SetRenderCallback
                                  : kAudioOutputUnitProperty_SetInputCallback ;
    rcbs.inputProc = AudioIOProc;
    rcbs.inputProcRefCon = refCon;
    ERR_WRAP( AudioUnitSetProperty(
                                 *audioUnit,
                                 callbackKey,
                                 kAudioUnitScope_Output,
                                 outStreamParams ? OUTPUT_ELEMENT : INPUT_ELEMENT,
                                 &rcbs,
                                 sizeof(rcbs)) );

    /*IMPLEMENTME: may need to worry about channel mapping.*/
    /*FEEDBACK: The current implementation offers SR conversion on
                output only. This is the natural way that the API works
                and so it stands to reason that that is the most natural
                thing for an application to do. It also stands to reason that
                users will not want their input sample rate changed, even if
                the might tolorate their output sample rate being changed.
                However, it is possible to use buffering and an AudioConverter
                to actually convert the audio, if this is desired. For now,
                Sample rate conversion happens on the output only. */

    /* initialize the audio unit */
    ERR_WRAP( AudioUnitInitialize(*audioUnit) );

    if( inStreamParams && outStreamParams )
       VDBUG( ("Opened device %ld for input and output.\n", *audioDevice ) );
    else if( inStreamParams )
       VDBUG( ("Opened device %ld for input.\n", *audioDevice ) );
    else if( outStreamParams )
       VDBUG( ("Opened device %ld for output.\n", *audioDevice ) );
    return paNoError;
#undef ERR_WRAP

    error:
       CloseComponent( *audioUnit );
       *audioUnit = NULL;
       if( result )
          return PaMacCore_SetError( result, line, 1 );
       return paResult;
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
    PaMacAUHAL *auhalHostApi = (PaMacAUHAL*)hostApi;
    PaMacCoreStream *stream = 0;
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
    VDBUG( ("Opening Stream.\n") );

    /*These first few bits of code are from paSkeleton with few modifications.*/
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

        /* Host supports interleaved float32 */
        hostInputSampleFormat = paFloat32;
    }
    else
    {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paInt16; /* Surpress 'uninitialised var' warnings. */
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

        /* Host supports interleaved float32 */
        hostOutputSampleFormat = paFloat32;
    }
    else
    {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paFloat32; /* Surpress 'uninitialized var' warnings. */
    }

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */

    stream = (PaMacCoreStream*)PaUtil_AllocateMemory( sizeof(PaMacCoreStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* If we fail after this point, we my be left in a bad state, with
       some data structures setup and others not. So, first thing we
       do is initialize everything so that if we fail, we know what hasn't
       been touched.
     */

    stream->inputAudioBufferList.mBuffers[0].mData = NULL;
    stream->inputRingBuffer.buffer = NULL;
    stream->inputUnit = NULL;
    stream->outputUnit = NULL;
    stream->inputFramesPerBuffer = 0;
    stream->outputFramesPerBuffer = 0;
    stream->bufferProcessorIsInitialized = FALSE;

    assert( streamCallback ) ; /*we only do callback right now */
    if( streamCallback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &auhalHostApi->callbackStreamInterface, streamCallback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                               &auhalHostApi->blockingStreamInterface, streamCallback, userData );
    }
    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );


    /* -- Now we actually open and setup streams. -- */
    if( inputParameters && outputParameters && outputParameters->device == inputParameters->device )
    { /* full duplex. One device. */
       result = OpenAndSetupOneAudioUnit( inputParameters,
                                          outputParameters,
                                          framesPerBuffer,
                                          &(stream->inputFramesPerBuffer),
                                          &(stream->outputFramesPerBuffer),
                                          auhalHostApi,
                                          &(stream->inputUnit),
                                          &(stream->inputDevice),
                                          sampleRate,
                                          stream );
       stream->outputUnit = stream->inputUnit;
       stream->outputDevice = stream->inputDevice;
       if( result != paNoError )
           goto error;
    }
    else
    { /* full duplex, different devices OR simplex */
       result = OpenAndSetupOneAudioUnit( NULL,
                                          outputParameters,
                                          framesPerBuffer,
                                          NULL,
                                          &(stream->outputFramesPerBuffer),
                                          auhalHostApi,
                                          &(stream->outputUnit),
                                          &(stream->outputDevice),
                                          sampleRate,
                                          stream );
       if( result != paNoError )
           goto error;
       result = OpenAndSetupOneAudioUnit( inputParameters,
                                          NULL,
                                          framesPerBuffer,
                                          &(stream->inputFramesPerBuffer),
                                          NULL,
                                          auhalHostApi,
                                          &(stream->inputUnit),
                                          &(stream->inputDevice),
                                          sampleRate,
                                          stream );
       if( result != paNoError )
           goto error;
    }

    if( stream->inputUnit ) {
       const size_t szfl = sizeof(float);
       /* setup the AudioBufferList used for input */
       bzero( &stream->inputAudioBufferList, sizeof( AudioBufferList ) );
       stream->inputAudioBufferList.mNumberBuffers = 1;
       stream->inputAudioBufferList.mBuffers[0].mNumberChannels
                 = inputChannelCount;
       stream->inputAudioBufferList.mBuffers[0].mDataByteSize
                 = stream->inputFramesPerBuffer*inputChannelCount*szfl;
       stream->inputAudioBufferList.mBuffers[0].mData
                 = (float *) calloc(
                               stream->inputFramesPerBuffer*inputChannelCount,
                               szfl );
       if( !stream->inputAudioBufferList.mBuffers[0].mData )
       {
          result = paInsufficientMemory;
          goto error;
       }
        
       /*
        * If input and output devs are different, we also need a
        * ring buffer to store inpt data while waiting for output
        * data.
        */
       if( stream->outputUnit && stream->inputUnit != stream->outputUnit )
       {
          /* Calculate an appropriate ring buffer size. It must be at least
             3x framesPerBuffer and 2x suggested latency and it must be a
             power of 2. FEEDBACK: too liberal/conservative/another way?*/
          double latency;
          int index, i;
          void *data;
          long ringSize;
          latency = MAX( inputParameters->suggestedLatency,
                         outputParameters->suggestedLatency );
          ringSize = latency * sampleRate * 2 * inputChannelCount;
          VDBUG( ( "suggested latency: %d\n", (int) (latency*sampleRate) ) );
          if( ringSize < stream->inputFramesPerBuffer * 3 )
             ringSize = stream->inputFramesPerBuffer * 3 * inputChannelCount;
          if( ringSize < stream->outputFramesPerBuffer * 3 )
             ringSize = stream->outputFramesPerBuffer * 3 * inputChannelCount;
          VDBUG(("inFramesPerBuffer:%d\n",(int)stream->inputFramesPerBuffer));
          VDBUG(("outFramesPerBuffer:%d\n",(int)stream->outputFramesPerBuffer));
          VDBUG(("Ringbuffer size (1): %d\n", (int)ringSize ));

          /* round up to the next power of 2 */
          index = -1;
          for( i=0; i<sizeof(long)*8; ++i )
             if( ringSize >> i & 0x01 )
                index = i;
          assert( index > 0 );
          if( ringSize <= ( 0x01 << index ) )
             ringSize = 0x01 << index ;
          else
             ringSize = 0x01 << ( index + 1 );
          VDBUG(( "Final Ringbuffer size (2): %d\n", (int)ringSize ));

          /*now, we need to allocate memory for the ring buffer*/
          data = calloc( ringSize, szfl );
          if( !data )
          {
             result = paInsufficientMemory;
             goto error;
          }

          /* now we can initialize the ring buffer */
          assert( 0 ==
            RingBuffer_Init( &stream->inputRingBuffer,
                             ringSize*szfl, data ) );
          /* advance the read point a little, so we are reading from the
             middle of the buffer */
          RingBuffer_AdvanceWriteIndex( &stream->inputRingBuffer, ringSize*szfl / 4 );
       }
    }

    /* -- initialize Buffer Processor -- */
    {
    unsigned long maxHostFrames = stream->inputFramesPerBuffer;
    if( stream->outputFramesPerBuffer > maxHostFrames )
       maxHostFrames = stream->outputFramesPerBuffer;
    result = PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
              inputChannelCount, inputSampleFormat,
              hostInputSampleFormat,
              outputChannelCount, outputSampleFormat,
              hostOutputSampleFormat,
              sampleRate,
              streamFlags,
              framesPerBuffer,
              /* If sample rate conversion takes place, the buffer size
                 will not be known, although it is probably limited by
                 the maxFramesPerSlice property. */
              maxHostFrames,
              paUtilUnknownHostBufferSize,
              streamCallback, userData );
    if( result != paNoError )
        goto error;
    }
    stream->bufferProcessorIsInitialized = TRUE;

    /*
        IMPLEMENT ME: initialise the following fields with estimated or actual
        values.
        I think this is okay the way it is br 12/1/05
        maybe need to change input latency estimate if IO devs differ
    */
    stream->streamRepresentation.streamInfo.inputLatency =
            PaUtil_GetBufferProcessorInputLatency(&stream->bufferProcessor);
    stream->streamRepresentation.streamInfo.outputLatency =
            PaUtil_GetBufferProcessorOutputLatency(&stream->bufferProcessor);
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    stream->sampleRate  = sampleRate;
    stream->userInChan  = inputChannelCount;
    stream->userOutChan = outputChannelCount;

    stream->isTimeSet   = FALSE;
    stream->state = STOPPED;
    stream->xrunFlags = 0;

    *s = (PaStream*)stream;

    return result;

error:
    CloseStream( stream );
    return result;
}

PaTime GetStreamTime( PaStream *s )
{
   /* FIXME: I am not at all sure this timing info stuff is right.
             patest_sine_time reports negative latencies, which is wierd.*/
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    AudioTimeStamp timeStamp;

    if ( !stream->isTimeSet )
        return (PaTime)0;

    if ( stream->outputDevice )
        AudioDeviceGetCurrentTime( stream->outputDevice, &timeStamp);
    else if ( stream->inputDevice )
        AudioDeviceGetCurrentTime( stream->inputDevice, &timeStamp);
    else
        return (PaTime)0;

    return (PaTime)(timeStamp.mSampleTime - stream->startTime.mSampleTime)/stream->sampleRate;
}

static void setStreamStartTime( PaMacCoreStream *stream )
{
   /* FIXME: I am not at all sure this timing info stuff is right.
             patest_sine_time reports negative latencies, which is wierd.*/
   if( stream->inputDevice )
      AudioDeviceGetCurrentTime( stream->inputDevice, &stream->startTime);
   else
      AudioDeviceGetCurrentTime( stream->outputDevice, &stream->startTime);
}


static PaTime TimeStampToSecs(PaMacCoreStream *stream, const AudioTimeStamp* timeStamp)
{
    if (timeStamp->mFlags & kAudioTimeStampSampleTimeValid)
        return (timeStamp->mSampleTime / stream->sampleRate);
    else
        return 0;
}

/*
 * Called by the AudioUnit API to process audio from the sound card.
 * This is where the magic happens.
 */
/* FEEDBACK: there is a lot of redundant code here because of how all the cases differ. This makes it hard to maintain, so if there are suggestinos for cleaning it up, I'm all ears. */
static OSStatus AudioIOProc( void *inRefCon,
                               AudioUnitRenderActionFlags *ioActionFlags,
                               const AudioTimeStamp *inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList *ioData )
{
   unsigned long framesProcessed = 0;
   PaStreamCallbackTimeInfo timeInfo = {0,0,0};
   const bool isRender = inBusNumber == OUTPUT_ELEMENT;
   PaMacCoreStream *stream = (PaMacCoreStream*)inRefCon;

   PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

   if( !stream->isTimeSet )
      setStreamStartTime( stream );
   stream->isTimeSet = TRUE;


   /* -----------------------------------------------------------------*\
      This output may be useful for debugging,
      But printing durring the callback is a bad enough idea that
      this is not enabled by enableing the usual debugging calls.
   \* -----------------------------------------------------------------*/
   /*
   static int renderCount = 0;
   static int inputCount = 0;
   printf( "-------------------  starting reder/input\n" );
   if( isRender )
      printf("Render callback (%d):\t", ++renderCount);
   else
      printf("Input callback  (%d):\t", ++inputCount);
   printf( "Call totals: %d (input), %d (render)\n", inputCount, renderCount );

   printf( "--- inBusNumber: %lu\n", inBusNumber );
   printf( "--- inNumberFrames: %lu\n", inNumberFrames );
   printf( "--- %x ioData\n", (unsigned) ioData );
   if( ioData )
   {
      int i=0;
      printf( "--- ioData.mNumBuffers %lu: \n", ioData->mNumberBuffers );
      for( i=0; i<ioData->mNumberBuffers; ++i )
         printf( "--- ioData buffer %d size: %lu.\n", i, ioData->mBuffers[i].mDataByteSize );
   }
      ----------------------------------------------------------------- */

   if( isRender ) {
      AudioTimeStamp currentTime;
      timeInfo.outputBufferDacTime = TimeStampToSecs(stream, inTimeStamp);
      AudioDeviceGetCurrentTime(stream->outputDevice, &currentTime);
      timeInfo.currentTime = TimeStampToSecs(stream, &currentTime);
   }
   if( isRender && stream->inputUnit == stream->outputUnit )
      timeInfo.inputBufferAdcTime = TimeStampToSecs(stream, inTimeStamp);
   if( !isRender ) {
      AudioTimeStamp currentTime;
      timeInfo.inputBufferAdcTime = TimeStampToSecs(stream, inTimeStamp);
      AudioDeviceGetCurrentTime(stream->inputDevice, &currentTime);
      timeInfo.currentTime = TimeStampToSecs(stream, &currentTime);
   }


   if( isRender && stream->inputUnit == stream->outputUnit )
   { /* -- handles duplex, one device -- */
      OSErr err = 0;
      unsigned long frames;
      int callbackResult;
      /* -- read input -- */
      err= AudioUnitRender(stream->inputUnit,
                       ioActionFlags,
                       inTimeStamp,
                       INPUT_ELEMENT,
                       inNumberFrames,
                       &stream->inputAudioBufferList );
      /* FEEDBACK: I'm not sure what to do when this call fails */
      assert( !err );
      /* -- start processing -- */
      PaUtil_BeginBufferProcessing( &(stream->bufferProcessor),
                                    &timeInfo,
                                    stream->xrunFlags );
      stream->xrunFlags = 0;

      /* -- Copy and process output data -- */
      assert( ioData->mNumberBuffers == 1 );
      frames = ioData->mBuffers[0].mDataByteSize;
      frames /= sizeof( float ) * ioData->mBuffers[0].mNumberChannels;
      assert( frames == stream->outputFramesPerBuffer );
      assert( ioData->mBuffers[0].mNumberChannels == stream->userOutChan );
      PaUtil_SetOutputFrameCount( &(stream->bufferProcessor), frames );
      PaUtil_SetInterleavedOutputChannels( &(stream->bufferProcessor),
                                        0,
                                        ioData->mBuffers[0].mData,
                                        ioData->mBuffers[0].mNumberChannels);
      /* -- copy and process input data -- */
      PaUtil_SetInputFrameCount( &(stream->bufferProcessor), frames );
      PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                             0,
                             stream->inputAudioBufferList.mBuffers[0].mData,
                             stream->inputAudioBufferList.mBuffers[0].mNumberChannels);
      /* -- complete processing -- */
      callbackResult = paContinue;
      framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
      switch( callbackResult )
      {
      case paContinue: break;
      case paComplete:
         stream->state = CALLBACK_STOPPED ;
         AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
         break;
      case paAbort:
         stream->state = CALLBACK_STOPPED ;
         AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
         break;
      }
   }
   else if( isRender )
   { /* -- handles duplex (seperate devices) and simplex output only -- */
      unsigned long frames;
      int callbackResult;

      /* Sometimes, when stopping a duplex stream we get erroneous
         xrun flags, so if this is our last run, clear the flags. */
      int xrunFlags = stream->xrunFlags;
      if( stream->state == STOPPING || stream->state == CALLBACK_STOPPED )
         xrunFlags = 0;
      /* -- start processing -- */
      PaUtil_BeginBufferProcessing( &(stream->bufferProcessor),
                                    &timeInfo,
                                    xrunFlags );
      stream->xrunFlags = 0; /* FIXME: we only send flags to Buf Proc once. Is that OKAY? */

      /* -- Copy and process output data -- */
      assert( ioData->mNumberBuffers == 1 );
      frames = ioData->mBuffers[0].mDataByteSize;
      frames /= sizeof( float ) * ioData->mBuffers[0].mNumberChannels;
      assert( ioData->mBuffers[0].mNumberChannels == stream->userOutChan );
      PaUtil_SetOutputFrameCount( &(stream->bufferProcessor), frames );
      PaUtil_SetInterleavedOutputChannels( &(stream->bufferProcessor),
                                        0,
                                        ioData->mBuffers[0].mData,
                                        ioData->mBuffers[0].mNumberChannels);
      callbackResult= paContinue ;
      /* -- copy and process input data, and complete processing -- */
      if( stream->inputUnit ) {
         const int flsz = sizeof( float );
         /* we need to read data out of the ring buffer and into the
            buffer processor */
         void *data1, *data2;
         long size1, size2;
         int inChan = stream->inputAudioBufferList.mBuffers[0].mNumberChannels;
         RingBuffer_GetReadRegions( &stream->inputRingBuffer,
                                    inChan*frames*flsz,
                                    &data1, &size1,
                                    &data2, &size2 );
         if( size1 / ( flsz * inChan ) == frames ) {
            /* simplest case: all in first buffer */
            PaUtil_SetInputFrameCount( &(stream->bufferProcessor), frames );
            PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                0,
                                data1,
                                inChan );
            framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
            RingBuffer_AdvanceReadIndex(&stream->inputRingBuffer, size1 );
         } else if( ( size1 + size2 ) / ( flsz * inChan ) < frames ) {
            /*we underflowed. take what data we can, zero the rest.*/
            float data[frames*inChan];
            if( size1 )
               memcpy( data, data1, size1 );
            if( size2 )
               memcpy( data+size1, data2, size2 );
            bzero( data+size1+size2, frames*flsz*inChan - size1 - size2 );

            PaUtil_SetInputFrameCount( &(stream->bufferProcessor), frames );
            PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                0,
                                data,
                                inChan );
            framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
            RingBuffer_AdvanceReadIndex( &stream->inputRingBuffer,
                                         size1+size2 );
            /* flag underflow */
            stream->xrunFlags |= paInputUnderflow;
         } else {
            /*we got all the data, but split between buffers*/
            PaUtil_SetInputFrameCount( &(stream->bufferProcessor),
                                       size1 / ( flsz * inChan ) );
            PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                0,
                                data1,
                                inChan );
            PaUtil_Set2ndInputFrameCount( &(stream->bufferProcessor),
                                          size2 / ( flsz * inChan ) );
            PaUtil_Set2ndInterleavedInputChannels( &(stream->bufferProcessor),
                                0,
                                data2,
                                inChan );
            framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
            RingBuffer_AdvanceReadIndex(&stream->inputRingBuffer, size1+size2 );
         }
      } else {
         framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
      }

      switch( callbackResult )
      {
      case paContinue: break;
      case paComplete:
         stream->state = CALLBACK_STOPPED ;
         if( stream->inputUnit )
            AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
         AudioOutputUnitStop(stream->outputUnit); /*FIXME: this aborts*/
         break;
      case paAbort:
         stream->state = CALLBACK_STOPPED ;
         if( stream->inputUnit )
            AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
         AudioOutputUnitStop(stream->outputUnit); /*FIXME: this aborts*/
         break;
      }
   }
   else
   {
      if( stream->outputUnit )
      { /* -- handles input for seperate device, full duplex case -- */
         /*br 11/28/05: my understanding is that when the AudioIOProc is called
          *             it's just a signal that data is available. we need to
          *             call AudioUnitRender with our own buffer in order to
          *             get the input data. */
         OSErr err = 0;
         long bytesIn, bytesOut;
         err= AudioUnitRender(stream->inputUnit,
                    ioActionFlags,
                    inTimeStamp,
                    INPUT_ELEMENT,
                    inNumberFrames,
                    &stream->inputAudioBufferList );
         /* FEEDBACK: I'm not sure what to do when this call fails */
         assert( !err );
         bytesIn = sizeof( float ) * inNumberFrames * stream->inputAudioBufferList.mBuffers[0].mNumberChannels ;
         bytesOut = RingBuffer_Write( &stream->inputRingBuffer,
                                stream->inputAudioBufferList.mBuffers[0].mData,
                                bytesIn );
         if( bytesIn != bytesOut ) {
            stream->xrunFlags |= paInputOverflow ;
         }
      }
      else
      { /* -- handles simplex input only case -- */
         OSErr err = 0;
         int callbackResult;
         err= AudioUnitRender(stream->inputUnit,
                    ioActionFlags,
                    inTimeStamp,
                    INPUT_ELEMENT,
                    inNumberFrames,
                    &stream->inputAudioBufferList );
         /* FEEDBACK: I'm not sure what to do when this call fails */
         assert( !err );
         /* -- start processing -- */
         PaUtil_BeginBufferProcessing( &(stream->bufferProcessor),
                                       &timeInfo,
                                       stream->xrunFlags );
         stream->xrunFlags = 0;
         /* -- transfer the data -- */
         PaUtil_SetInputFrameCount( &(stream->bufferProcessor), inNumberFrames );
         PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                   0,
                                   stream->inputAudioBufferList.mBuffers[0].mData,
                                   stream->inputAudioBufferList.mBuffers[0].mNumberChannels);
         
         /* -- complete processing -- */
         callbackResult= paContinue ;
         framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
         switch( callbackResult )
         {
         case paContinue: break;
         case paComplete:
            stream->state = CALLBACK_STOPPED ;
            if( stream->outputUnit )
               AudioOutputUnitStop(stream->outputUnit); /*FIXME: this aborts*/
            AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
            stream->state = CALLBACK_STOPPED ;
            break;
         case paAbort:
            stream->state = CALLBACK_STOPPED ;
            if( stream->outputUnit )
               AudioOutputUnitStop(stream->outputUnit); /*FIXME: this aborts*/
            AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
            break;
         }
      }
   }

   PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
   return noErr;
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    /* This may be called from a failed OpenStream.
       Therefore, each piece of info is treated seperately. */
    PaError result = paNoError;
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VDBUG( ( "Closing stream.\n" ) );

    if( stream ) {
       if( stream->outputUnit && stream->outputUnit != stream->inputUnit ) {
          AudioUnitUninitialize( stream->outputUnit );
          CloseComponent( stream->outputUnit );
       }
       stream->outputUnit = NULL;
       if( stream->inputUnit )
       {
          AudioUnitUninitialize( stream->inputUnit );
          CloseComponent( stream->inputUnit );
          stream->inputUnit = NULL;
       }
       if( stream->inputRingBuffer.buffer )
          free( stream->inputRingBuffer.buffer );
       stream->inputRingBuffer.buffer = NULL;
       if( stream->inputAudioBufferList.mBuffers[0].mData )
          free( stream->inputAudioBufferList.mBuffers[0].mData );
       stream->inputAudioBufferList.mBuffers[0].mData = NULL;

       if( stream->bufferProcessorIsInitialized )
          PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
       PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
       PaUtil_FreeMemory( stream );
    }

    return result;
}


static PaError StartStream( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    OSErr result = noErr;
    VDBUG( ( "Starting stream.\n" ) );

    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

#define ERR_WRAP(mac_err) do { result = mac_err ; if ( result != noErr ) return ERR(result) ; } while(0)
    /* -- start -- */
    stream->state = ACTIVE;
    if( stream->inputUnit ) {
       ERR_WRAP( AudioOutputUnitStart(stream->inputUnit) );
    }
    if( stream->outputUnit && stream->outputUnit != stream->inputUnit ) {
       ERR_WRAP( AudioOutputUnitStart(stream->outputUnit) );
    }

    return paNoError;
#undef ERR_WRAP
}


static PaError StopStream( PaStream *s )
{
    /* FIXME */
    /* I found no docs for AudioOutputUnitStop that explain its
       exact behaviour, however, it tests it seems to abort the stream
       immediately, which is NOT what we want. */
    /*FIXME: sometimes buffer over/underruns are reported as the stream is stopping. See if this can be avoided. */
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    OSErr result = noErr;
    VDBUG( ( "Stopping stream.\n" ) );

    stream->state = STOPPING;

#define ERR_WRAP(mac_err) do { result = mac_err ; if ( result != noErr ) return ERR(result) ; } while(0)
    /* -- stop and reset -- */
    if( stream->inputUnit == stream->outputUnit && stream->inputUnit )
    {
       ERR_WRAP( AudioOutputUnitStop(stream->inputUnit) );
       ERR_WRAP( AudioUnitReset(stream->inputUnit, kAudioUnitScope_Global, 1) );
       ERR_WRAP( AudioUnitReset(stream->inputUnit, kAudioUnitScope_Global, 0) );
    }
    else
    {
       if( stream->inputUnit )
       {
          ERR_WRAP(AudioOutputUnitStop(stream->inputUnit) );
          ERR_WRAP(AudioUnitReset(stream->inputUnit,kAudioUnitScope_Global,1));
       }
       if( stream->outputUnit )
       {
          ERR_WRAP(AudioOutputUnitStop(stream->outputUnit));
          ERR_WRAP(AudioUnitReset(stream->outputUnit,kAudioUnitScope_Global,0));
       }
    }
    if( stream->inputRingBuffer.buffer ) {
       RingBuffer_Flush( &stream->inputRingBuffer );
       /* advance the write point a little, so we are reading from the
          middle of the buffer. We'll need extra at the end because
          testing has shown that this helps. */
       bzero(stream->inputRingBuffer.buffer,stream->inputRingBuffer.bufferSize);
       RingBuffer_AdvanceWriteIndex( &stream->inputRingBuffer,
                                    stream->inputRingBuffer.bufferSize / 4 );
    }

    stream->isTimeSet = FALSE;
    stream->xrunFlags = 0;
    stream->state = STOPPED;

    VDBUG( ( "Stream Stopped.\n" ) );
    return paNoError;
#undef ERR_WRAP
}

static PaError AbortStream( PaStream *s )
{
    VDBUG( ( "Aborting stream.\n" ) );
    return StopStream(s);
}


static PaError IsStreamStopped( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    return stream->state == STOPPED ? 1 : 0;
}


static PaError IsStreamActive( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    return ( stream->state == ACTIVE || stream->state == STOPPING );
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}


/*
    As separate stream interfaces are used for blocking and callback
    streams, the following functions can be guaranteed to only be called
    for blocking streams. IMPLEMENTME: no blocking interface yet!
*/

static PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;
    
    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;
    
    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    /* suppress unused variable warnings */
    (void) stream;
    
    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    /* suppress unused variable warnings */
    (void) stream;
    
    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}
