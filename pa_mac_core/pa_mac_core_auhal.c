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

#ifndef MIN
#define MIN(a, b)  (((a)<(b))?(a):(b))
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
/*#define MAC_CORE_DEBUG*/
#ifdef MAC_CORE_DEBUG
# define DBUG(MSG) do { printf("||PaMacCore (AUHAL)|| "); printf MSG ; fflush(stdout); } while(0)
#else
# define DBUG(MSG)
#endif

/* Very verbose debugging */
/*#define MAC_CORE_VERBOSE_DEBUG*/
#ifdef MAC_CORE_VERBOSE_DEBUG
# define VDBUG(MSG) do { printf("||PaMacCore (AUHAL)|| "); printf MSG ; fflush(stdout); } while(0)
#else
# define VDBUG(MSG)
#endif

/* FIXME: these are arbitrary and eventualy should be more dynamic/based
          on latency or something. */
#define RING_BUFFER_SIZE (20)
#define RING_BUFFER_LAG  (10)


/* Some utilities that sort of belong here, but were getting too unweildy */
#include "pa_mac_core_utilities.c"
/* Special purpose ring buffer just for pa_mac_core input processing. */
#include "pa_mac_core_input_ring_buffer.c"


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
    AudioUnit inputUnit;
    AudioUnit outputUnit;
    AudioDeviceID inputDevice;
    AudioDeviceID outputDevice;
    size_t userInChan;
    size_t userOutChan;
    size_t inputFramesPerBuffer;
    size_t outputFramesPerBuffer;
    InputRingBuffer inputRingBuffer;
    AudioTimeStamp startTime;
    bool isTimeSet;
    PaStreamCallbackFlags xrunFlags;
    enum {
       STOPPED          = 0,
       ACTIVE           = 1,
       CALLBACK_STOPPED = 2
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
                           sampleRate, 1024, 0, NULL, NULL );
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
    if( outStreamParams )
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Input,
                            OUTPUT_ELEMENT,
                            actualOutputFramesPerBuffer,
                            sizeof(unsigned long) ) );
    if( inStreamParams )
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Output,
                            INPUT_ELEMENT,
                            actualInputFramesPerBuffer,
                            sizeof(unsigned long) ) );

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
    /* FIXME: there will probably be problems in the callback if the framesPerBuffer is different on input and output, either because the device combos wouldn't allow it or because of S/R conversion. This is an unusual case (since it requires IO from seperate devices), but it should be fixed anyway. The callback should be modified to read off whatever is available from the input stream and let the buffer processor deal with it (if the buffer processsor is equiped for that).*/

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
    stream->inputUnit = NULL;
    stream->outputUnit = NULL;
    stream->inputFramesPerBuffer = 0;
    stream->outputFramesPerBuffer = 0;
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

    /* FIXME: size/lag are arbitrary. use some latency values or something.
              (size/lag only affects latency in full duplex/seperate devices)*/
    if( stream->inputUnit ) {
       result = InitializeRingBuffer( &stream->inputRingBuffer,
                                      RING_BUFFER_SIZE,
                                      RING_BUFFER_LAG,
                                      inputChannelCount,
                                      stream->inputFramesPerBuffer );
       if( result )
          goto error;
    }
    /*FIXME: if input and output buffersize differ we will need to do things
     *       a little differently in the callback. for now, force them to
     *       the same size so the callback doesn't freak out (I think there is
     *       another fixme that explains this better). */
    if( inputParameters && outputParameters )
       assert( stream->inputFramesPerBuffer == stream->outputFramesPerBuffer );

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

    /*
        IMPLEMENT ME: initialise the following fields with estimated or actual
        values.
        I think this is okay the way it is br 12/1/05
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
    /* FIXME: there is more to clean up here. ring buffers, units, etc.*/
    if( stream )
        PaUtil_FreeMemory( stream );

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
      /* Using the Ring buffer here just because it's there and it means
         not having to allocate an additional AudioBufferList and keep
         track of more variables. The cost of getting buffers is negligable.*/
      /* -- read input -- */
      AudioBufferList *abl = GetBufferForWrite( &stream->inputRingBuffer );
      assert( abl );
      err= AudioUnitRender(stream->inputUnit,
                       ioActionFlags,
                       inTimeStamp,
                       INPUT_ELEMENT,
                       inNumberFrames,
                       abl );
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
                             abl->mBuffers[0].mData,
                             abl->mBuffers[0].mNumberChannels);
      /* -- complete processing -- */
      callbackResult= stream->state == ACTIVE ? paContinue : paComplete ;
      framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
      switch( callbackResult )
      {
      case paContinue: break;
      case paComplete:
         AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
            stream->state = CALLBACK_STOPPED ;
         break;
      case paAbort:
         AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
            stream->state = CALLBACK_STOPPED ;
         break;
      }

      /* Because we used the ring buffer, we need to do a bit more work
         to keep it straight. */
      DoneWithWrite( &stream->inputRingBuffer );
      assert( GetBufferForRead( &stream->inputRingBuffer ) );
      DoneWithRead( &stream->inputRingBuffer );
   }
   else if( isRender )
   { /* -- handles duplex (seperate devices) and simplex output only -- */
      AudioBufferList *abl = NULL;
      unsigned long frames;
      int callbackResult;
      /* -- get previously stored input buffers (for duplex)-- */
      if( stream->inputUnit ) {
         abl = GetBufferForRead( &stream->inputRingBuffer );
         if( !abl )
            stream->xrunFlags |= paInputUnderflow;
         if( !abl )
            DBUG( ("Input underrun") );
      }

      /* -- start processing -- */
      PaUtil_BeginBufferProcessing( &(stream->bufferProcessor),
                                    &timeInfo,
                                    stream->xrunFlags );
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
      /* -- copy and process input data -- */
      if( stream->inputUnit ) {
         if( abl ) {
            /* FIXME: if input/out SR differ (eg. due to sr conversion, or buffer size issues)
                      we may get ahead or behind here. */
            PaUtil_SetInputFrameCount( &(stream->bufferProcessor), stream->inputFramesPerBuffer );
            PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                   0,
                                   abl->mBuffers[0].mData,
                                   abl->mBuffers[0].mNumberChannels);
         } else {
            /* FIXME: will this do the trick for an underrun? */
            PaUtil_SetInputFrameCount( &(stream->bufferProcessor), 0 );
         }
      }
      /* -- complete processing -- */
      callbackResult= stream->state == ACTIVE ? paContinue : paComplete ;
      framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );

      switch( callbackResult )
      {
      case paContinue: break;
      case paComplete:
         if( stream->inputUnit )
            AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
         AudioOutputUnitStop(stream->outputUnit); /*FIXME: this aborts*/
            stream->state = CALLBACK_STOPPED ;
         break;
      case paAbort:
         if( stream->inputUnit )
            AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
         AudioOutputUnitStop(stream->outputUnit); /*FIXME: this aborts*/
            stream->state = CALLBACK_STOPPED ;
         break;
      }

      /* complete the input read */
      if( stream->inputUnit && abl )
         DoneWithRead( &stream->inputRingBuffer );
   }
   else
   {
      AudioBufferList *abl = GetBufferForWrite( &stream->inputRingBuffer );
      assert( inNumberFrames == stream->inputFramesPerBuffer );
      if( !abl )
         DBUG( ("Input overrun") );
      if( stream->outputUnit )
      { /* -- handles input for seperate device, full duplex case -- */
         /*br 11/28/05: my understanding is that when the AudioIOProc is called
          *             it's just a signal that data is available. we need to
          *             call AudioUnitRender with our own buffer in order to
          *             get the input data. */
         if( abl )
         {
            OSErr err = 0;
            err= AudioUnitRender(stream->inputUnit,
                       ioActionFlags,
                       inTimeStamp,
                       INPUT_ELEMENT,
                       inNumberFrames,
                       abl );
            DoneWithWrite( &stream->inputRingBuffer );
            /* FEEDBACK: I'm not sure what to do when this call fails */
            assert( !err );
         }
         else 
         {
            stream->xrunFlags = paInputOverflow ;
         }
      }
      else
      { /* -- handles simplex input only case -- */
         OSErr err = 0;
         int callbackResult;
         assert( abl );
         err= AudioUnitRender(stream->inputUnit,
                    ioActionFlags,
                    inTimeStamp,
                    INPUT_ELEMENT,
                    inNumberFrames,
                    abl );
         DoneWithWrite( &stream->inputRingBuffer );
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
                                   abl->mBuffers[0].mData,
                                   abl->mBuffers[0].mNumberChannels);
         
         /* -- complete processing -- */
         callbackResult= stream->state == ACTIVE ? paContinue : paComplete ;
         framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
         abl = GetBufferForRead( &stream->inputRingBuffer );
         assert( abl );
         DoneWithRead( &stream->inputRingBuffer );

         switch( callbackResult )
         {
         case paContinue: break;
         case paComplete:
            if( stream->outputUnit )
               AudioOutputUnitStop(stream->outputUnit); /*FIXME: this aborts*/
            AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
            stream->state = CALLBACK_STOPPED ;
            break;
         case paAbort:
            if( stream->inputUnit )
            if( stream->outputUnit )
               AudioOutputUnitStop(stream->outputUnit); /*FIXME: this aborts*/
            AudioOutputUnitStop(stream->inputUnit); /*FIXME: this aborts*/
            stream->state = CALLBACK_STOPPED ;
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
    PaError result = paNoError;
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VDBUG( ( "Closing stream.\n" ) );

    /*
        IMPLEMENT ME:
            - additional stream closing + cleanup
        NOTE: this should be it!
    */
    if( stream->outputUnit && stream->outputUnit != stream->inputUnit ) {
       AudioUnitUninitialize( stream->outputUnit );
       CloseComponent( stream->outputUnit );
    }
    stream->outputUnit = NULL;
    if( stream->inputUnit )
    {
       AudioUnitUninitialize( stream->inputUnit );
       CloseComponent( stream->inputUnit );
       DisposeRingBuffer( &stream->inputRingBuffer );
       stream->inputUnit = NULL;
    }

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
    PaUtil_FreeMemory( stream );

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
    if( stream->inputUnit ) {
       ERR_WRAP( AudioOutputUnitStart(stream->inputUnit) );
    }
    if( stream->outputUnit && stream->outputUnit != stream->inputUnit ) {
       ERR_WRAP( AudioOutputUnitStart(stream->outputUnit) );
    }
    /*setStreamStartTime( stream );*/

    stream->state = ACTIVE;

    return paNoError;
#undef ERR_WRAP
}


static PaError StopStream( PaStream *s )
{
    /* FIXME */
    /* I found no docs for AudioOutputUnitStop that explain its
       exact behaviour, however, it tests it seems to abort the stream
       immediatelyi, which is NOT what we want. */
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    OSErr result = noErr;
    VDBUG( ( "Stopping stream.\n" ) );

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
    if( stream->inputUnit )
       ResetRingBuffer( &stream->inputRingBuffer, RING_BUFFER_LAG, TRUE );

    stream->isTimeSet = FALSE;
    stream->xrunFlags = 0;
    stream->state = STOPPED;

    return paNoError;
#undef ERR_WRAP
}

static PaError AbortStream( PaStream *s )
{
    OSErr result = noErr;
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VDBUG( ( "Aborting stream.\n" ) );

#define ERR_WRAP(mac_err) do { result = mac_err ; if ( result != noErr ) return ERR(result) ; } while(0)
    /* -- stop and reset: */
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
    if( stream->inputUnit )
       ResetRingBuffer( &stream->inputRingBuffer, RING_BUFFER_LAG, TRUE );

    stream->isTimeSet = FALSE;
    stream->xrunFlags = 0;
    stream->state = STOPPED;

    return paNoError;
#undef ERR_WRAP
}


static PaError IsStreamStopped( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    return stream->state == STOPPED ? 1 : 0;
}


static PaError IsStreamActive( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    return stream->state == ACTIVE ? 1 : 0;
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
