/*
 * $Id$
 * Portable Audio I/O Library Multi-Host API front end
 * Validate function parameters and manage multiple host APIs.
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
#include <stdarg.h>
#include <memory.h>

#include "portaudio.h"
#include "pa_util.h"
#include "pa_hostapi.h"
#include "pa_stream.h"

#include "pa_trace.h"


//#define PA_LOG_API_CALLS
/*
    The basic format for log messages is as follows:
 
    - entry (void function):
 
    "FunctionName called.\n"
 
    - entry (non void function):
 
    "FunctionName called:\n"
    "\tParam1Type param1: param1Value\n"
    "\tParam2Type param2: param2Value\n"      (etc...)
 
 
    - exit (no return value)
 
    "FunctionName returned.\n"
 
    - exit (simple return value)
 
    "FunctionName returned:\n"
    "\tReturnType: returnValue\n\n"
 
    if the return type is an error code, the error text is displayed in ()
 
    if the return type is not an error code, but has taken a special value
    because an error occurred, then the reason for the error is shown in []
 
    if the return type is a struct ptr, the struct is dumped.
 
    see the code for more detailed examples
*/


static long hostError_ = 0;

void PaUtil_SetHostError( long error )
{
    hostError_ = error;
}


void PaUtil_DebugPrint( const char *format, ... )
{
    va_list ap;

    va_start( ap, format );
    vfprintf( stderr, format, ap );
    va_end( ap );

    fflush( stderr );
}


static PaUtilHostApiRepresentation **hostApis_ = 0;
static int hostApisCount_ = 0;
static int initializationCount_ = 0;
static int deviceCount_ = 0;

PaUtilStreamRepresentation *firstOpenStream_ = NULL;


#define PA_IS_INITIALISED_ (initializationCount_ != 0)


static int CountHostApiInitializers( void )
{
    int result = 0;

    while( paHostApiInitializers[ result ] != 0 )
        ++result;
    return result;
}


static void TerminateHostApis( void )
{
    /* terminate in reverse order from initialization */

    while( hostApisCount_ > 0 )
    {
        --hostApisCount_;
        hostApis_[hostApisCount_]->Terminate( hostApis_[hostApisCount_] );
    }
    hostApisCount_ = 0;
    deviceCount_ = 0;

    if( hostApis_ != 0 )
        PaUtil_FreeMemory( hostApis_ );
    hostApis_ = 0;
}


static PaError InitializeHostApis( void )
{
    PaError result = paNoError;
    int i, initializerCount, baseDeviceIndex;

    initializerCount = CountHostApiInitializers();

    hostApis_ = PaUtil_AllocateMemory( sizeof(PaUtilHostApiRepresentation*) * initializerCount );
    if( !hostApis_ )
    {
        result = paInsufficientMemory;
        goto error; 
    }

    hostApisCount_ = 0;
    deviceCount_ = 0;
    baseDeviceIndex = 0;

    for( i=0; i< initializerCount; ++i )
    {
        hostApis_[hostApisCount_] = NULL;
        result = paHostApiInitializers[i]( &hostApis_[hostApisCount_], hostApisCount_ );
        if( result != paNoError )
            goto error;

        if( hostApis_[hostApisCount_] )
        {

            hostApis_[hostApisCount_]->privatePaFrontInfo.baseDeviceIndex = baseDeviceIndex;
            baseDeviceIndex += hostApis_[hostApisCount_]->deviceCount;
            deviceCount_ += hostApis_[hostApisCount_]->deviceCount;

            ++hostApisCount_;
        }
    }

    return result;

error:
    TerminateHostApis();
    return result;
}


/*
    FindHostApi() finds the index of the host api to which
    <device> belongs and returns it. if <hostSpecificDeviceIndex> is
    non-null, the host specific device index is returned in it.
    returns -1 if <device> is out of range.
 
*/
static int FindHostApi( PaDeviceIndex device, int *hostSpecificDeviceIndex )
{
    int i=0;

    if( !PA_IS_INITIALISED_ )
        return -1;

    if( device < 0 )
        return -1;

    while( i < hostApisCount_
            && device >= hostApis_[i]->deviceCount )
    {

        device -= hostApis_[i]->deviceCount;
        ++i;
    }

    if( i >= hostApisCount_ )
        return -1;

    if( hostSpecificDeviceIndex )
        *hostSpecificDeviceIndex = device;

    return i;
}


static void AddOpenStream( PaStream* stream )
{
    ((PaUtilStreamRepresentation*)stream)->nextOpenStream = firstOpenStream_;
    firstOpenStream_ = (PaUtilStreamRepresentation*)stream;
}


static void RemoveOpenStream( PaStream* stream )
{
    PaUtilStreamRepresentation *previous = NULL;
    PaUtilStreamRepresentation *current = firstOpenStream_;

    while( current != NULL )
    {
        if( ((PaStream*)current) == stream )
        {
            if( previous == NULL )
            {
                firstOpenStream_ = current->nextOpenStream;
            }
            else
            {
                previous->nextOpenStream = current->nextOpenStream;
            }
            return;
        }
        else
        {
            previous = current;
            current = current->nextOpenStream;
        }
    }
}


static void CloseOpenStreams( void )
{
    /* we call Pa_CloseStream() here to ensure that the same destruction
        logic is used for automatically closed streams */

    while( firstOpenStream_ != NULL )
        Pa_CloseStream( firstOpenStream_ );
}


PaError Pa_Initialize( void )
{
    PaError result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint( "Pa_Initialize called.\n" );
#endif

    if( PA_IS_INITIALISED_ )
    {
        ++initializationCount_;
        result = paNoError;
    }
    else
    {
        PaUtil_InitializeClock();
        PaUtil_ResetTraceMessages();

        result = InitializeHostApis();
        if( result == paNoError )
            ++initializationCount_;
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint( "Pa_Initialize returned:\n" );
    PaUtil_DebugPrint( "\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_Terminate( void )
{
    PaError result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_Terminate called.\n" );
#endif

    if( PA_IS_INITIALISED_ )
    {
        if( --initializationCount_ == 0 )
        {
            CloseOpenStreams();

            TerminateHostApis();

            PaUtil_DumpTraceMessages();
        }
        result = paNoError;
    }
    else
    {
        result=  paNotInitialized;
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_Terminate returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


long Pa_GetHostError( void )
{
    return hostError_;
}


const char *Pa_GetErrorText( PaError errnum )
{
    const char *msg;

    switch(errnum)
    {
    case paNoError:                  msg = "Success"; break;
    case paNotInitialized:           msg = "PortAudio not initialized"; break;
    case paHostError:                msg = "Host error"; break;
    case paInvalidChannelCount:      msg = "Invalid number of channels"; break;
    case paInvalidSampleRate:        msg = "Invalid sample rate"; break;
    case paInvalidDevice:            msg = "Invalid device"; break;
    case paInvalidFlag:              msg = "Invalid flag"; break;
    case paSampleFormatNotSupported: msg = "Sample format not supported"; break;
    case paBadIODeviceCombination:   msg = "Illegal combination of I/O devices"; break;
    case paInsufficientMemory:       msg = "Insufficient memory"; break;
    case paBufferTooBig:             msg = "Buffer too big"; break;
    case paBufferTooSmall:           msg = "Buffer too small"; break;
    case paNullCallback:             msg = "No callback routine specified"; break;
    case paBadStreamPtr:             msg = "Invalid stream pointer"; break;
    case paTimedOut:                 msg = "Wait timed out"; break;
    case paInternalError:            msg = "Internal PortAudio error"; break;
    case paDeviceUnavailable:        msg = "Device unavailable"; break;
    case paIncompatibleStreamInfo:   msg = "Incompatible host API specific stream info"; break;
    case paStreamIsStopped:          msg = "Stream is stopped"; break;
    case paStreamIsNotStopped:        msg = "Stream is not stopped"; break;
    default:                         msg = "Illegal error number"; break;
    }
    return msg;
}


PaHostApiIndex Pa_HostApiTypeIdToHostApiIndex( PaHostApiTypeId type )
{
    PaHostApiIndex result;
    int i;
    
#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_HostApiTypeIdToHostApiIndex called:\n" );
    PaUtil_DebugPrint("\PaHostApiTypeId type: %d\n", type );
#endif

    if( !PA_IS_INITIALISED_ )
    {

        result = -1;
        
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiTypeIdToHostApiIndex returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiIndex: -1 [ PortAudio not initialized ]\n\n" );
#endif

    }
    else
    {
        result = -1;
        
        for( i=0; i < hostApisCount_; ++i )
        {
            if( hostApis_[i]->info.type == type )
            {
                result = i;
                break;
            }         
        }

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiTypeIdToHostApiIndex returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiIndex: %d\n\n", result );
#endif
    }

    return result;
}


PaError PaUtil_GetHostApiRepresentation( struct PaUtilHostApiRepresentation **hostApi,
        PaHostApiTypeId type )
{
    PaError result;
    int i;
    
    if( !PA_IS_INITIALISED_ )
    {
        result = paNotInitialized;
    }
    else
    {
        result = paInternalError; // FIXME: should return host API not found
                
        for( i=0; i < hostApisCount_; ++i )
        {
            if( hostApis_[i]->info.type == type )
            {
                *hostApi = hostApis_[i];
                result = paNoError;
                break;
            }
        }
    }

    return result;
}


PaError PaUtil_DeviceIndexToHostApiDeviceIndex(
        PaDeviceIndex *hostApiDevice, PaDeviceIndex device, struct PaUtilHostApiRepresentation *hostApi )
{
    PaError result;
    PaDeviceIndex x;
    
    x = device - hostApi->privatePaFrontInfo.baseDeviceIndex;

    if( x < 0 || x >= hostApi->deviceCount )
    {
        result = paInvalidDevice;
    }
    else
    {
        *hostApiDevice = x;
        result = paNoError;
    }

    return result;
}


PaHostApiIndex Pa_CountHostApis( void )
{

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_CountHostApis called.\n" );
#endif

    if( !PA_IS_INITIALISED_ )
    {

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_CountHostApis returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiIndex: 0 [ PortAudio not initialized ]\n\n" );
#endif

        return 0;
    }
    else
    {

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_CountHostApis returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiIndex %d\n\n", hostApisCount_ );
#endif

        return hostApisCount_;
    }
}


const PaHostApiInfo* Pa_GetHostApiInfo( PaHostApiIndex hostApi )
{
    PaHostApiInfo *info;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetHostApiInfo called:\n" );
    PaUtil_DebugPrint("\tPaHostApiIndex hostApi: %d\n", hostApi );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        info = NULL;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetHostApiInfo returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiInfo*: NULL [ PortAudio not initialized ]\n\n" );
#endif

    }
    else if( hostApi < 0 || hostApi >= hostApisCount_ )
    {
        info = NULL;
        
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetHostApiInfo returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiInfo*: NULL [ hostApi out of range ]\n\n" );
#endif

    }
    else
    {
        info = &hostApis_[hostApi]->info;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetHostApiInfo returned:\n" );
        PaUtil_DebugPrint("\tPaHostApiInfo*: 0x%p\n", info );
        PaUtil_DebugPrint("\t{" );
        PaUtil_DebugPrint("\t\tint structVersion: %d\n", info->structVersion );
        PaUtil_DebugPrint("\t\tPaHostApiTypeId type: %d\n", info->type );
        PaUtil_DebugPrint("\t\tconst char *name: %s\n\n", info->name );
        PaUtil_DebugPrint("\t}\n\n" );
#endif

    }

     return info;
}


PaDeviceIndex Pa_HostApiDefaultInputDevice( PaHostApiIndex hostApi )
{
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_HostApiDefaultInputDevice called:\n" );
    PaUtil_DebugPrint("\tPaHostApiIndex hostApi: %d\n", hostApi );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultInputDevice returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ PortAudio not initialized ]\n\n" );
#endif

    }
    else if( hostApi < 0 || hostApi >= hostApisCount_ )
    {
        result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultInputDevice returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ hostApi out of range ]\n\n" );
#endif

    }
    else if( hostApis_[hostApi]->defaultOutputDeviceIndex == paNoDevice )
    {
        result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultInputDevice returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ no default device ]\n\n" );
#endif

    }
    else
    {
        result = hostApis_[hostApi]->privatePaFrontInfo.baseDeviceIndex
                 + hostApis_[hostApi]->defaultInputDeviceIndex;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultInputDevice returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif

    }

    return result;
}


PaDeviceIndex Pa_HostApiDefaultOutputDevice( PaHostApiIndex hostApi )
{
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice called:\n" );
    PaUtil_DebugPrint("\tPaHostApiIndex hostApi: %d\n", hostApi );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ PortAudio not initialized ]\n\n" );
#endif

    }
    else if( hostApi < 0 || hostApi >= hostApisCount_ )
    {
        result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ hostApi out of range ]\n\n" );
#endif

    }
    else if( hostApis_[hostApi]->defaultOutputDeviceIndex == paNoDevice )
    {
        result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ no default device ]\n\n" );
#endif

    }
    else
    {
        result = hostApis_[hostApi]->privatePaFrontInfo.baseDeviceIndex
                 + hostApis_[hostApi]->defaultOutputDeviceIndex;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif

    }

    return result;
}


int Pa_HostApiCountDevices( PaHostApiIndex hostApi )
{
    int result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice called:\n" );
    PaUtil_DebugPrint("\tPaHostApiIndex hostApi: %d\n", hostApi );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice returned:\n" );
        PaUtil_DebugPrint("\tint: 0 [ PortAudio not initialized ]\n\n" );
#endif

    }
    else if( hostApi < 0 || hostApi >= hostApisCount_ )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice returned:\n" );
        PaUtil_DebugPrint("\tint: 0 [ hostApi out of range ]\n\n" );
#endif

    }
    else
    {
        result = hostApis_[hostApi]->deviceCount;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDefaultOutputDevice returned:\n" );
        PaUtil_DebugPrint("\tint: %d\n\n", result );
#endif

    }

    return result;
}


PaDeviceIndex Pa_HostApiDeviceIndexToDeviceIndex( PaHostApiIndex hostApi, int hostApiDeviceIndex )
{
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex called:\n" );
    PaUtil_DebugPrint("\tPaHostApiIndex hostApi: %d\n", hostApi );
    PaUtil_DebugPrint("\tint hostApiDeviceIndex: %d\n", hostApiDeviceIndex );
#endif


    if( !PA_IS_INITIALISED_ )
    {
        result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ PortAudio not initialized ]\n\n" );
#endif

    }
    else
    {
        if( hostApi < 0 || hostApi >= hostApisCount_ )
        {
            result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ hostApi out of range ]\n\n" );
#endif

        }
        else
        {
            if( hostApiDeviceIndex < 0 ||
                    hostApiDeviceIndex >= hostApis_[hostApi]->deviceCount )
            {
                result = paNoDevice;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: paNoDevice [ hostApiDeviceIndex out of range ]\n\n" );
#endif

            }
            else
            {
                result = hostApis_[hostApi]->privatePaFrontInfo.baseDeviceIndex + hostApiDeviceIndex;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_HostApiDeviceIndexToPaDeviceIndex returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif
            }
        }
    }

    return result;
}


PaDeviceIndex Pa_CountDevices( void )
{
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_CountDevices called.\n" );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_CountDevices returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: 0 [ PortAudio not initialized ]\n\n" );
#endif

    }
    else
    {
        result = deviceCount_;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_CountDevices returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif

    }

    return result;
}


PaDeviceIndex Pa_GetDefaultInputDevice( void )
{
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultInputDevice called.\n" );
#endif

    result = Pa_HostApiDefaultInputDevice( Pa_GetDefaultHostApi() );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultInputDevice returned:\n" );
    PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif

    return result;
}


PaDeviceIndex Pa_GetDefaultOutputDevice( void )
{
    PaDeviceIndex result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultOutputDevice called.\n" );
#endif

    result = Pa_HostApiDefaultOutputDevice( Pa_GetDefaultHostApi() );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetDefaultOutputDevice returned:\n" );
    PaUtil_DebugPrint("\tPaDeviceIndex: %d\n\n", result );
#endif

    return result;
}


const PaDeviceInfo* Pa_GetDeviceInfo( PaDeviceIndex device )
{
    int hostSpecificDeviceIndex;
    int hostApiIndex = FindHostApi( device, &hostSpecificDeviceIndex );
    PaDeviceInfo *result;


#ifdef PA_LOG_API_CALLS
    int i;
    PaUtil_DebugPrint("Pa_GetDeviceInfo called:\n" );
    PaUtil_DebugPrint("\tPaDeviceIndex device: %d\n", device );
#endif

    if( hostApiIndex < 0 )
    {
        result = NULL;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetDeviceInfo returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceInfo* NULL [ invalid device index ]\n\n" );
#endif

    }
    else
    {
        result = hostApis_[hostApiIndex]->deviceInfos[ hostSpecificDeviceIndex ];

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetDeviceInfo returned:\n" );
        PaUtil_DebugPrint("\tPaDeviceInfo*: 0x%p:\n", result );
        PaUtil_DebugPrint("\t{" );

        PaUtil_DebugPrint("\t\tint structVersion: %d\n", result->structVersion );
        PaUtil_DebugPrint("\t\tconst char *name: %s\n", result->name );
        PaUtil_DebugPrint("\t\tPaHostApiIndex hostApi: %d\n", result->hostApi );
        PaUtil_DebugPrint("\t\tint maxInputChannels: %d\n", result->maxInputChannels );
        PaUtil_DebugPrint("\t\tint maxOutputChannels: %d\n", result->maxOutputChannels );
        PaUtil_DebugPrint("\t\tint numSampleRates: %d\n", result->numSampleRates );

        PaUtil_DebugPrint("\t\tconst double *sampleRates: { " );
        for( i=0; i<((result->numSampleRates==-1)?2:result->numSampleRates); ++i )
        {
            if( i != 0 )
                PaUtil_DebugPrint(", " );
            PaUtil_DebugPrint("%g", result->sampleRates[i] );
        }
        PaUtil_DebugPrint(" }\n" );

        PaUtil_DebugPrint("\t\tPaSampleFormat nativeSampleFormats: 0x%x\n", result->nativeSampleFormats );
        PaUtil_DebugPrint("\t}\n\n" );
#endif

    }

    return result;
}


/*
    SampleFormatIsValid() returns 1 if sampleFormat is a sample format
    defined in portaudio.h, or 0 otherwise.
*/
static int SampleFormatIsValid( PaSampleFormat format )
{
    switch( format & ~paNonInterleaved )
    {
    case paFloat32: return 1;
    case paInt16: return 1;
    case paInt32: return 1;
    case paInt24: return 1;
    case paInt8: return 1;
    case paUInt8: return 1;
    case paCustomFormat: return 1;
    default: return 0;
    }
}

/*
    NOTE: make sure this validation list is kept syncronised with the one in
            pa_util.h
            
    ValidateOpenStreamParameters() checks that parameters to Pa_OpenStream()
    conform to the expected values as described below. This function is
    also designed to be used with the proposed Pa_IsFormatSupported() function.
    
    There are basically two types of validation that could be performed:
    Generic conformance validation, and device capability mismatch
    validation. This function performs only generic conformance validation.
    Validation that would require knowledge of device capabilities is
    not performed because of potentially complex relationships between
    combinations of parameters - for example, even if the sampleRate
    seems ok, it might not be for a duplex stream - we have no way of
    checking this in an API-neutral way, so we don't try.
 
    On success the function returns PaNoError and fills in hostApi,
    hostApiInputDeviceID, and hostApiOutputDeviceID fields. On failure
    the function returns an error code indicating the first encountered
    parameter error.
 
 
    If ValidateOpenStreamParameters() returns paNoError, the following
    assertions are guaranteed to be true.
 
    - at least one of inputDevice & outputDevice is valid (not paNoDevice)
 
    - if inputDevice & outputDevice are both valid, they both use the same host api
 
    PaDeviceIndex inputDevice
        - is within range (0 to Pa_CountDevices-1)
 
    int numInputChannels
        - if inputDevice is valid, numInputChannels is > 0
        - upper bound is NOT validated against device capabilities
 
    PaSampleFormat inputSampleFormat
        - is one of the sample formats defined in portaudio.h
 
    void *inputStreamInfo
        - if supplied its hostApi field matches the input device's host Api
 
    PaDeviceIndex outputDevice
        - is within range (0 to Pa_CountDevices-1)
 
    int numOutputChannels
        - if inputDevice is valid, numInputChannels is > 0
        - upper bound is NOT validated against device capabilities
 
    PaSampleFormat outputSampleFormat
        - is one of the sample formats defined in portaudio.h
        
    void *outputStreamInfo
        - if supplied its hostApi field matches the output device's host Api
 
    double sampleRate
        - is not an 'absurd' rate (less than 1000. or greater than 200000.)
        - sampleRate is NOT validated against device capabilities
 
    PaStreamFlags streamFlags
        - unused platform neutral flags are zero
*/
static PaError ValidateOpenStreamParameters(
    PaDeviceIndex inputDevice,
    int numInputChannels,
    PaSampleFormat inputSampleFormat,
    PaHostApiSpecificStreamInfo *inputStreamInfo,
    PaDeviceIndex outputDevice,
    int numOutputChannels,
    PaSampleFormat outputSampleFormat,
    PaHostApiSpecificStreamInfo *outputStreamInfo,
    double sampleRate,
    PaStreamFlags streamFlags,
    PaUtilHostApiRepresentation **hostApi,
    PaDeviceIndex *hostApiInputDevice,
    PaDeviceIndex *hostApiOutputDevice )
{
    int inputHostApiIndex, outputHostApiIndex;

    if( (inputDevice == paNoDevice) && (outputDevice == paNoDevice) )
    {

        return paInvalidDevice;

    }
    else
    {
        if( inputDevice == paNoDevice )
        {
            *hostApiInputDevice = paNoDevice;
        }
        else if( inputDevice == paUseHostApiSpecificDeviceSpecification )
        {
            if( inputStreamInfo )
            {
                inputHostApiIndex = Pa_HostApiTypeIdToHostApiIndex(
                        inputStreamInfo->hostApiType );

                if( inputHostApiIndex != -1 )
                {
                    *hostApiInputDevice = paUseHostApiSpecificDeviceSpecification;
                    *hostApi = hostApis_[inputHostApiIndex];
                }
                else
                {
                    return paInvalidDevice;
                }
            }
            else
            {
                return paInvalidDevice;
            }
        }
        else
        {
            if( inputDevice < 0 || inputDevice >= deviceCount_ )
                return paInvalidDevice;

            inputHostApiIndex = FindHostApi( inputDevice, hostApiInputDevice );
            if( inputHostApiIndex < 0 )
                return paInternalError;

            *hostApi = hostApis_[inputHostApiIndex];

            if( numInputChannels <= 0 )
                return paInvalidChannelCount;

            if( !SampleFormatIsValid( inputSampleFormat ) )
                return paSampleFormatNotSupported;

            if( inputStreamInfo != NULL )
            {
                if( inputStreamInfo->hostApiType != (*hostApi)->info.type )
                    return paIncompatibleStreamInfo;
            }
        }

        if( outputDevice == paNoDevice )
        {
            *hostApiOutputDevice = paNoDevice;
        }
        else if( outputDevice == paUseHostApiSpecificDeviceSpecification  )
        {
            if( outputStreamInfo )
            {
                outputHostApiIndex = Pa_HostApiTypeIdToHostApiIndex(
                        outputStreamInfo->hostApiType );

                if( outputHostApiIndex != -1 )
                {
                    *hostApiOutputDevice = paUseHostApiSpecificDeviceSpecification;
                    *hostApi = hostApis_[outputHostApiIndex];
                }
                else
                {
                    return paInvalidDevice;
                }
            }
            else
            {
                return paInvalidDevice;
            }
        }
        else
        {
            if( outputDevice < 0 || outputDevice >= deviceCount_ )
                return paInvalidDevice;

            outputHostApiIndex = FindHostApi( outputDevice, hostApiOutputDevice );
            if( outputHostApiIndex < 0 )
                return paInternalError;

            *hostApi = hostApis_[outputHostApiIndex];

            if( numOutputChannels <= 0 )
                return paInvalidChannelCount;

            if( !SampleFormatIsValid( outputSampleFormat ) )
                return paSampleFormatNotSupported;

            if( outputStreamInfo != NULL )
            {
                if( outputStreamInfo->hostApiType != (*hostApi)->info.type )
                    return paIncompatibleStreamInfo;
            }
        }   

        if( inputDevice != paNoDevice && outputDevice != paNoDevice )
        {
            /* ensure that both devices use the same API */
            if( outputHostApiIndex != inputHostApiIndex )
                return paBadIODeviceCombination;
        }
    }
    
    
    /* Check for absurd sample rates. */
    if( (sampleRate < 1000.0) || (sampleRate > 200000.0) )
        return paInvalidSampleRate;

    if( ((streamFlags & ~paPlatformSpecificFlags) & ~(paClipOff | paDitherOff)) != 0 ) return paInvalidFlag;

    return paNoError;
}


PaError Pa_OpenStream( PaStream** stream,
                       PaDeviceIndex inputDevice,
                       int numInputChannels,
                       PaSampleFormat inputSampleFormat,
                       unsigned long inputLatency,
                       PaHostApiSpecificStreamInfo *inputStreamInfo,
                       PaDeviceIndex outputDevice,
                       int numOutputChannels,
                       PaSampleFormat outputSampleFormat,
                       unsigned long outputLatency,
                       PaHostApiSpecificStreamInfo *outputStreamInfo,
                       double sampleRate,
                       unsigned long framesPerCallback,
                       PaStreamFlags streamFlags,
                       PortAudioCallback *callback,
                       void *userData )
{
    PaError result;
    PaDeviceIndex hostApiInputDevice, hostApiOutputDevice;
    PaUtilHostApiRepresentation *hostApi;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenStream called:\n" );
    PaUtil_DebugPrint("\tPaStream** stream: 0x%p\n", stream );
    PaUtil_DebugPrint("\tPaDeviceIndex inputDevice: %d\n", inputDevice );
    PaUtil_DebugPrint("\tint numInputChannels: %d\n", numInputChannels );
    PaUtil_DebugPrint("\tPaSampleFormat inputSampleFormat: %d\n", inputSampleFormat );
    PaUtil_DebugPrint("\tunsigned long inputLatency: %d\n", inputLatency );
    PaUtil_DebugPrint("\tvoid *inputStreamInfo: 0x%p\n", inputStreamInfo );
    PaUtil_DebugPrint("\tPaDeviceIndex outputDevice: %d\n", outputDevice );
    PaUtil_DebugPrint("\tint numOutputChannels: %d\n", numOutputChannels );
    PaUtil_DebugPrint("\tPaSampleFormat outputSampleFormat: %d\n", outputSampleFormat );
    PaUtil_DebugPrint("\tunsigned long outputLatency: %d\n", outputLatency );
    PaUtil_DebugPrint("\tvoid *outputStreamInfo: 0x%p\n", outputStreamInfo );
    PaUtil_DebugPrint("\tdouble sampleRate: %g\n", sampleRate );
    PaUtil_DebugPrint("\tunsigned long framesPerCallback: %d\n", framesPerCallback );
    PaUtil_DebugPrint("\tPaStreamFlags streamFlags: 0x%x\n", streamFlags );
    PaUtil_DebugPrint("\tPortAudioCallback *callback: 0x%p\n", callback );
    PaUtil_DebugPrint("\tvoid *userData: 0x%p\n", userData );
#endif

    if( !PA_IS_INITIALISED_ )
    {
        result = paNotInitialized;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
        PaUtil_DebugPrint("\t*(PaStream** stream): undefined\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        return result;
    }

    /* Check for parameter errors. */

    if( stream == NULL )
    {
        result = paBadStreamPtr;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
        PaUtil_DebugPrint("\t*(PaStream** stream): undefined\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        return result;
    }

    result = ValidateOpenStreamParameters( inputDevice, numInputChannels, inputSampleFormat,
                                           inputStreamInfo, outputDevice, numOutputChannels, outputSampleFormat,
                                           outputStreamInfo, sampleRate,
                                           streamFlags, &hostApi, &hostApiInputDevice, &hostApiOutputDevice );
    if( result != paNoError )
    {
#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
        PaUtil_DebugPrint("\t*(PaStream** stream): undefined\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif
        return result;
    }

    if( callback == NULL )
    {
        return paNullCallback; /* FIXME: remove when blocking read/write is added */
    }

    if( inputDevice == paNoDevice )
    {
        numInputChannels = 0;
        inputStreamInfo = NULL;
    }

    if( outputDevice == paNoDevice )
    {
        numOutputChannels = 0;
        outputStreamInfo = NULL;
    }

    result = hostApi->OpenStream( hostApi, stream,
                                  hostApiInputDevice, numInputChannels, inputSampleFormat, inputLatency, inputStreamInfo,
                                  hostApiOutputDevice, numOutputChannels, outputSampleFormat, outputLatency, outputStreamInfo,
                                  sampleRate, framesPerCallback, streamFlags, callback, userData );

    if( result == paNoError )
        AddOpenStream( *stream );


#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenStream returned:\n" );
    PaUtil_DebugPrint("\t*(PaStream** stream): 0x%p\n", *stream );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_OpenDefaultStream( PaStream** stream,
                              int numInputChannels,
                              int numOutputChannels,
                              PaSampleFormat sampleFormat,
                              double sampleRate,
                              unsigned long framesPerCallback,
                              PortAudioCallback *callback,
                              void *userData )
{
    PaError result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenDefaultStream called:\n" );
    PaUtil_DebugPrint("\tPaStream** stream: 0x%p\n", stream );
    PaUtil_DebugPrint("\tint numInputChannels: %d\n", numInputChannels );
    PaUtil_DebugPrint("\tint numOutputChannels: %d\n", numOutputChannels );
    PaUtil_DebugPrint("\tPaSampleFormat sampleFormat: %d\n", sampleFormat );
    PaUtil_DebugPrint("\tdouble sampleRate: %g\n", sampleRate );
    PaUtil_DebugPrint("\tunsigned long framesPerCallback: %d\n", framesPerCallback );
    PaUtil_DebugPrint("\tPortAudioCallback *callback: 0x%p\n", callback );
    PaUtil_DebugPrint("\tvoid *userData: 0x%p\n", userData );
#endif

    result = Pa_OpenStream(
                 stream,
                 ((numInputChannels > 0) ? Pa_GetDefaultInputDevice() : paNoDevice),
                 numInputChannels, sampleFormat, 0, NULL,
                 ((numOutputChannels > 0) ? Pa_GetDefaultOutputDevice() : paNoDevice),
                 numOutputChannels, sampleFormat, 0, NULL,
                 sampleRate, framesPerCallback, paNoFlag, callback, userData );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_OpenDefaultStream returned:\n" );
    PaUtil_DebugPrint("\t*(PaStream** stream): 0x%p", *stream );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


static PaError ValidateStream( PaStream* stream )
{
    if( !PA_IS_INITIALISED_ ) return paNotInitialized;

    if( stream == NULL ) return paBadStreamPtr;

    if( ((PaUtilStreamRepresentation*)stream)->magic != PA_STREAM_MAGIC )
        return paBadStreamPtr;

    return paNoError;
}


PaError Pa_CloseStream( PaStream* stream )
{
    PaUtilStreamInterface *interface;
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_CloseStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    /* always remove the open stream from our list, even if this function
        eventually returns an error. Otherwise CloseOpenStreams() will
        get stuck in an infinite loop */
    RemoveOpenStream( stream ); /* be sure to call this _before_ closing the stream */

    if( result == paNoError )
    {
        interface = PA_STREAM_INTERFACE(stream);
        if( !interface->IsStopped( stream ) )
        {
            result = interface->Abort( stream );
        }

        if( result == paNoError )                 /* REVIEW: shouldn't we close anyway? */
            result = interface->Close( stream );
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_CloseStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_StartStream( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_StartStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
    {
        if( !PA_STREAM_INTERFACE(stream)->IsStopped( stream ) )
        {
            result = paStreamIsNotStopped ;
        }
        else
        {
            result = PA_STREAM_INTERFACE(stream)->Start( stream );
        }
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_StartStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_StopStream( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_StopStream called\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
    {
        if( PA_STREAM_INTERFACE(stream)->IsStopped( stream ) )
        {
            result = paStreamIsStopped;
        }
        else
        {
            result = PA_STREAM_INTERFACE(stream)->Stop( stream );
        }
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_StopStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_AbortStream( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_AbortStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
    {
        if( PA_STREAM_INTERFACE(stream)->IsStopped( stream ) )
        {
            result = paStreamIsStopped;
        }
        else
        {
            result = PA_STREAM_INTERFACE(stream)->Abort( stream );
        }
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_AbortStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_IsStreamStopped( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsStreamStopped called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
        result = PA_STREAM_INTERFACE(stream)->IsStopped( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsStreamStopped returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_IsStreamActive( PaStream *stream )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsStreamActive called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( result == paNoError )
        result = PA_STREAM_INTERFACE(stream)->IsActive( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_IsStreamActive returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaTimestamp Pa_GetStreamTime( PaStream *stream )
{
    PaError error = ValidateStream( stream );
    PaTimestamp result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamTime called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamTime returned:\n" );
        PaUtil_DebugPrint("\tPaTimestamp: 0 [PaError error:%d ( %s )]\n\n", result, error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetTime( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamTime returned:\n" );
        PaUtil_DebugPrint("\tPaTimestamp: %g\n\n", result );
#endif

    }

    return result;
}


double Pa_GetStreamCpuLoad( PaStream* stream )
{
    PaError error = ValidateStream( stream );
    double result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamCpuLoad called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {

        result = 0.0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamCpuLoad returned:\n" );
        PaUtil_DebugPrint("\tdouble: 0.0 [PaError error: %d ( %s )]\n\n", error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetCpuLoad( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamCpuLoad returned:\n" );
        PaUtil_DebugPrint("\tdouble: %g\n\n", result );
#endif

    }

    return result;
}


PaError Pa_ReadStream( PaStream* stream,
                       void *buffer,
                       unsigned long frames )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_ReadStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    /* FIXME: should return an error if buffer is zero or frames <= 0 */
    if( frames > 0 && buffer != 0 )
        result = PA_STREAM_INTERFACE(stream)->Read( stream, buffer, frames );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_ReadStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}


PaError Pa_WriteStream( PaStream* stream,
                        void *buffer,
                        unsigned long frames )
{
    PaError result = ValidateStream( stream );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_WriteStream called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    /* FIXME: should return an error if buffer is zero or frames <= 0 */
    if( frames > 0 && buffer != 0 )
        result = PA_STREAM_INTERFACE(stream)->Write( stream, buffer, frames );

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_WriteStream returned:\n" );
    PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return result;
}

unsigned long Pa_GetStreamReadAvailable( PaStream* stream )
{
    PaError error = ValidateStream( stream );
    unsigned long result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamReadAvailable called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamReadAvailable returned:\n" );
        PaUtil_DebugPrint("\tunsigned long: 0 [ PaError error: %d ( %s ) ]\n\n", error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetReadAvailable( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamReadAvailable returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    }

    return result;
}


unsigned long Pa_GetStreamWriteAvailable( PaStream* stream )
{
    PaError error = ValidateStream( stream );
    unsigned long result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetStreamWriteAvailable called:\n" );
    PaUtil_DebugPrint("\tPaStream* stream: 0x%p\n", stream );
#endif

    if( error != paNoError )
    {
        result = 0;

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamWriteAvailable returned:\n" );
        PaUtil_DebugPrint("\tunsigned long: 0 [ PaError error: %d ( %s ) ]\n\n", error, Pa_GetErrorText( error ) );
#endif

    }
    else
    {
        result = PA_STREAM_INTERFACE(stream)->GetWriteAvailable( stream );

#ifdef PA_LOG_API_CALLS
        PaUtil_DebugPrint("Pa_GetStreamWriteAvailable returned:\n" );
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    }

    return result;
}


PaError Pa_GetSampleSize( PaSampleFormat format )
{
    int result;

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetSampleSize called:\n" );
    PaUtil_DebugPrint("\tPaSampleFormat format: %d\n", format );
#endif

    switch( format & ~paNonInterleaved )
    {

    case paUInt8:
    case paInt8:
        result = 1;
        break;

    case paInt16:
        result = 2;
        break;

    case paInt24:
        result = 3;
        break;

    case paFloat32:
    case paInt32:
        result = 4;
        break;

    default:
        result = paSampleFormatNotSupported;
        break;
    }

#ifdef PA_LOG_API_CALLS
    PaUtil_DebugPrint("Pa_GetSampleSize returned:\n" );
    if( result > 0 )
        PaUtil_DebugPrint("\tint: %d\n\n", result );
    else
        PaUtil_DebugPrint("\tPaError: %d ( %s )\n\n", result, Pa_GetErrorText( result ) );
#endif

    return (PaError) result;
}

