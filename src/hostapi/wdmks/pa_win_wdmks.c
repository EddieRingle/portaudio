/*
* $Id$
* PortAudio Windows WDM-KS interface
*
* Author: Andrew Baldwin, Robert Bielik (WaveRT)
* Based on the Open Source API proposed by Ross Bencina
* Copyright (c) 1999-2004 Andrew Baldwin, Ross Bencina, Phil Burk
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
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
* ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
* CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
* WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
* The text above constitutes the entire PortAudio license; however, 
* the PortAudio community also makes the following non-binding requests:
*
* Any person wishing to distribute modifications to the Software is
* requested to send the modifications to the original developer so that
* they can be incorporated into the canonical version. It is also 
* requested that these non-binding requests be included along with the 
* license above.
*/

/** @file
@ingroup hostapi_src
@brief Portaudio WDM-KS host API.

@note This is the implementation of the Portaudio host API using the
Windows WDM/Kernel Streaming API in order to enable very low latency
playback and recording on all modern Windows platforms (e.g. 2K, XP, Vista, Win7)
Note: This API accesses the device drivers below the usual KMIXER
component which is normally used to enable multi-client mixing and
format conversion. That means that it will lock out all other users
of a device for the duration of active stream using those devices
*/

/* Exclude compilation altogether if not wanted. Will remove linking to setupapi.lib */
#ifndef PA_NO_WDMKS

#include <stdio.h>

#if (defined(WIN32) && (defined(_MSC_VER) && (_MSC_VER >= 1200))) /* MSC version 6 and above */
#pragma comment( lib, "setupapi.lib" )
#endif

/* Debugging/tracing support */

#define PA_LOGE_
#define PA_LOGL_

/* The __PA_DEBUG macro is used in RT parts, so it can be switched off without affecting
   the rest of the debug tracing */
#if 0
#define __PA_DEBUG PA_DEBUG
#else
#define __PA_DEBUG(x)
#endif

#ifdef __GNUC__
#include <initguid.h>
#define _WIN32_WINNT 0x0501
#define WINVER 0x0501
#endif

#include <string.h> /* strlen() */
#include <assert.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"
#include "portaudio.h"
#include "pa_debugprint.h"
#include "pa_memorybarrier.h"

#include <windows.h>
#include <winioctl.h>
#include <process.h>

#include <math.h>

#ifdef __GNUC__
#undef PA_LOGE_
#define PA_LOGE_ PA_DEBUG(("%s {\n",__FUNCTION__))
#undef PA_LOGL_
#define PA_LOGL_ PA_DEBUG(("} %s\n",__FUNCTION__))
/* These defines are set in order to allow the WIndows DirectX
* headers to compile with a GCC compiler such as MinGW
* NOTE: The headers may generate a few warning in GCC, but
* they should compile */
#define _INC_MMSYSTEM
#define _INC_MMREG
#define _NTRTL_ /* Turn off default definition of DEFINE_GUIDEX */
#define DEFINE_GUID_THUNK(name,guid) DEFINE_GUID(name,guid)
#define DEFINE_GUIDEX(n) DEFINE_GUID_THUNK( n, STATIC_##n )
#if !defined( DEFINE_WAVEFORMATEX_GUID )
#define DEFINE_WAVEFORMATEX_GUID(x) (USHORT)(x), 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
#endif
#define  WAVE_FORMAT_ADPCM      0x0002
#define  WAVE_FORMAT_IEEE_FLOAT 0x0003
#define  WAVE_FORMAT_ALAW       0x0006
#define  WAVE_FORMAT_MULAW      0x0007
#define  WAVE_FORMAT_MPEG       0x0050
#define  WAVE_FORMAT_DRM        0x0009
#define DYNAMIC_GUID_THUNK(l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8) {l,w1,w2,{b1,b2,b3,b4,b5,b6,b7,b8}}
#define DYNAMIC_GUID(data) DYNAMIC_GUID_THUNK(data)
#endif

/* use CreateThread for CYGWIN/Windows Mobile, _beginthreadex for all others */
#if !defined(__CYGWIN__) && !defined(_WIN32_WCE)
#define CREATE_THREAD_FUNCTION (HANDLE)_beginthreadex
#define PA_THREAD_FUNC static unsigned WINAPI
#else
#define CREATE_THREAD_FUNCTION CreateThread
#define PA_THREAD_FUNC static DWORD WINAPI
#endif

#ifdef _MSC_VER
#define NOMMIDS
#define DYNAMIC_GUID(data) {data}
#define _NTRTL_ /* Turn off default definition of DEFINE_GUIDEX */
#undef DEFINE_GUID
#define DEFINE_GUID(n,data) EXTERN_C const GUID n = {data}
#define DEFINE_GUID_THUNK(n,data) DEFINE_GUID(n,data)
#define DEFINE_GUIDEX(n) DEFINE_GUID_THUNK(n, STATIC_##n)
#endif

#include <setupapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <tchar.h>
#include <assert.h>
#include <stdio.h>

/* These next definitions allow the use of the KSUSER DLL */
typedef KSDDKAPI DWORD WINAPI KSCREATEPIN(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);
extern HMODULE      DllKsUser;
extern KSCREATEPIN* FunctionKsCreatePin;

/* These definitions allows the use of AVRT.DLL on Vista and later OSs */
extern HMODULE      DllAvRt;
typedef HANDLE WINAPI AVSETMMTHREADCHARACTERISTICS(LPCSTR, LPDWORD TaskIndex);
typedef BOOL WINAPI AVREVERTMMTHREADCHARACTERISTICS(HANDLE);
typedef enum _PA_AVRT_PRIORITY
{
    PA_AVRT_PRIORITY_LOW = -1,
    PA_AVRT_PRIORITY_NORMAL,
    PA_AVRT_PRIORITY_HIGH,
    PA_AVRT_PRIORITY_CRITICAL
} PA_AVRT_PRIORITY, *PPA_AVRT_PRIORITY;
typedef BOOL WINAPI AVSETMMTHREADPRIORITY(HANDLE, PA_AVRT_PRIORITY);
extern AVSETMMTHREADCHARACTERISTICS* FunctionAvSetMmThreadCharacteristics;
extern AVREVERTMMTHREADCHARACTERISTICS* FunctionAvRevertMmThreadCharacteristics;
extern AVSETMMTHREADPRIORITY* FunctionAvSetMmThreadPriority;

/* Forward definition to break circular type reference between pin and filter */
struct __PaWinWdmFilter;
typedef struct __PaWinWdmFilter PaWinWdmFilter;

struct __PaWinWdmPin;
typedef struct __PaWinWdmPin PaWinWdmPin;

struct __PaWinWdmStream;
typedef struct __PaWinWdmStream PaWinWdmStream;

/* Function prototype for getting audio position */
typedef PaError (*FunctionGetPinAudioPosition)(PaWinWdmPin*, unsigned long*);

/* Function prototype for memory barrier */
typedef void (*FunctionMemoryBarrier)(void);

struct __PaProcessThreadInfo;
typedef struct __PaProcessThreadInfo PaProcessThreadInfo;

typedef void (*FunctionPinHandler)(PaProcessThreadInfo* pInfo, unsigned eventIndex);

/* The Pin structure
* A pin is an input or output node, e.g. for audio flow */
struct __PaWinWdmPin
{
    HANDLE                      handle;
    PaWinWdmFilter*             parentFilter;
    unsigned long               pinId;
    KSPIN_CONNECT*              pinConnect;
    unsigned long               pinConnectSize;
    KSDATAFORMAT_WAVEFORMATEX*  ksDataFormatWfx;
    KSPIN_COMMUNICATION         communication;
    KSDATARANGE*                dataRanges;
    KSMULTIPLE_ITEM*            dataRangesItem;
    KSPIN_DATAFLOW              dataFlow;
    KSPIN_CINSTANCES            instances;
    unsigned long               frameSize;
    int                         maxChannels;
    unsigned long               formats;
    int                         bestSampleRate;
    ULONG                       *positionRegister;  /* WaveRT */
    ULONG                       hwLatency;          /* WaveRT */
    FunctionMemoryBarrier       fnMemBarrier;       /* WaveRT */
    FunctionGetPinAudioPosition fnAudioPosition;    /* WaveRT */
    FunctionPinHandler          fnEventHandler;
    FunctionPinHandler          fnSubmitHandler;
};

/* The Filter structure
* A filter has a number of pins and a "friendly name" */
struct __PaWinWdmFilter
{
    HANDLE         handle;
    int            isWaveRT;
    DWORD          deviceNode;
    int            pinCount;
    PaWinWdmPin**  pins;
    TCHAR          filterName[MAX_PATH];
    TCHAR          friendlyName[MAX_PATH];
    int            maxInputChannels;
    int            maxOutputChannels;
    unsigned long  formats;
    int            usageCount;
    int            bestSampleRate;
};

/* PaWinWdmHostApiRepresentation - host api datastructure specific to this implementation */
typedef struct __PaWinWdmHostApiRepresentation
{
    PaUtilHostApiRepresentation  inheritedHostApiRep;
    PaUtilStreamInterface        callbackStreamInterface;
    PaUtilStreamInterface        blockingStreamInterface;

    PaUtilAllocationGroup*       allocations;
    PaWinWdmFilter**             filters;
    int                          filterCount;
}
PaWinWdmHostApiRepresentation;

typedef struct __PaWinWdmDeviceInfo
{
    PaDeviceInfo     inheritedDeviceInfo;
    PaWinWdmFilter*  filter;
}
PaWinWdmDeviceInfo;

typedef struct __DATAPACKET
{
    KSSTREAM_HEADER  Header;
    OVERLAPPED       Signal;
} DATAPACKET;

/* PaWinWdmStream - a stream data structure specifically for this implementation */
typedef struct __PaWinWdmStream
{
    PaUtilStreamRepresentation  streamRepresentation;
    PaUtilCpuLoadMeasurer       cpuLoadMeasurer;
    PaUtilBufferProcessor       bufferProcessor;

    PaWinWdmPin*                capturePin;
    PaWinWdmPin*                renderPin;
    char*                       hostInBuffer;
    char*                       hostOutBuffer;
    char*                       hostSilenceBuffer;
    unsigned long               framesPerHostIBuffer;
    unsigned long               framesPerHostOBuffer;
    int                         bytesPerInputFrame;
    int                         bytesPerOutputFrame;
    int                         streamStarted;
    int                         streamActive;
    int                         streamStop;
    int                         streamAbort;
    int                         oldProcessPriority;
    HANDLE                      streamThread;
    HANDLE                      eventAbort;
    HANDLE                      eventsRender[2];    /* 2 (WaveCyclic) 1 (WaveRT) */
    HANDLE                      eventsCapture[2];   /* 2 (WaveCyclic) 1 (WaveRT) */
    DATAPACKET                  packetsRender[2];   /* 2 render packets */
    DATAPACKET                  packetsCapture[2];  /* 2 capture packets */
    PaStreamFlags               streamFlags;
    /* These values handle the case where the user wants to use fewer
    * channels than the device has */
    int                         userInputChannels;
    int                         deviceInputChannels;
    int                         userOutputChannels;
    int                         deviceOutputChannels;
    int                         inputSampleSize;
    int                         outputSampleSize;
};

/* Gather all processing variables in a struct */
struct __PaProcessThreadInfo 
{
    PaWinWdmStream              *stream;
    PaStreamCallbackTimeInfo    ti;
    PaStreamCallbackFlags       underover;
    int                         cbResult;
    volatile int                pending;
    volatile int                priming;
    unsigned long               timeout;
    unsigned                    captureHead;
    unsigned                    renderHead;
    unsigned                    captureTail;
    unsigned                    renderTail;

    DATAPACKET*                 capturePackets[4];
    DATAPACKET*                 renderPackets[4];

    unsigned                    inBufferSize;       /* WaveRT */
    unsigned                    outBufferSize;      /* WaveRT */
    unsigned                    lastOutBuf;         /* WaveRT */

};

static const unsigned cPacketsArrayMask = 3;

HMODULE         DllKsUser = NULL;
KSCREATEPIN*    FunctionKsCreatePin = NULL;

HMODULE         DllAvRt = NULL;
AVSETMMTHREADCHARACTERISTICS* FunctionAvSetMmThreadCharacteristics = NULL;
AVREVERTMMTHREADCHARACTERISTICS* FunctionAvRevertMmThreadCharacteristics = NULL;
AVSETMMTHREADPRIORITY* FunctionAvSetMmThreadPriority = NULL;

/* prototypes for functions declared in this file */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

    PaError PaWinWdm_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

#ifdef __cplusplus
}
#endif /* __cplusplus */

/* Low level I/O functions */
static PaError WdmSyncIoctl2(HANDLE handle,
                             unsigned long ioctlNumber,
                             void* inBuffer,
                             unsigned long inBufferCount,
                             void* outBuffer,
                             unsigned long outBufferCount,
                             unsigned long* bytesReturned,
                             OVERLAPPED* pOverlapped);
static PaError WdmSyncIoctl(HANDLE handle,
                            unsigned long ioctlNumber,
                            void* inBuffer,
                            unsigned long inBufferCount,
                            void* outBuffer,
                            unsigned long outBufferCount,
                            unsigned long* bytesReturned);
static PaError WdmGetPropertySimple(HANDLE handle,
                                    const GUID* const guidPropertySet,
                                    unsigned long property,
                                    void* value,
                                    unsigned long valueCount,
                                    void* instance,
                                    unsigned long instanceCount);
static PaError WdmSetPropertySimple(HANDLE handle,
                                    const GUID* const guidPropertySet,
                                    unsigned long property,
                                    void* value,
                                    unsigned long valueCount,
                                    void* instance,
                                    unsigned long instanceCount);
static PaError WdmGetPinPropertySimple(HANDLE  handle,
                                       unsigned long pinId,
                                       const GUID* const guidPropertySet,
                                       unsigned long property,
                                       void* value,
                                       unsigned long valueCount);
static PaError WdmGetPinPropertyMulti(HANDLE  handle,
                                      unsigned long pinId,
                                      const GUID* const guidPropertySet,
                                      unsigned long property,
                                      KSMULTIPLE_ITEM** ksMultipleItem);

/** Pin management functions */
static PaWinWdmPin* PinNew(PaWinWdmFilter* parentFilter, unsigned long pinId, PaError* error);
static void PinFree(PaWinWdmPin* pin);
static void PinClose(PaWinWdmPin* pin);
static PaError PinInstantiate(PaWinWdmPin* pin);
/*static PaError PinGetState(PaWinWdmPin* pin, KSSTATE* state); NOT USED */
static PaError PinSetState(PaWinWdmPin* pin, KSSTATE state);
static PaError PinSetFormat(PaWinWdmPin* pin, const WAVEFORMATEX* format);
static PaError PinIsFormatSupported(PaWinWdmPin* pin, const WAVEFORMATEX* format);
/* WaveRT support */
static PaError PinGetBufferWithNotification(PaWinWdmPin* pPin, void** pBuffer, DWORD* pRequestedBufSize, BOOL* pbCallMemBarrier);
static PaError PinRegisterPositionRegister(PaWinWdmPin* pPin);
static PaError PinRegisterNotificationHandle(PaWinWdmPin* pPin, HANDLE handle);
static PaError PinUnregisterNotificationHandle(PaWinWdmPin* pPin, HANDLE handle);
static PaError PinGetHwLatency(PaWinWdmPin* pPin, ULONG* pFifoSize, ULONG* pChipsetDelay, ULONG* pCodecDelay);
static PaError PinGetAudioPositionDirect(PaWinWdmPin* pPin, ULONG* pPosition);
static PaError PinGetAudioPositionViaIOCTL(PaWinWdmPin* pPin, ULONG* pPosition);

/* Filter management functions */
static PaWinWdmFilter* FilterNew(BOOL fRealtime,
                                 DWORD devNode,
                                 TCHAR* filterName,
                                 TCHAR* friendlyName,
                                 PaError* error);
static void FilterFree(PaWinWdmFilter* filter);
static PaWinWdmPin* FilterCreateRenderPin(
    PaWinWdmFilter* filter,
    const WAVEFORMATEX* wfex,
    PaError* error);
static PaWinWdmPin* FilterFindViableRenderPin(
    PaWinWdmFilter* filter,
    const WAVEFORMATEX* wfex,
    PaError* error);
static PaError FilterCanCreateRenderPin(
                                        PaWinWdmFilter* filter,
                                        const WAVEFORMATEX* wfex);
static PaWinWdmPin* FilterCreateCapturePin(
    PaWinWdmFilter* filter,
    const WAVEFORMATEX* wfex,
    PaError* error);
static PaWinWdmPin* FilterFindViableCapturePin(
    PaWinWdmFilter* filter,
    const WAVEFORMATEX* wfex,
    PaError* error);
static PaError FilterCanCreateCapturePin(
    PaWinWdmFilter* filter,
    const WAVEFORMATEX* pwfx);
static PaError FilterUse(
                         PaWinWdmFilter* filter);
static void FilterRelease(
                          PaWinWdmFilter* filter);

/* Interface functions */
static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError IsFormatSupported(
struct PaUtilHostApiRepresentation *hostApi,
    const PaStreamParameters *inputParameters,
    const PaStreamParameters *outputParameters,
    double sampleRate );
static PaError OpenStream(
struct PaUtilHostApiRepresentation *hostApi,
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
static PaError ReadStream(
                          PaStream* stream,
                          void *buffer,
                          unsigned long frames );
static PaError WriteStream(
                           PaStream* stream,
                           const void *buffer,
                           unsigned long frames );
static signed long GetStreamReadAvailable( PaStream* stream );
static signed long GetStreamWriteAvailable( PaStream* stream );
static PaError GetStreamInfo(PaStream* stream, void *inInfo, void *outInfo);

/* Utility functions */
static unsigned long GetWfexSize(const WAVEFORMATEX* wfex);
static PaError BuildFilterList(PaWinWdmHostApiRepresentation* wdmHostApi);
static BOOL PinWrite(HANDLE h, DATAPACKET* p);
static BOOL PinRead(HANDLE h, DATAPACKET* p);
static void DuplicateFirstChannelInt16(void* buffer, int channels, int samples);
static void DuplicateFirstChannelInt24(void* buffer, int channels, int samples);
PA_THREAD_FUNC ProcessingThread(void*);

/* Pin handler functions */
static void PaPinCaptureEventHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static void PaPinCaptureSubmitHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex);

static void PaPinRenderEventHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static void PaPinRenderSubmitHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex);

static void PaPinCaptureEventHandler_WaveRT(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static void PaPinCaptureSubmitHandler_WaveRT(PaProcessThreadInfo* pInfo, unsigned eventIndex);

static void PaPinRenderEventHandler_WaveRT(PaProcessThreadInfo* pInfo, unsigned eventIndex);
static void PaPinRenderSubmitHandler_WaveRT(PaProcessThreadInfo* pInfo, unsigned eventIndex);

/* Function bodies */

#if defined(_DEBUG) && defined(PA_ENABLE_DEBUG_OUTPUT)
#define PA_WDMKS_SET_TREF
static PaTime tRef = 0;

static void PaWinWdmDebugPrintf(const char* fmt, ...)
{
    va_list list;
    char buffer[1024];
    PaTime t = PaUtil_GetTime() - tRef;
    va_start(list, fmt);
    _vsnprintf(buffer, 1023, fmt, list);
    va_end(list);
    PaUtil_DebugPrint("%6.3lf: %s", t, buffer);
}

#ifdef PA_DEBUG
#undef PA_DEBUG
#define PA_DEBUG(x)    PaWinWdmDebugPrintf x ;
#endif
#endif

static void MemoryBarrierDummy(void)
{
    /* Do nothing */
}

static void MemoryBarrierRead(void)
{
    PaUtil_ReadMemoryBarrier();
}

static void MemoryBarrierWrite(void)
{
    PaUtil_WriteMemoryBarrier();
}

static unsigned long GetWfexSize(const WAVEFORMATEX* wfex)
{
    if( wfex->wFormatTag == WAVE_FORMAT_PCM )
    {
        return sizeof( WAVEFORMATEX );
    }
    else
    {
        return (sizeof( WAVEFORMATEX ) + wfex->cbSize);
    }
}

static void PaWindWDM_SetLastErrorInfo(long errCode, const char* fmt, ...)
{
    va_list list;
    char buffer[1024];
    va_start(list, fmt);
    _vsnprintf(buffer, 1023, fmt, list);
    va_end(list);
    PaUtil_SetLastHostErrorInfo(paWDMKS, errCode, buffer);
}

/*
   Another variant that needs an external OVERLAPPED struct, used for getting audio position
   if WaveRT driver doesn't support mapping of position register to user space. In that case
   we don't want to allocate/free memory, or create/close events in the processing thread 
*/
static PaError WdmSyncIoctl2(HANDLE handle,
                             unsigned long ioctlNumber,
                             void* inBuffer,
                             unsigned long inBufferCount,
                             void* outBuffer,
                             unsigned long outBufferCount,
                             unsigned long* bytesReturned,
                             OVERLAPPED* pOverlapped)
{
    PaError result = paNoError;
    int boolResult;
    unsigned long error;

    assert(bytesReturned != NULL);

    boolResult = DeviceIoControl(handle, ioctlNumber, inBuffer, inBufferCount,
        outBuffer, outBufferCount, bytesReturned, pOverlapped);
    if( !boolResult )
    {
        error = GetLastError();
        if( error == ERROR_IO_PENDING )
        {
            error = WaitForSingleObject(pOverlapped->hEvent,INFINITE);
            if( error != WAIT_OBJECT_0 )
            {
                result = paUnanticipatedHostError;
            }
        }
        else if((( error == ERROR_INSUFFICIENT_BUFFER ) ||
            ( error == ERROR_MORE_DATA )) &&
            ( ioctlNumber == IOCTL_KS_PROPERTY ) &&
            ( outBufferCount == 0 ))
        {
            boolResult = TRUE;
        }
        else
        {
            result = paUnanticipatedHostError;
        }
    }
    if( !boolResult )
        *bytesReturned = 0;

    return result;
}


/*
Low level pin/filter access functions
*/
static PaError WdmSyncIoctl(
                            HANDLE handle,
                            unsigned long ioctlNumber,
                            void* inBuffer,
                            unsigned long inBufferCount,
                            void* outBuffer,
                            unsigned long outBufferCount,
                            unsigned long* bytesReturned)
{
    PaError result = paNoError;
    HANDLE hEvent;
    unsigned long dummyBytesReturned;

    if( !bytesReturned )
    {
        /* Use a dummy as the caller hasn't supplied one */
        bytesReturned = &dummyBytesReturned;
    }

    hEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
    if( !hEvent )
    {
        result = paInsufficientMemory;
    }
    else {
        OVERLAPPED overlapped;
        
        /* A valid event handle whose low-order bit is set keeps I/O completion from being queued to the completion port.
           See "GetQueuedCompletionStatus" on MSDN */
        overlapped.hEvent = (HANDLE)((DWORD_PTR)hEvent | 0x1);

        result = WdmSyncIoctl2(handle, ioctlNumber, inBuffer, inBufferCount,
            outBuffer, outBufferCount, bytesReturned, &overlapped);

        CloseHandle( hEvent );
    }
    return result;
}

static PaError WdmGetPropertySimple(HANDLE handle,
                                    const GUID* const guidPropertySet,
                                    unsigned long property,
                                    void* value,
                                    unsigned long valueCount,
                                    void* instance,
                                    unsigned long instanceCount)
{
    PaError result;
    KSPROPERTY* ksProperty;
    unsigned long propertyCount;

    propertyCount = sizeof(KSPROPERTY) + instanceCount;
    ksProperty = (KSPROPERTY*)_alloca( propertyCount );
    if( !ksProperty )
    {
        return paInsufficientMemory;
    }

    FillMemory((void*)ksProperty,sizeof(ksProperty),0);
    ksProperty->Set = *guidPropertySet;
    ksProperty->Id = property;
    ksProperty->Flags = KSPROPERTY_TYPE_GET;

    if( instance )
    {
        memcpy( (void*)(((char*)ksProperty)+sizeof(KSPROPERTY)), instance, instanceCount );
    }

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        ksProperty,
        propertyCount,
        value,
        valueCount,
        NULL);

    return result;
}

static PaError WdmSetPropertySimple(
                                    HANDLE handle,
                                    const GUID* const guidPropertySet,
                                    unsigned long property,
                                    void* value,
                                    unsigned long valueCount,
                                    void* instance,
                                    unsigned long instanceCount)
{
    PaError result;
    KSPROPERTY* ksProperty;
    unsigned long propertyCount  = 0;

    propertyCount = sizeof(KSPROPERTY) + instanceCount;
    ksProperty = (KSPROPERTY*)_alloca( propertyCount );
    if( !ksProperty )
    {
        return paInsufficientMemory;
    }

    ksProperty->Set = *guidPropertySet;
    ksProperty->Id = property;
    ksProperty->Flags = KSPROPERTY_TYPE_SET;

    if( instance )
    {
        memcpy((void*)((char*)ksProperty + sizeof(KSPROPERTY)), instance, instanceCount);
    }

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        ksProperty,
        propertyCount,
        value,
        valueCount,
        NULL);

    return result;
}

static PaError WdmGetPinPropertySimple(
                                       HANDLE  handle,
                                       unsigned long pinId,
                                       const GUID* const guidPropertySet,
                                       unsigned long property,
                                       void* value,
                                       unsigned long valueCount)
{
    PaError result;

    KSP_PIN ksPProp;
    ksPProp.Property.Set = *guidPropertySet;
    ksPProp.Property.Id = property;
    ksPProp.Property.Flags = KSPROPERTY_TYPE_GET;
    ksPProp.PinId = pinId;
    ksPProp.Reserved = 0;

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        &ksPProp,
        sizeof(KSP_PIN),
        value,
        valueCount,
        NULL);

    return result;
}

static PaError WdmGetPinPropertyMulti(
                                      HANDLE handle,
                                      unsigned long pinId,
                                      const GUID* const guidPropertySet,
                                      unsigned long property,
                                      KSMULTIPLE_ITEM** ksMultipleItem)
{
    PaError result;
    unsigned long multipleItemSize = 0;
    KSP_PIN ksPProp;

    ksPProp.Property.Set = *guidPropertySet;
    ksPProp.Property.Id = property;
    ksPProp.Property.Flags = KSPROPERTY_TYPE_GET;
    ksPProp.PinId = pinId;
    ksPProp.Reserved = 0;

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        &ksPProp.Property,
        sizeof(KSP_PIN),
        NULL,
        0,
        &multipleItemSize);
    if( result != paNoError )
    {
        return result;
    }

    *ksMultipleItem = (KSMULTIPLE_ITEM*)PaUtil_AllocateMemory( multipleItemSize );
    if( !*ksMultipleItem )
    {
        return paInsufficientMemory;
    }

    result = WdmSyncIoctl(
        handle,
        IOCTL_KS_PROPERTY,
        &ksPProp,
        sizeof(KSP_PIN),
        (void*)*ksMultipleItem,
        multipleItemSize,
        NULL);

    if( result != paNoError )
    {
        PaUtil_FreeMemory( ksMultipleItem );
    }

    return result;
}

/*
Create a new pin object belonging to a filter
The pin object holds all the configuration information about the pin
before it is opened, and then the handle of the pin after is opened
*/
static PaWinWdmPin* PinNew(PaWinWdmFilter* parentFilter, unsigned long pinId, PaError* error)
{
    PaWinWdmPin* pin;
    PaError result;
    unsigned long i;
    KSMULTIPLE_ITEM* item = NULL;
    KSIDENTIFIER* identifier;
    KSDATARANGE* dataRange;
    const ULONG streamingId = parentFilter->isWaveRT ? KSINTERFACE_STANDARD_LOOPED_STREAMING : KSINTERFACE_STANDARD_STREAMING;

    PA_LOGE_;
    PA_DEBUG(("Creating pin %d:\n",pinId));

    /* Allocate the new PIN object */
    pin = (PaWinWdmPin*)PaUtil_AllocateMemory( sizeof(PaWinWdmPin) );
    if( !pin )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Zero the pin object */
    /* memset( (void*)pin, 0, sizeof(PaWinWdmPin) ); */

    pin->parentFilter = parentFilter;
    pin->pinId = pinId;

    /* Allocate a connect structure */
    pin->pinConnectSize = sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX);
    pin->pinConnect = (KSPIN_CONNECT*)PaUtil_AllocateMemory( pin->pinConnectSize );
    if( !pin->pinConnect )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Configure the connect structure with default values */
    pin->pinConnect->Interface.Set               = KSINTERFACESETID_Standard;
    pin->pinConnect->Interface.Id                = streamingId;
    pin->pinConnect->Interface.Flags             = 0;
    pin->pinConnect->Medium.Set                  = KSMEDIUMSETID_Standard;
    pin->pinConnect->Medium.Id                   = KSMEDIUM_TYPE_ANYINSTANCE;
    pin->pinConnect->Medium.Flags                = 0;
    pin->pinConnect->PinId                       = pinId;
    pin->pinConnect->PinToHandle                 = NULL;
    pin->pinConnect->Priority.PriorityClass      = KSPRIORITY_NORMAL;
    pin->pinConnect->Priority.PrioritySubClass   = 1;
    pin->ksDataFormatWfx = (KSDATAFORMAT_WAVEFORMATEX*)(pin->pinConnect + 1);
    pin->ksDataFormatWfx->DataFormat.FormatSize  = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    pin->ksDataFormatWfx->DataFormat.Flags       = 0;
    pin->ksDataFormatWfx->DataFormat.Reserved    = 0;
    pin->ksDataFormatWfx->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    pin->ksDataFormatWfx->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
    pin->ksDataFormatWfx->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    pin->frameSize = 0; /* Unknown until we instantiate pin */

    /* Get the COMMUNICATION property */
    result = WdmGetPinPropertySimple(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_COMMUNICATION,
        &pin->communication,
        sizeof(KSPIN_COMMUNICATION));
    if( result != paNoError )
        goto error;

    if( /*(pin->communication != KSPIN_COMMUNICATION_SOURCE) &&*/
        (pin->communication != KSPIN_COMMUNICATION_SINK) &&
        (pin->communication != KSPIN_COMMUNICATION_BOTH) )
    {
        PA_DEBUG(("Not source/sink\n"));
        result = paInvalidDevice;
        goto error;
    }

    /* Get dataflow information */
    result = WdmGetPinPropertySimple(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_DATAFLOW,
        &pin->dataFlow,
        sizeof(KSPIN_DATAFLOW));

    if( result != paNoError )
        goto error;

    /* Get the INTERFACE property list */
    result = WdmGetPinPropertyMulti(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_INTERFACES,
        &item);

    if( result != paNoError )
        goto error;

    identifier = (KSIDENTIFIER*)(item+1);

    /* Check that at least one interface is STANDARD_STREAMING */
    result = paUnanticipatedHostError;
    for( i = 0; i < item->Count; i++ )
    {
        if( !memcmp( (void*)&identifier[i].Set, (void*)&KSINTERFACESETID_Standard, sizeof( GUID ) ) &&
            ( identifier[i].Id == streamingId ) )
        {
            result = paNoError;
            break;
        }
    }

    if( result != paNoError )
    {
        PA_DEBUG(("No %s streaming\n", streamingId==KSINTERFACE_STANDARD_LOOPED_STREAMING?"looped":"standard"));
        goto error;
    }

    /* Don't need interfaces any more */
    PaUtil_FreeMemory( item );
    item = NULL;

    /* Get the MEDIUM properties list */
    result = WdmGetPinPropertyMulti(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_MEDIUMS,
        &item);

    if( result != paNoError )
        goto error;

    identifier = (KSIDENTIFIER*)(item+1); /* Not actually necessary... */

    /* Check that at least one medium is STANDARD_DEVIO */
    result = paUnanticipatedHostError;
    for( i = 0; i < item->Count; i++ )
    {
        if( !memcmp( (void*)&identifier[i].Set, (void*)&KSMEDIUMSETID_Standard, sizeof( GUID ) ) &&
            ( identifier[i].Id == KSMEDIUM_STANDARD_DEVIO ) )
        {
            result = paNoError;
            break;
        }
    }

    if( result != paNoError )
    {
        PA_DEBUG(("No standard devio\n"));
        goto error;
    }
    /* Don't need mediums any more */
    PaUtil_FreeMemory( item );
    item = NULL;

    /* Get DATARANGES */
    result = WdmGetPinPropertyMulti(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_DATARANGES,
        &pin->dataRangesItem);

    if( result != paNoError )
        goto error;

    pin->dataRanges = (KSDATARANGE*)(pin->dataRangesItem +1);

    /* Check that at least one datarange supports audio */
    result = paUnanticipatedHostError;
    dataRange = pin->dataRanges;
    pin->maxChannels = 0;
    pin->bestSampleRate = 0;
    pin->formats = 0;
    for( i = 0; i <pin->dataRangesItem->Count; i++)
    {
        PA_DEBUG(("DR major format %x\n",*(unsigned long*)(&(dataRange->MajorFormat))));
        /* Check that subformat is WAVEFORMATEX, PCM or WILDCARD */
        if( IS_VALID_WAVEFORMATEX_GUID(&dataRange->SubFormat) ||
            !memcmp((void*)&dataRange->SubFormat, (void*)&KSDATAFORMAT_SUBTYPE_PCM, sizeof ( GUID ) ) ||
            ( !memcmp((void*)&dataRange->SubFormat, (void*)&KSDATAFORMAT_SUBTYPE_WILDCARD, sizeof ( GUID ) ) &&
            ( !memcmp((void*)&dataRange->MajorFormat, (void*)&KSDATAFORMAT_TYPE_AUDIO, sizeof ( GUID ) ) ) ) )
        {
            result = paNoError;
            /* Record the maximum possible channels with this pin */
            PA_DEBUG(("MaxChannel: %d\n",pin->maxChannels));
            if( (int)((KSDATARANGE_AUDIO*)dataRange)->MaximumChannels > pin->maxChannels )
            {
                pin->maxChannels = ((KSDATARANGE_AUDIO*)dataRange)->MaximumChannels;
                /*PA_DEBUG(("MaxChannel: %d\n",pin->maxChannels));*/
            }
            /* Record the formats (bit depths) that are supported */
            if( ((KSDATARANGE_AUDIO*)dataRange)->MinimumBitsPerSample <= 16 )
            {
                pin->formats |= paInt16;
                PA_DEBUG(("Format 16 bit supported\n"));
            }
            if( ((KSDATARANGE_AUDIO*)dataRange)->MaximumBitsPerSample >= 24 )
            {
                pin->formats |= paInt24;
                PA_DEBUG(("Format 24 bit supported\n"));
            }
            if( ( pin->bestSampleRate != 48000) &&
                (((KSDATARANGE_AUDIO*)dataRange)->MaximumSampleFrequency >= 48000) &&
                (((KSDATARANGE_AUDIO*)dataRange)->MinimumSampleFrequency <= 48000) )
            {
                pin->bestSampleRate = 48000;
                PA_DEBUG(("48kHz supported\n"));
            }
            else if(( pin->bestSampleRate != 48000) && ( pin->bestSampleRate != 44100 ) &&
                (((KSDATARANGE_AUDIO*)dataRange)->MaximumSampleFrequency >= 44100) &&
                (((KSDATARANGE_AUDIO*)dataRange)->MinimumSampleFrequency <= 44100) )
            {
                pin->bestSampleRate = 44100;
                PA_DEBUG(("44.1kHz supported\n"));
            }
            else
            {
                pin->bestSampleRate = ((KSDATARANGE_AUDIO*)dataRange)->MaximumSampleFrequency;
            }
        }
        dataRange = (KSDATARANGE*)( ((char*)dataRange) + dataRange->FormatSize);
    }

    if( result != paNoError )
        goto error;

    /* Get instance information */
    result = WdmGetPinPropertySimple(
        parentFilter->handle,
        pinId,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_CINSTANCES,
        &pin->instances,
        sizeof(KSPIN_CINSTANCES));

    if( result != paNoError )
        goto error;

    /* Success */
    *error = paNoError;
    PA_DEBUG(("Pin created successfully\n"));
    PA_LOGL_;
    return pin;

error:
    /*
    Error cleanup
    */
    PaUtil_FreeMemory( item );
    if( pin )
    {
        PaUtil_FreeMemory( pin->pinConnect );
        PaUtil_FreeMemory( pin->dataRangesItem );
        PaUtil_FreeMemory( pin );
    }
    *error = result;
    PA_LOGL_;
    return NULL;
}

/*
Safely free all resources associated with the pin
*/
static void PinFree(PaWinWdmPin* pin)
{
    PA_LOGE_;
    if( pin )
    {
        PinClose(pin);
        if( pin->pinConnect )
        {
            PaUtil_FreeMemory( pin->pinConnect );
        }
        if( pin->dataRangesItem )
        {
            PaUtil_FreeMemory( pin->dataRangesItem );
        }
        PaUtil_FreeMemory( pin );
    }
    PA_LOGL_;
}

/*
If the pin handle is open, close it
*/
static void PinClose(PaWinWdmPin* pin)
{
    PA_LOGE_;
    if( pin == NULL )
    {
        PA_DEBUG(("Closing NULL pin!"));
        PA_LOGL_;
        return;
    }
    if( pin->handle != NULL )
    {
        PinSetState( pin, KSSTATE_PAUSE );
        PinSetState( pin, KSSTATE_STOP );
        CloseHandle( pin->handle );
        pin->handle = NULL;
        FilterRelease(pin->parentFilter);
    }
    PA_LOGL_;
}

/*
Set the state of this (instantiated) pin
*/
static PaError PinSetState(PaWinWdmPin* pin, KSSTATE state)
{
    PaError result = paNoError;
    KSPROPERTY prop;
    BOOL ret;
    ULONG cbBytesReturned = 0;

    PA_LOGE_;
    prop.Set = KSPROPSETID_Connection;
    prop.Id  = KSPROPERTY_CONNECTION_STATE;
    prop.Flags = KSPROPERTY_TYPE_SET;

    if( pin == NULL )
        return paInternalError;
    if( pin->handle == NULL )
        return paInternalError;

    ret = DeviceIoControl(pin->handle, IOCTL_KS_PROPERTY, &prop, sizeof(KSPROPERTY), &state, sizeof(KSSTATE), &cbBytesReturned, NULL);

    if (ret != TRUE) {
        result = paInternalError;
    }

    PA_LOGL_;
    return result;
}

static PaError PinInstantiate(PaWinWdmPin* pin)
{
    PaError result;
    unsigned long createResult;
    KSALLOCATOR_FRAMING ksaf;
    KSALLOCATOR_FRAMING_EX ksafex;

    PA_LOGE_;

    if( pin == NULL )
        return paInternalError;
    if(!pin->pinConnect)
        return paInternalError;

    FilterUse(pin->parentFilter);

    createResult = FunctionKsCreatePin(
        pin->parentFilter->handle,
        pin->pinConnect,
        GENERIC_WRITE | GENERIC_READ,
        &pin->handle
        );

    PA_DEBUG(("Pin create result = %x\n",createResult));
    if( createResult != ERROR_SUCCESS )
    {
        FilterRelease(pin->parentFilter);
        pin->handle = NULL;
        return paInvalidDevice;
    }

    result = WdmGetPropertySimple(
        pin->handle,
        &KSPROPSETID_Connection,
        KSPROPERTY_CONNECTION_ALLOCATORFRAMING,
        &ksaf,
        sizeof(ksaf),
        NULL,
        0);

    if( result != paNoError )
    {
        result = WdmGetPropertySimple(
            pin->handle,
            &KSPROPSETID_Connection,
            KSPROPERTY_CONNECTION_ALLOCATORFRAMING_EX,
            &ksafex,
            sizeof(ksafex),
            NULL,
            0);
        if( result == paNoError )
        {
            pin->frameSize = ksafex.FramingItem[0].FramingRange.Range.MinFrameSize;
        }
    }
    else
    {
        pin->frameSize = ksaf.FrameSize;
    }

    PA_LOGL_;

    return paNoError;
}

static PaError PinSetFormat(PaWinWdmPin* pin, const WAVEFORMATEX* format)
{
    unsigned long size;
    void* newConnect;

    PA_LOGE_;

    if( pin == NULL )
        return paInternalError;
    if( format == NULL )
        return paInternalError;

    size = GetWfexSize(format) + sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX) - sizeof(WAVEFORMATEX);

    if( pin->pinConnectSize != size )
    {
        newConnect = PaUtil_AllocateMemory( size );
        if( newConnect == NULL )
            return paInsufficientMemory;
        memcpy( newConnect, (void*)pin->pinConnect, min(pin->pinConnectSize,size) );
        PaUtil_FreeMemory( pin->pinConnect );
        pin->pinConnect = (KSPIN_CONNECT*)newConnect;
        pin->pinConnectSize = size;
        pin->ksDataFormatWfx = (KSDATAFORMAT_WAVEFORMATEX*)((KSPIN_CONNECT*)newConnect + 1);
        pin->ksDataFormatWfx->DataFormat.FormatSize = size - sizeof(KSPIN_CONNECT);
    }

    memcpy( (void*)&(pin->ksDataFormatWfx->WaveFormatEx), format, GetWfexSize(format) );
    pin->ksDataFormatWfx->DataFormat.SampleSize = (unsigned short)(format->nChannels * (format->wBitsPerSample / 8));

    PA_LOGL_;

    return paNoError;
}

static PaError PinIsFormatSupported(PaWinWdmPin* pin, const WAVEFORMATEX* format)
{
    KSDATARANGE_AUDIO* dataRange;
    unsigned long count;
    GUID guid = DYNAMIC_GUID( DEFINE_WAVEFORMATEX_GUID(format->wFormatTag) );
    PaError result = paInvalidDevice;

    PA_LOGE_;

    if( format->wFormatTag == WAVE_FORMAT_EXTENSIBLE )
    {
        guid = ((WAVEFORMATEXTENSIBLE*)format)->SubFormat;
    }
    dataRange = (KSDATARANGE_AUDIO*)pin->dataRanges;
    for(count = 0; count<pin->dataRangesItem->Count; count++)
    {
        if(( !memcmp(&(dataRange->DataRange.MajorFormat),&KSDATAFORMAT_TYPE_AUDIO,sizeof(GUID)) ) ||
            ( !memcmp(&(dataRange->DataRange.MajorFormat),&KSDATAFORMAT_TYPE_WILDCARD,sizeof(GUID)) ))
        {
            /* This is an audio or wildcard datarange... */
            if(( !memcmp(&(dataRange->DataRange.SubFormat),&KSDATAFORMAT_SUBTYPE_WILDCARD,sizeof(GUID)) ) ||
                ( !memcmp(&(dataRange->DataRange.SubFormat),&guid,sizeof(GUID)) ))
            {
                if(( !memcmp(&(dataRange->DataRange.Specifier),&KSDATAFORMAT_SPECIFIER_WILDCARD,sizeof(GUID)) ) ||
                    ( !memcmp(&(dataRange->DataRange.Specifier),&KSDATAFORMAT_SPECIFIER_WAVEFORMATEX,sizeof(GUID) )))
                {

                    PA_DEBUG(("Pin:%x, DataRange:%d\n",(void*)pin,count));
                    PA_DEBUG(("\tFormatSize:%d, SampleSize:%d\n",dataRange->DataRange.FormatSize,dataRange->DataRange.SampleSize));
                    PA_DEBUG(("\tMaxChannels:%d\n",dataRange->MaximumChannels));
                    PA_DEBUG(("\tBits:%d-%d\n",dataRange->MinimumBitsPerSample,dataRange->MaximumBitsPerSample));
                    PA_DEBUG(("\tSampleRate:%d-%d\n",dataRange->MinimumSampleFrequency,dataRange->MaximumSampleFrequency));

                    if( dataRange->MaximumChannels < format->nChannels )
                    {
                        result = paInvalidChannelCount;
                        continue;
                    }
                    if( dataRange->MinimumBitsPerSample > format->wBitsPerSample )
                    {
                        result = paSampleFormatNotSupported;
                        continue;
                    }
                    if( dataRange->MaximumBitsPerSample < format->wBitsPerSample )
                    {
                        result = paSampleFormatNotSupported;
                        continue;
                    }
                    if( dataRange->MinimumSampleFrequency > format->nSamplesPerSec )
                    {
                        result = paInvalidSampleRate;
                        continue;
                    }
                    if( dataRange->MaximumSampleFrequency < format->nSamplesPerSec )
                    {
                        result = paInvalidSampleRate;
                        continue;
                    }
                    /* Success! */
                    PA_LOGL_;
                    return paNoError;
                }
            }
        }
        dataRange = (KSDATARANGE_AUDIO*)( ((char*)dataRange) + dataRange->DataRange.FormatSize);
    }

    PA_LOGL_;

    return result;
}

static PaError PinGetBufferWithNotification(PaWinWdmPin* pPin, void** pBuffer, DWORD* pRequestedBufSize, BOOL* pbCallMemBarrier)
{
    PaError result = paNoError;
    KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION propIn;
    KSRTAUDIO_BUFFER propOut;
    ULONG cbBytesReturned = 0;
    BOOL res;

    propIn.BaseAddress = NULL;
    propIn.NotificationCount = 2;
    propIn.RequestedBufferSize = *pRequestedBufSize;
    propIn.Property.Set = KSPROPSETID_RtAudio;
    propIn.Property.Id = KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION;
    propIn.Property.Flags = KSPROPERTY_TYPE_GET;

    res = DeviceIoControl(pPin->handle, IOCTL_KS_PROPERTY,
        &propIn,
        sizeof(KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION),
        &propOut,
        sizeof(KSRTAUDIO_BUFFER),
        &cbBytesReturned,
        NULL);

    if (res) 
    {
        *pBuffer = propOut.BufferAddress;
        *pRequestedBufSize = propOut.ActualBufferSize;
        *pbCallMemBarrier = propOut.CallMemoryBarrier;
    }
    else 
    {
        PA_DEBUG(("Failed to get buffer with notification\n"));
        result = paUnanticipatedHostError;
    }

    return result;
}


static PaError PinRegisterPositionRegister(PaWinWdmPin* pPin) 
{
    PaError result = paNoError;
    KSRTAUDIO_HWREGISTER_PROPERTY propIn;
    KSRTAUDIO_HWREGISTER propOut;
    ULONG cbBytesReturned = 0;
    BOOL res;

    PA_LOGE_;

    propIn.BaseAddress = NULL;
    propIn.Property.Set = KSPROPSETID_RtAudio;
    propIn.Property.Id = KSPROPERTY_RTAUDIO_POSITIONREGISTER;
    propIn.Property.Flags = KSPROPERTY_TYPE_GET;

    res = DeviceIoControl(pPin->handle, IOCTL_KS_PROPERTY,
        &propIn,
        sizeof(KSRTAUDIO_HWREGISTER_PROPERTY),
        &propOut,
        sizeof(KSRTAUDIO_HWREGISTER),
        &cbBytesReturned,
        NULL);

    if (res) 
    {
        pPin->positionRegister = (ULONG*)propOut.Register;
    }
    else
    {
        PA_DEBUG(("Failed to register position register\n"));
        result = paUnanticipatedHostError;
    }

    PA_LOGL_;

    return result;
}

static PaError PinRegisterNotificationHandle(PaWinWdmPin* pPin, HANDLE handle) 
{
    PaError result = paNoError;
    KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY prop;
    ULONG cbBytesReturned = 0;
    BOOL res;

    PA_LOGE_;

    prop.NotificationEvent = handle;
    prop.Property.Set = KSPROPSETID_RtAudio;
    prop.Property.Id = KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT;
    prop.Property.Flags = KSPROPERTY_TYPE_GET;

    res = DeviceIoControl(pPin->handle,
        IOCTL_KS_PROPERTY,
        &prop,
        sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY),
        &prop,
        sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY),
        &cbBytesReturned,
        NULL);

    if (!res) {
        PA_DEBUG(("Failed to register notification handle 0x%08X\n", handle));
        result = paUnanticipatedHostError;
    }

    PA_LOGL_;

    return result;
}

static PaError PinUnregisterNotificationHandle(PaWinWdmPin* pPin, HANDLE handle) 
{
    PaError result = paNoError;
    KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY prop;
    ULONG cbBytesReturned = 0;
    BOOL res;

    PA_LOGE_;

    if (handle != NULL)
    {

        prop.NotificationEvent = handle;
        prop.Property.Set = KSPROPSETID_RtAudio;
        prop.Property.Id = KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT;
        prop.Property.Flags = KSPROPERTY_TYPE_GET;

        res = DeviceIoControl(pPin->handle,
            IOCTL_KS_PROPERTY,
            &prop,
            sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY),
            &prop,
            sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY),
            &cbBytesReturned,
            NULL);

        if (!res) {
            PA_DEBUG(("Failed to unregister notification handle 0x%08X\n", handle));
            result = paUnanticipatedHostError;
        }

        PA_LOGL_;

    }

    return result;
}

static PaError PinGetHwLatency(PaWinWdmPin* pPin, ULONG* pFifoSize, ULONG* pChipsetDelay, ULONG* pCodecDelay)
{
    PaError result = paNoError;
    KSPROPERTY propIn;
    KSRTAUDIO_HWLATENCY propOut;
    ULONG cbBytesReturned = 0;
    HRESULT hr;

    PA_LOGE_;

    propIn.Set = KSPROPSETID_RtAudio;
    propIn.Id = KSPROPERTY_RTAUDIO_HWLATENCY;
    propIn.Flags = KSPROPERTY_TYPE_GET;

    hr = WdmSyncIoctl(pPin->handle, IOCTL_KS_PROPERTY,
        &propIn,
        sizeof(KSPROPERTY),
        &propOut,
        sizeof(KSRTAUDIO_HWLATENCY),
        &cbBytesReturned);

    if (SUCCEEDED(hr)) {
        *pFifoSize = propOut.FifoSize;
        *pChipsetDelay = propOut.ChipsetDelay;
        *pCodecDelay = propOut.CodecDelay;
    }
    else {
        PA_DEBUG(("Failed to retrieve hardware FIFO size!\n"));
        result = paUnanticipatedHostError;
    }

    PA_LOGL_;

    return result;
}

/* This one is used for WaveRT */
static PaError PinGetAudioPositionDirect(PaWinWdmPin* pPin, ULONG* pPosition)
{
    *pPosition = (*pPin->positionRegister);
    return paNoError;
}

/* This one also, but in case the driver hasn't implemented memory mapped access to the position register */
static PaError PinGetAudioPositionViaIOCTL(PaWinWdmPin* pPin, ULONG* pPosition)
{
    PaError result = paNoError;
    KSPROPERTY propIn;
    KSAUDIO_POSITION propOut;
    ULONG cbBytesReturned = 0;
    BOOL fRes;

    PA_LOGE_;

    propIn.Set = KSPROPSETID_Audio;
    propIn.Id = KSPROPERTY_AUDIO_POSITION;
    propIn.Flags = KSPROPERTY_TYPE_GET;

    fRes = DeviceIoControl(pPin->handle,
        IOCTL_KS_PROPERTY,
        &propIn, sizeof(KSPROPERTY),
        &propOut, sizeof(KSAUDIO_POSITION),
        &cbBytesReturned,
        NULL);

    if (fRes) {
        *pPosition = (ULONG)(propOut.PlayOffset);
    }
    else {
        PA_DEBUG(("Failed to get audio position!\n"));
        result = paUnanticipatedHostError;
    }

    PA_LOGL_;

    return result;

}

/***********************************************************************************************/

/**
* Create a new filter object
*/
static PaWinWdmFilter* FilterNew(BOOL fRealtime, DWORD devNode, TCHAR* filterName, TCHAR* friendlyName, PaError* error)
{
    PaWinWdmFilter* filter;
    PaError result;
    int pinId;
    int valid;


    /* Allocate the new filter object */
    filter = (PaWinWdmFilter*)PaUtil_AllocateMemory( sizeof(PaWinWdmFilter) );
    if( !filter )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Set flag for WaveRT */
    filter->isWaveRT = fRealtime;

    /* Store device node */
    filter->deviceNode = devNode;

    /* Zero the filter object - done by AllocateMemory */
    /* memset( (void*)filter, 0, sizeof(PaWinWdmFilter) ); */

    /* Copy the filter name */
    _tcsncpy(filter->filterName, filterName, MAX_PATH);

    /* Copy the friendly name */
    _tcsncpy(filter->friendlyName, friendlyName, MAX_PATH);

    /* Open the filter handle */
    result = FilterUse(filter);
    if( result != paNoError )
    {
        goto error;
    }

    /* Get pin count */
    result = WdmGetPinPropertySimple
        (
        filter->handle,
        0,
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_CTYPES,
        &filter->pinCount,
        sizeof(filter->pinCount)
        );

    if( result != paNoError)
    {
        goto error;
    }

    /* Allocate pointer array to hold the pins */
    filter->pins = (PaWinWdmPin**)PaUtil_AllocateMemory( sizeof(PaWinWdmPin*) * filter->pinCount );
    if( !filter->pins )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* Create all the pins we can */
    filter->maxInputChannels = 0;
    filter->maxOutputChannels = 0;
    filter->bestSampleRate = 0;

    valid = 0;
    for(pinId = 0; pinId < filter->pinCount; pinId++)
    {
        /* Create the pin with this Id */
        PaWinWdmPin* newPin;
        newPin = PinNew(filter, pinId, &result);
        if( result == paInsufficientMemory )
            goto error;
        if( newPin != NULL )
        {
            filter->pins[pinId] = newPin;
            valid = 1;

            /* Get the max output channel count */
            if(( newPin->dataFlow == KSPIN_DATAFLOW_IN ) &&
                (( newPin->communication == KSPIN_COMMUNICATION_SINK) ||
                ( newPin->communication == KSPIN_COMMUNICATION_BOTH)))
            {
                if(newPin->maxChannels > filter->maxOutputChannels)
                    filter->maxOutputChannels = newPin->maxChannels;
                filter->formats |= newPin->formats;
            }
            /* Get the max input channel count */
            if(( newPin->dataFlow == KSPIN_DATAFLOW_OUT ) &&
                (( newPin->communication == KSPIN_COMMUNICATION_SINK) ||
                ( newPin->communication == KSPIN_COMMUNICATION_BOTH)))
            {
                if(newPin->maxChannels > filter->maxInputChannels)
                    filter->maxInputChannels = newPin->maxChannels;
                filter->formats |= newPin->formats;
            }

            if(newPin->bestSampleRate > filter->bestSampleRate)
            {
                filter->bestSampleRate = newPin->bestSampleRate;
            }
        }
    }

    if(( filter->maxInputChannels == 0) && ( filter->maxOutputChannels == 0))
    {
        /* No input or output... not valid */
        valid = 0;
    }

    if( !valid )
    {
        /* No valid pin was found on this filter so we destroy it */
        result = paDeviceUnavailable;
        goto error;
    }

    /* Close the filter handle for now
    * It will be opened later when needed */
    FilterRelease(filter);

    *error = paNoError;
    return filter;

error:
    /*
    Error cleanup
    */
    if( filter )
    {
        for( pinId = 0; pinId < filter->pinCount; pinId++ )
            PinFree(filter->pins[pinId]);
        PaUtil_FreeMemory( filter->pins );
        if( filter->handle )
            CloseHandle( filter->handle );
        PaUtil_FreeMemory( filter );
    }
    *error = result;
    return NULL;
}

/**
* Free a previously created filter
*/
static void FilterFree(PaWinWdmFilter* filter)
{
    int pinId;
    PA_LOGL_;
    if( filter )
    {
        for( pinId = 0; pinId < filter->pinCount; pinId++ )
            PinFree(filter->pins[pinId]);
        PaUtil_FreeMemory( filter->pins );
        if( filter->handle )
            CloseHandle( filter->handle );
        PaUtil_FreeMemory( filter );
    }
    PA_LOGE_;
}

/**
* Reopen the filter handle if necessary so it can be used
**/
static PaError FilterUse(PaWinWdmFilter* filter)
{
    assert( filter );

    PA_LOGE_;
    if( filter->handle == NULL )
    {
        /* Open the filter */
        filter->handle = CreateFile(
            filter->filterName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            NULL);

        if( filter->handle == NULL )
        {
            return paDeviceUnavailable;
        }
    }
    filter->usageCount++;
    PA_LOGL_;
    return paNoError;
}

/**
* Release the filter handle if nobody is using it
**/
static void FilterRelease(PaWinWdmFilter* filter)
{
    assert( filter );
    assert( filter->usageCount > 0 );

    PA_LOGE_;
    filter->usageCount--;
    if( filter->usageCount == 0 )
    {
        if( filter->handle != NULL )
        {
            CloseHandle( filter->handle );
            filter->handle = NULL;
        }
    }
    PA_LOGL_;
}

/**
* Create a render (playback) Pin using the supplied format
**/
static PaWinWdmPin* FilterCreateRenderPin(PaWinWdmFilter* filter,
                                          const WAVEFORMATEX* wfex,
                                          PaError* error)
{
    PaError result;
    PaWinWdmPin* pin;

    assert( filter );

    pin = FilterFindViableRenderPin(filter,wfex,&result);
    if(!pin)
    {
        goto error;
    }
    result = PinSetFormat(pin,wfex);
    if( result != paNoError )
    {
        goto error;
    }
    result = PinInstantiate(pin);
    if( result != paNoError )
    {
        goto error;
    }

    *error = paNoError;
    return pin;

error:
    *error = result;
    return NULL;
}

/**
* Find a pin that supports the given format
**/
static PaWinWdmPin* FilterFindViableRenderPin(PaWinWdmFilter* filter,
                                              const WAVEFORMATEX* wfex,
                                              PaError* error)
{
    int pinId;
    PaWinWdmPin*  pin;
    PaError result = paDeviceUnavailable;
    *error = paNoError;

    assert( filter );

    for( pinId = 0; pinId<filter->pinCount; pinId++ )
    {
        pin = filter->pins[pinId];
        if( pin != NULL )
        {
            if(( pin->dataFlow == KSPIN_DATAFLOW_IN ) &&
                (( pin->communication == KSPIN_COMMUNICATION_SINK) ||
                ( pin->communication == KSPIN_COMMUNICATION_BOTH)))
            {
                result = PinIsFormatSupported( pin, wfex );
                if( result == paNoError )
                {
                    return pin;
                }
            }
        }
    }

    *error = result;
    return NULL;
}

/**
* Check if there is a pin that should playback
* with the supplied format
**/
static PaError FilterCanCreateRenderPin(PaWinWdmFilter* filter,
                                        const WAVEFORMATEX* wfex)
{
    PaWinWdmPin* pin;
    PaError result;

    assert ( filter );

    pin = FilterFindViableRenderPin(filter,wfex,&result);
    /* result will be paNoError if pin found
    * or else an error code indicating what is wrong with the format
    **/
    return result;
}

/**
* Create a capture (record) Pin using the supplied format
**/
static PaWinWdmPin* FilterCreateCapturePin(PaWinWdmFilter* filter,
                                           const WAVEFORMATEX* wfex,
                                           PaError* error)
{
    PaError result;
    PaWinWdmPin* pin;

    assert( filter );

    pin = FilterFindViableCapturePin(filter,wfex,&result);
    if(!pin)
    {
        goto error;
    }

    result = PinSetFormat(pin,wfex);
    if( result != paNoError )
    {
        goto error;
    }

    result = PinInstantiate(pin);
    if( result != paNoError )
    {
        goto error;
    }

    *error = paNoError;
    return pin;

error:
    *error = result;
    return NULL;
}

/**
* Find a capture pin that supports the given format
**/
static PaWinWdmPin* FilterFindViableCapturePin(PaWinWdmFilter* filter,
                                               const WAVEFORMATEX* wfex,
                                               PaError* error)
{
    int pinId;
    PaWinWdmPin*  pin;
    PaError result = paDeviceUnavailable;
    *error = paNoError;

    assert( filter );

    for( pinId = 0; pinId<filter->pinCount; pinId++ )
    {
        pin = filter->pins[pinId];
        if( pin != NULL )
        {
            if(( pin->dataFlow == KSPIN_DATAFLOW_OUT ) &&
                (( pin->communication == KSPIN_COMMUNICATION_SINK) ||
                ( pin->communication == KSPIN_COMMUNICATION_BOTH)))
            {
                result = PinIsFormatSupported( pin, wfex );
                if( result == paNoError )
                {
                    return pin;
                }
            }
        }
    }

    *error = result;
    return NULL;
}

/**
* Check if there is a pin that should playback
* with the supplied format
**/
static PaError FilterCanCreateCapturePin(PaWinWdmFilter* filter,
                                         const WAVEFORMATEX* wfex)
{
    PaWinWdmPin* pin;
    PaError result;

    assert ( filter );

    pin = FilterFindViableCapturePin(filter,wfex,&result);
    /* result will be paNoError if pin found
    * or else an error code indicating what is wrong with the format
    **/
    return result;
}

static BOOL IsUSBDevice(const TCHAR* devicePath)
{
#ifdef UNICODE
    return (_wcsnicmp(devicePath, L"\\\\?\\USB", 5) == 0);
#else

    return (_strnicmp(devicePath, "\\\\?\\USB", 5) == 0);
#endif
}

typedef enum _tag_EAlias
{
    Alias_kRender   = (1<<0),
    Alias_kCapture  = (1<<1),
    Alias_kRealtime = (1<<2),
} EAlias;

/**
* Build the list of available filters
* Use the SetupDi API to enumerate all devices in the KSCATEGORY_AUDIO which 
* have a KSCATEGORY_RENDER or KSCATEGORY_CAPTURE alias. For each of these 
* devices initialise a PaWinWdmFilter structure by calling our NewFilter() 
* function. We enumerate devices twice, once to count how many there are, 
* and once to initialize the PaWinWdmFilter structures.
*
* Vista and later: Also check KSCATEGORY_REALTIME for WaveRT devices.
*/
static PaError BuildFilterList(PaWinWdmHostApiRepresentation* wdmHostApi)
{
    PaError result = paNoError;
    HDEVINFO handle = NULL;
    int device;
    int invalidDevices;
    int slot;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    SP_DEVICE_INTERFACE_DATA aliasData;
    SP_DEVINFO_DATA devInfoData;
    int noError;
    const int sizeInterface = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA) + (MAX_PATH * sizeof(WCHAR));
    unsigned char interfaceDetailsArray[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA) + (MAX_PATH * sizeof(WCHAR))];
    SP_DEVICE_INTERFACE_DETAIL_DATA* devInterfaceDetails = (SP_DEVICE_INTERFACE_DETAIL_DATA*)interfaceDetailsArray;
    TCHAR friendlyName[MAX_PATH];
    HKEY hkey;
    DWORD sizeFriendlyName;
    DWORD type;
    PaWinWdmFilter* newFilter;
    GUID* category = (GUID*)&KSCATEGORY_AUDIO;
    GUID* alias_render = (GUID*)&KSCATEGORY_RENDER;
    GUID* alias_capture = (GUID*)&KSCATEGORY_CAPTURE;
    GUID* category_realtime = (GUID*)&KSCATEGORY_REALTIME;
    DWORD hasAlias;

    PA_LOGE_;

    devInterfaceDetails->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    /* Open a handle to search for devices (filters) */
    handle = SetupDiGetClassDevs(category,NULL,NULL,DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if( handle == NULL )
    {
        return paUnanticipatedHostError;
    }
    PA_DEBUG(("Setup called\n"));

    /* First let's count the number of devices so we can allocate a list */
    invalidDevices = 0;
    for( device = 0;;device++ )
    {
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        interfaceData.Reserved = 0;
        aliasData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        aliasData.Reserved = 0;
        noError = SetupDiEnumDeviceInterfaces(handle,NULL,category,device,&interfaceData);
        PA_DEBUG(("Enum called\n"));
        if( !noError )
            break; /* No more devices */

        /* Check this one has the render or capture alias */
        hasAlias = 0;
        noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,alias_render,&aliasData);
        PA_DEBUG(("noError = %d\n",noError));
        if(noError)
        {
            if(aliasData.Flags && (!(aliasData.Flags & SPINT_REMOVED)))
            {
                PA_DEBUG(("Device %d has render alias\n",device));
                hasAlias |= Alias_kRender; /* Has render alias */
            }
            else
            {
                PA_DEBUG(("Device %d has no render alias\n",device));
            }
        }
        noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,alias_capture,&aliasData);
        if(noError)
        {
            if(aliasData.Flags && (!(aliasData.Flags & SPINT_REMOVED)))
            {
                PA_DEBUG(("Device %d has capture alias\n",device));
                hasAlias |= Alias_kCapture; /* Has capture alias */
            }
            else
            {
                PA_DEBUG(("Device %d has no capture alias\n",device));
            }
        }
        if(!hasAlias)
            invalidDevices++; /* This was not a valid capture or render audio device */
    }
    /* Remember how many there are */
    wdmHostApi->filterCount = device-invalidDevices;

    PA_DEBUG(("Interfaces found: %d\n",device-invalidDevices));

    /* Now allocate the list of pointers to devices */
    wdmHostApi->filters  = (PaWinWdmFilter**)PaUtil_AllocateMemory( sizeof(PaWinWdmFilter*) * device );
    if( !wdmHostApi->filters )
    {
        if(handle != NULL)
            SetupDiDestroyDeviceInfoList(handle);
        return paInsufficientMemory;
    }

    /* Now create filter objects for each interface found */
    slot = 0;
    for( device = 0;;device++ )
    {
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        interfaceData.Reserved = 0;
        aliasData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        aliasData.Reserved = 0;
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        devInfoData.Reserved = 0;

        noError = SetupDiEnumDeviceInterfaces(handle,NULL,category,device,&interfaceData);
        if( !noError )
            break; /* No more devices */

        /* Check this one has the render or capture alias */
        hasAlias = 0;
        noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,alias_render,&aliasData);
        if(noError)
        {
            if(aliasData.Flags && (!(aliasData.Flags & SPINT_REMOVED)))
            {
                PA_DEBUG(("Device %d has render alias\n",device));
                hasAlias |= Alias_kRender; /* Has render alias */
            }
        }
        noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,alias_capture,&aliasData);
        if(noError)
        {
            if(aliasData.Flags && (!(aliasData.Flags & SPINT_REMOVED)))
            {
                PA_DEBUG(("Device %d has capture alias\n",device));
                hasAlias |= Alias_kCapture; /* Has capture alias */
            }
        }
        if(!hasAlias)
        {
            continue; /* This was not a valid capture or render audio device */
        }
        else
        {
            /* Check if filter is WaveRT, if not it is a WaveCyclic */
            noError = SetupDiGetDeviceInterfaceAlias(handle,&interfaceData,category_realtime,&aliasData);
            if (noError)
            {
                PA_DEBUG(("Device %d has realtime alias\n",device));
                hasAlias |= Alias_kRealtime;
            }
        }

        noError = SetupDiGetDeviceInterfaceDetail(handle,&interfaceData,devInterfaceDetails,sizeInterface,NULL,&devInfoData);
        if( noError )
        {
            OSVERSIONINFO osvi;
            BOOL isEarlierThanVista;
            osvi.dwOSVersionInfoSize = sizeof(osvi);
            isEarlierThanVista = (GetVersionEx(&osvi) && osvi.dwMajorVersion<6);

            /* Try to get the "friendly name" for this interface */
            sizeFriendlyName = sizeof(friendlyName);
            friendlyName[0] = 0;

            if (isEarlierThanVista && IsUSBDevice(devInterfaceDetails->DevicePath))
            {
                /* XP and USB audio device needs to look elsewhere, otherwise it'll only be a "USB Audio Device". Not
                   very literate. */
                if (!SetupDiGetDeviceRegistryProperty(handle,
                    &devInfoData,
                    SPDRP_LOCATION_INFORMATION, 
                    &type,
                    (BYTE*)friendlyName,
                    sizeof(friendlyName),
                    NULL))
                {
                    friendlyName[0] = 0;
                }
            }

            if (friendlyName[0] == 0)
            {
                /* Fix contributed by Ben Allison
                * Removed KEY_SET_VALUE from flags on following call
                * as its causes failure when running without admin rights
                * and it was not required */
                hkey=SetupDiOpenDeviceInterfaceRegKey(handle,&interfaceData,0,KEY_QUERY_VALUE);
                if(hkey!=INVALID_HANDLE_VALUE)
                {
                    noError = RegQueryValueEx(hkey,TEXT("FriendlyName"),0,&type,(BYTE*)friendlyName,&sizeFriendlyName);
                    if( noError == ERROR_SUCCESS )
                    {
                        PA_DEBUG(("Interface %d, Name: %s\n",device,friendlyName));
                        RegCloseKey(hkey);
                    }
                    else
                    {
                        friendlyName[0] = 0;
                    }
                }
            }
            newFilter = FilterNew(!!(hasAlias & Alias_kRealtime), devInfoData.DevInst, devInterfaceDetails->DevicePath, friendlyName, &result);
            if( result == paNoError )
            {
                PA_DEBUG(("Filter created %s\n", (newFilter->isWaveRT?"(WaveRT)":"(WaveCyclic)")));

                wdmHostApi->filters[slot] = newFilter;

                slot++;
            }
            else
            {
                PA_DEBUG(("Filter NOT created\n"));
                /* As there are now less filters than we initially thought
                * we must reduce the count by one */
                wdmHostApi->filterCount--;
            }
        }
    }

    /* Clean up */
    if(handle != NULL)
        SetupDiDestroyDeviceInfoList(handle);

    return paNoError;
}

PaError PaWinWdm_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i, deviceCount;
    PaWinWdmHostApiRepresentation *wdmHostApi;
    PaWinWdmDeviceInfo *deviceInfoArray;
    PaWinWdmFilter* pFilter;
    PaWinWdmDeviceInfo *wdmDeviceInfo;
    PaDeviceInfo *deviceInfo;

    PA_LOGE_;

#ifdef PA_WDMKS_SET_TREF
    tRef = PaUtil_GetTime();
#endif

    /*
    Attempt to load the KSUSER.DLL without which we cannot create pins
    We will unload this on termination
    */
    if(DllKsUser == NULL)
    {
        DllKsUser = LoadLibrary(TEXT("ksuser.dll"));
        if(DllKsUser == NULL)
            goto error;
    }

    /* Attempt to load AVRT.DLL, if we can't, then we'll just use time critical prio instead... */
    if(DllAvRt == NULL)
    {
        DllAvRt = LoadLibrary(TEXT("avrt.dll"));
        if (DllAvRt != NULL)
        {
            FunctionAvSetMmThreadCharacteristics = (AVSETMMTHREADCHARACTERISTICS*)GetProcAddress(DllAvRt,"AvSetMmThreadCharacteristicsA");
            FunctionAvRevertMmThreadCharacteristics = (AVREVERTMMTHREADCHARACTERISTICS*)GetProcAddress(DllAvRt, "AvRevertMmThreadCharacteristics");
            FunctionAvSetMmThreadPriority = (AVSETMMTHREADPRIORITY*)GetProcAddress(DllAvRt, "AvSetMmThreadPriority");
        }
    }

    FunctionKsCreatePin = (KSCREATEPIN*)GetProcAddress(DllKsUser, "KsCreatePin");
    if(FunctionKsCreatePin == NULL)
        goto error;

    wdmHostApi = (PaWinWdmHostApiRepresentation*)PaUtil_AllocateMemory( sizeof(PaWinWdmHostApiRepresentation) );
    if( !wdmHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    wdmHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !wdmHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    result = BuildFilterList( wdmHostApi );
    if( result != paNoError )
    {
        goto error;
    }
    deviceCount = wdmHostApi->filterCount;

    *hostApi = &wdmHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paWDMKS;
    (*hostApi)->info.name = "Windows WDM-KS";
    (*hostApi)->info.defaultInputDevice = paNoDevice;
    (*hostApi)->info.defaultOutputDevice = paNoDevice;

    if( deviceCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
            wdmHostApi->allocations, sizeof(PaWinWdmDeviceInfo*) * deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaWinWdmDeviceInfo*)PaUtil_GroupAllocateMemory(
            wdmHostApi->allocations, sizeof(PaWinWdmDeviceInfo) * deviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i=0; i < deviceCount; ++i )
        {
            wdmDeviceInfo = &deviceInfoArray[i];
            deviceInfo = &wdmDeviceInfo->inheritedDeviceInfo;
            pFilter = wdmHostApi->filters[i];
            if( pFilter == NULL )
                continue;
            wdmDeviceInfo->filter = pFilter;
            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = hostApiIndex;
            deviceInfo->name = (char*)pFilter->friendlyName;
            PA_DEBUG(("Device found name: %s (%s)\n",(char*)pFilter->friendlyName, pFilter->isWaveRT?"WaveRT":"WaveCyclic"));
            deviceInfo->maxInputChannels = pFilter->maxInputChannels;
            if(deviceInfo->maxInputChannels > 0)
            {
                /* Set the default input device to the first device we find with
                * more than zero input channels
                **/
                if((*hostApi)->info.defaultInputDevice == paNoDevice)
                {
                    (*hostApi)->info.defaultInputDevice = i;
                }
            }

            deviceInfo->maxOutputChannels = pFilter->maxOutputChannels;
            if(deviceInfo->maxOutputChannels > 0)
            {
                /* Set the default output device to the first device we find with
                * more than zero output channels
                **/
                if((*hostApi)->info.defaultOutputDevice == paNoDevice)
                {
                    (*hostApi)->info.defaultOutputDevice = i;
                }
            }

            /* These low values are not very useful because
            * a) The lowest latency we end up with can depend on many factors such
            *    as the device buffer sizes/granularities, sample rate, channels and format
            * b) We cannot know the device buffer sizes until we try to open/use it at
            *    a particular setting
            * So: we give 512x48000Hz frames as the default low input latency
            **/
            if (pFilter->isWaveRT) {
                deviceInfo->defaultLowInputLatency = 0.003;
                deviceInfo->defaultLowOutputLatency = 0.003;
                deviceInfo->defaultHighInputLatency = (512/48000.0);
                deviceInfo->defaultHighOutputLatency = (512/48000.0);
                deviceInfo->defaultSampleRate = (double)(pFilter->bestSampleRate);
            }
            else {
                deviceInfo->defaultLowInputLatency = 0.01;
                deviceInfo->defaultLowOutputLatency = 0.01;
                deviceInfo->defaultHighInputLatency = (4096.0/48000.0);
                deviceInfo->defaultHighOutputLatency = (4096.0/48000.0);
                deviceInfo->defaultSampleRate = (double)(pFilter->bestSampleRate);
            }

            (*hostApi)->deviceInfos[i] = deviceInfo;
        }
    }

    (*hostApi)->info.deviceCount = deviceCount;

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &wdmHostApi->callbackStreamInterface, CloseStream, StartStream,
        StopStream, AbortStream, IsStreamStopped, IsStreamActive,
        GetStreamTime, GetStreamCpuLoad,
        PaUtil_DummyRead, PaUtil_DummyWrite,
        PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable);
        /*GetStreamInfo);*/

    PaUtil_InitializeStreamInterface( &wdmHostApi->blockingStreamInterface, CloseStream, StartStream,
        StopStream, AbortStream, IsStreamStopped, IsStreamActive,
        GetStreamTime, PaUtil_DummyGetCpuLoad,
        ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable);
        /*GetStreamInfo);*/

    PA_LOGL_;
    return result;

error:
    Terminate( (PaUtilHostApiRepresentation*)wdmHostApi );

    PA_LOGL_;
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaWinWdmHostApiRepresentation *wdmHostApi = (PaWinWdmHostApiRepresentation*)hostApi;
    int i;
    PA_LOGE_;

    if( DllKsUser != NULL )
    {
        FreeLibrary( DllKsUser );
        DllKsUser = NULL;
    }

    if( DllAvRt != NULL )
    {
        FreeLibrary( DllAvRt );
        DllAvRt = NULL;
    }

    if( wdmHostApi)
    {
        if (wdmHostApi->filters)
        {
            for( i=0; i<wdmHostApi->filterCount; i++)
            {
                if( wdmHostApi->filters[i] != NULL )
                {
                    FilterFree( wdmHostApi->filters[i] );
                    wdmHostApi->filters[i] = NULL;
                }
            }
            PaUtil_FreeMemory( wdmHostApi->filters );
        }
        if( wdmHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( wdmHostApi->allocations );
            PaUtil_DestroyAllocationGroup( wdmHostApi->allocations );
        }
        PaUtil_FreeMemory( wdmHostApi );
    }
    PA_LOGL_;
}

static void FillWFEXT( WAVEFORMATEXTENSIBLE* pwfext, PaSampleFormat sampleFormat, double sampleRate, int channelCount)
{
    PA_LOGE_;
    PA_DEBUG(( "  sampleRate = %.1lf\n" , sampleRate ));
    PA_DEBUG(( "sampleFormat = %lx\n" , sampleFormat ));
    PA_DEBUG(( "channelCount = %d\n", channelCount ));

    pwfext->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    pwfext->Format.nChannels = channelCount;
    pwfext->Format.nSamplesPerSec = (int)sampleRate;
    if(channelCount == 1)
        pwfext->dwChannelMask = KSAUDIO_SPEAKER_DIRECTOUT;
    else
        pwfext->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    if(sampleFormat == paFloat32)
    {
        pwfext->Format.nBlockAlign = channelCount * 4;
        pwfext->Format.wBitsPerSample = 32;
        pwfext->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
        pwfext->Samples.wValidBitsPerSample = 32;
        pwfext->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    else if(sampleFormat == paInt32)
    {
        pwfext->Format.nBlockAlign = channelCount * 4;
        pwfext->Format.wBitsPerSample = 32;
        pwfext->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
        pwfext->Samples.wValidBitsPerSample = 32;
        pwfext->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    else if(sampleFormat == paInt24)
    {
        pwfext->Format.nBlockAlign = channelCount * 3;
        pwfext->Format.wBitsPerSample = 24;
        pwfext->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
        pwfext->Samples.wValidBitsPerSample = 24;
        pwfext->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    else if(sampleFormat == paInt16)
    {
        pwfext->Format.nBlockAlign = channelCount * 2;
        pwfext->Format.wBitsPerSample = 16;
        pwfext->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
        pwfext->Samples.wValidBitsPerSample = 16;
        pwfext->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    pwfext->Format.nAvgBytesPerSec = pwfext->Format.nSamplesPerSec * pwfext->Format.nBlockAlign;

    PA_LOGL_;
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                 const PaStreamParameters *inputParameters,
                                 const PaStreamParameters *outputParameters,
                                 double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaWinWdmHostApiRepresentation *wdmHostApi = (PaWinWdmHostApiRepresentation*)hostApi;
    PaWinWdmFilter* pFilter;
    int result = paFormatIsSupported;
    WAVEFORMATEXTENSIBLE wfx;

    PA_LOGE_;

    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
        this implementation doesn't support any custom sample formats */
        if( inputSampleFormat & paCustomFormat )
        {
            PaWindWDM_SetLastErrorInfo(paSampleFormatNotSupported, "Custom input format not supported");
            return paSampleFormatNotSupported;
        }

        /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            PaWindWDM_SetLastErrorInfo(paInvalidDevice, "paUseHostApiSpecificDeviceSpecification not supported");
            return paInvalidDevice;
        }

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
        {
            PaWindWDM_SetLastErrorInfo(paInvalidChannelCount, "Invalid input channel count");
            return paInvalidChannelCount;
        }

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
        {
            PaWindWDM_SetLastErrorInfo(paIncompatibleHostApiSpecificStreamInfo, "Host API stream info not supported");
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        }

        /* Check that the input format is supported */
        FillWFEXT(&wfx,paInt16,sampleRate,inputChannelCount);

        pFilter = wdmHostApi->filters[inputParameters->device];
        result = FilterCanCreateCapturePin(pFilter,(const WAVEFORMATEX*)&wfx);
        if( result != paNoError )
        {
            /* Try a WAVE_FORMAT_PCM instead */
            wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
            wfx.Format.cbSize = 0;
            wfx.Samples.wValidBitsPerSample = 0;
            wfx.dwChannelMask = 0;
            wfx.SubFormat = GUID_NULL;
            result = FilterCanCreateCapturePin(pFilter,(const WAVEFORMATEX*)&wfx);
            if( result != paNoError )
            {
                PaWindWDM_SetLastErrorInfo(result, "FilterCanCreateCapturePin failed: sr=%u,ch=%u,bits=%u", wfx.Format.nSamplesPerSec, wfx.Format.nChannels, wfx.Format.wBitsPerSample);
                return result;
            }
        }
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
        {
            PaWindWDM_SetLastErrorInfo(paSampleFormatNotSupported, "Custom output format not supported");
            return paSampleFormatNotSupported;
        }

        /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            PaWindWDM_SetLastErrorInfo(paInvalidDevice, "paUseHostApiSpecificDeviceSpecification not supported");
            return paInvalidDevice;
        }

        /* check that output device can support outputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
        {
            PaWindWDM_SetLastErrorInfo(paInvalidChannelCount, "Invalid output channel count");
            return paInvalidChannelCount;
        }

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
        {
            PaWindWDM_SetLastErrorInfo(paIncompatibleHostApiSpecificStreamInfo, "Host API stream info not supported");
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        }

        /* Check that the output format is supported */
        FillWFEXT(&wfx,paInt16,sampleRate,outputChannelCount);

        pFilter = wdmHostApi->filters[outputParameters->device];
        result = FilterCanCreateRenderPin(pFilter,(const WAVEFORMATEX*)&wfx);
        if( result != paNoError )
        {
            /* Try a WAVE_FORMAT_PCM instead */
            wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
            wfx.Format.cbSize = 0;
            wfx.Samples.wValidBitsPerSample = 0;
            wfx.dwChannelMask = 0;
            wfx.SubFormat = GUID_NULL;
            result = FilterCanCreateRenderPin(pFilter,(const WAVEFORMATEX*)&wfx);
            if( result != paNoError )
            {
                PaWindWDM_SetLastErrorInfo(result, "FilterCanCreateRenderPin(OUT) failed: %u,%u,%u", wfx.Format.nSamplesPerSec, wfx.Format.nChannels, wfx.Format.wBitsPerSample);
                return result;
            }
        }

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
    we have the capability to convert from inputSampleFormat to
    a native format

    - check that output device can support outputSampleFormat, or that
    we have the capability to convert from outputSampleFormat to
    a native format
    */
    if((inputChannelCount == 0)&&(outputChannelCount == 0))
    {
        PaWindWDM_SetLastErrorInfo(paSampleFormatNotSupported, "No input or output channels defined");
        result = paSampleFormatNotSupported; /* Not right error */
    }

    PA_LOGL_;
    return result;
}

static void ResetStreamEvents(PaWinWdmStream* stream) 
{
    unsigned i;
    ResetEvent(stream->eventAbort);
    for (i=0; i<2; ++i)
    {
        if (stream->eventsCapture[i])
        {
            ResetEvent(stream->eventsCapture[i]);
        }
        if (stream->eventsRender[i])
        {
            ResetEvent(stream->eventsRender[i]);
        }
    }
}

static void CloseStreamEvents(PaWinWdmStream* stream) 
{
    unsigned i;
    if (stream->eventAbort)
    {
        CloseHandle(stream->eventAbort);
        stream->eventAbort = 0;
    }
    /* Unregister notification handles for WaveRT */
    if (stream->capturePin && stream->capturePin->parentFilter->isWaveRT)
    {
        PinUnregisterNotificationHandle(stream->capturePin, stream->eventsCapture[0]);
    }
    if (stream->renderPin && stream->renderPin->parentFilter->isWaveRT)
    {
        PinUnregisterNotificationHandle(stream->renderPin, stream->eventsRender[0]);
    }

    for (i=0; i<2; ++i)
    {
        if (stream->eventsCapture[i])
        {
            CloseHandle(stream->eventsCapture[i]);
            stream->eventsCapture[i] = 0;
        }
        if (stream->eventsRender[i])
        {
            CloseHandle(stream->eventsRender[i]);
            stream->eventsRender[i];
        }
    }
}

typedef unsigned unsigned_type;
unsigned_type gcd(unsigned_type u, unsigned_type v)
{
    unsigned_type shift;

    /* GCD(0,x) := x */
    if (!(u & v)) /* (u == 0) || (v == 0) */
        return u | v;

    /* Let shift := lg K, where K is the greatest power of 2
    dividing both u and v. */
    for (shift = 0; ((u | v) & 1) == 0; ++shift) {
        u >>= 1;
        v >>= 1;
    }

    while ((u & 1) == 0)
        u >>= 1;

    /* From here on, u is always odd. */
    do {
        while ((v & 1) == 0)  /* Loop X */
            v >>= 1;

        /* Now u and v are both odd, so diff(u, v) is even.
        Let u = min(u, v), v = diff(u, v)/2. */
        if (u < v) {
            v -= u;
        } else {
            unsigned_type diff = u - v;
            u = v;
            v = diff;
        }
        v >>= 1;
    } while (v != 0);

    return u << shift;
}


/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                          PaStream** s,
                          const PaStreamParameters *inputParameters,
                          const PaStreamParameters *outputParameters,
                          double sampleRate,
                          unsigned long framesPerUserBuffer,
                          PaStreamFlags streamFlags,
                          PaStreamCallback *streamCallback,
                          void *userData )
{
    PaError result = paNoError;
    PaWinWdmHostApiRepresentation *wdmHostApi = (PaWinWdmHostApiRepresentation*)hostApi;
    PaWinWdmStream *stream = 0;
    /* unsigned long framesPerHostBuffer; these may not be equivalent for all implementations */
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
    int userInputChannels,userOutputChannels;
    WAVEFORMATEXTENSIBLE wfx;

    PA_LOGE_;
    PA_DEBUG(("OpenStream:sampleRate = %f\n",sampleRate));
    PA_DEBUG(("OpenStream:framesPerBuffer = %lu\n",framesPerUserBuffer));

    if( inputParameters )
    {
        userInputChannels = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            PaWindWDM_SetLastErrorInfo(paInvalidDevice, "paUseHostApiSpecificDeviceSpecification(in) not supported");
            return paInvalidDevice;
        }

        /* check that input device can support stream->userInputChannels */
        if( userInputChannels > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
        {
            PaWindWDM_SetLastErrorInfo(paInvalidChannelCount, "Invalid input channel count");
            return paInvalidChannelCount;
        }

        /* validate inputStreamInfo */
        if( inputParameters->hostApiSpecificStreamInfo )
        {
            PaWindWDM_SetLastErrorInfo(paIncompatibleHostApiSpecificStreamInfo, "Host API stream info not supported (in)");
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        }
    }
    else
    {
        userInputChannels = 0;
        inputSampleFormat = hostInputSampleFormat = paInt16; /* Supress 'uninitialised var' warnings. */
    }

    if( outputParameters )
    {
        userOutputChannels = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
        paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
        {
            PaWindWDM_SetLastErrorInfo(paInvalidDevice, "paUseHostApiSpecificDeviceSpecification(out) not supported");
            return paInvalidDevice;
        }

        /* check that output device can support stream->userInputChannels */
        if( userOutputChannels > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
        {
            PaWindWDM_SetLastErrorInfo(paInvalidChannelCount, "Invalid output channel count");
            return paInvalidChannelCount;
        }

        /* validate outputStreamInfo */
        if( outputParameters->hostApiSpecificStreamInfo )
        {
            PaWindWDM_SetLastErrorInfo(paIncompatibleHostApiSpecificStreamInfo, "Host API stream info not supported (out)");
            return paIncompatibleHostApiSpecificStreamInfo; /* this implementation doesn't use custom stream info */
        }
    }
    else
    {
        userOutputChannels = 0;
        outputSampleFormat = hostOutputSampleFormat = paInt16; /* Supress 'uninitialized var' warnings. */
    }

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
    {
        PaWindWDM_SetLastErrorInfo(paInvalidFlag, "Invalid flag supplied");
        return paInvalidFlag; /* unexpected platform specific flag */
    }

    stream = (PaWinWdmStream*)PaUtil_AllocateMemory( sizeof(PaWinWdmStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }
    /* Zero the stream object */
    /* memset((void*)stream,0,sizeof(PaWinWdmStream)); */

    if( streamCallback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
            &wdmHostApi->callbackStreamInterface, streamCallback, userData );
    }
    else
    {
        /* PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
            &wdmHostApi->blockingStreamInterface, streamCallback, userData ); */

        /* We don't support the blocking API yet */
        PA_DEBUG(("Blocking API not supported yet!\n"));
        PaWindWDM_SetLastErrorInfo(paUnanticipatedHostError, "Blocking API not supported yet");
        result = paUnanticipatedHostError;
        goto error;
    }

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    /* Instantiate the input pin if necessary */
    if(userInputChannels > 0)
    {
        PaWinWdmFilter* pFilter;

        result = paSampleFormatNotSupported;
        pFilter = wdmHostApi->filters[inputParameters->device];
        stream->userInputChannels = userInputChannels;

        if(((inputSampleFormat & ~paNonInterleaved) & pFilter->formats) != 0)
        {   /* inputSampleFormat is supported, so try to use it */
            hostInputSampleFormat = inputSampleFormat;
            FillWFEXT(&wfx, hostInputSampleFormat, sampleRate, stream->userInputChannels);
            stream->bytesPerInputFrame = wfx.Format.nBlockAlign;
            stream->capturePin = FilterCreateCapturePin(pFilter, (const WAVEFORMATEX*)&wfx, &result);
            stream->deviceInputChannels = stream->userInputChannels;
        }

        if(result != paNoError)
        {   /* Search through all PaSampleFormats to find one that works */
            hostInputSampleFormat = paFloat32;

            do {
                FillWFEXT(&wfx, hostInputSampleFormat, sampleRate, stream->userInputChannels);
                stream->bytesPerInputFrame = wfx.Format.nBlockAlign;
                stream->capturePin = FilterCreateCapturePin(pFilter, (const WAVEFORMATEX*)&wfx, &result);
                stream->deviceInputChannels = stream->userInputChannels;

                if(stream->capturePin == NULL) result = paSampleFormatNotSupported;
                if(result != paNoError)    hostInputSampleFormat <<= 1;
            }
            while(result != paNoError && hostInputSampleFormat <= paUInt8);
        }

        if(result != paNoError)
        {    /* None of the PaSampleFormats worked.  Set the hostInputSampleFormat to the best fit
             * and try a PCM format.
             **/
            hostInputSampleFormat =
                PaUtil_SelectClosestAvailableFormat( pFilter->formats, inputSampleFormat );

            /* Try a WAVE_FORMAT_PCM instead */
            wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
            wfx.Format.cbSize = 0;
            wfx.Samples.wValidBitsPerSample = 0;
            wfx.dwChannelMask = 0;
            wfx.SubFormat = GUID_NULL;
            stream->capturePin = FilterCreateCapturePin(pFilter,(const WAVEFORMATEX*)&wfx,&result);
            if(stream->capturePin == NULL) result = paSampleFormatNotSupported;
        }

        if( result != paNoError )
        {
            /* Some or all KS devices can only handle the exact number of channels
            * they specify. But PortAudio clients expect to be able to
            * at least specify mono I/O on a multi-channel device
            * If this is the case, then we will do the channel mapping internally
            **/
            if( stream->userInputChannels < pFilter->maxInputChannels )
            {
                FillWFEXT(&wfx,hostInputSampleFormat,sampleRate,pFilter->maxInputChannels);
                stream->bytesPerInputFrame = wfx.Format.nBlockAlign;
                stream->capturePin = FilterCreateCapturePin(pFilter,(const WAVEFORMATEX*)&wfx,&result);
                stream->deviceInputChannels = pFilter->maxInputChannels;

                if( result != paNoError )
                {
                    /* Try a WAVE_FORMAT_PCM instead */
                    wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
                    wfx.Format.cbSize = 0;
                    wfx.Samples.wValidBitsPerSample = 0;
                    wfx.dwChannelMask = 0;
                    wfx.SubFormat = GUID_NULL;
                    stream->capturePin = FilterCreateCapturePin(pFilter,(const WAVEFORMATEX*)&wfx,&result);
                }
            }
        }

        if(stream->capturePin == NULL)
        {
            PaWindWDM_SetLastErrorInfo(result, "Failed to create capture pin: sr=%u,ch=%u,bits=%u,align=%u",
                wfx.Format.nSamplesPerSec, wfx.Format.nChannels, wfx.Format.wBitsPerSample, wfx.Format.nBlockAlign);
            goto error;
        }

        switch(hostInputSampleFormat)
        {
        case paInt16: stream->inputSampleSize = 2; break;
        case paInt24: stream->inputSampleSize = 3; break;
        case paInt32:
        case paFloat32:    stream->inputSampleSize = 4; break;
        }

        stream->capturePin->frameSize /= stream->bytesPerInputFrame;
        PA_DEBUG(("Pin output frames: %d\n",stream->capturePin->frameSize));
    }
    else
    {
        stream->capturePin = NULL;
        stream->bytesPerInputFrame = 0;
    }

    /* Instantiate the output pin if necessary */
    if(userOutputChannels > 0)
    {
        PaWinWdmFilter* pFilter;

        result = paSampleFormatNotSupported;
        pFilter = wdmHostApi->filters[outputParameters->device];
        stream->userOutputChannels = userOutputChannels;

        if(((outputSampleFormat & ~paNonInterleaved) & pFilter->formats) != 0)
        {
            hostOutputSampleFormat = outputSampleFormat;
            FillWFEXT(&wfx,hostOutputSampleFormat,sampleRate,stream->userOutputChannels);
            stream->bytesPerOutputFrame = wfx.Format.nBlockAlign;
            stream->renderPin = FilterCreateRenderPin(pFilter,(WAVEFORMATEX*)&wfx,&result);
            stream->deviceOutputChannels = stream->userOutputChannels;
        }

        if(result != paNoError)
        {
            hostOutputSampleFormat = paFloat32;

            do {
                FillWFEXT(&wfx,hostOutputSampleFormat,sampleRate,stream->userOutputChannels);
                stream->bytesPerOutputFrame = wfx.Format.nBlockAlign;
                stream->renderPin = FilterCreateRenderPin(pFilter,(WAVEFORMATEX*)&wfx,&result);
                stream->deviceOutputChannels = stream->userOutputChannels;

                if(stream->renderPin == NULL) result = paSampleFormatNotSupported;
                if(result != paNoError)    hostOutputSampleFormat <<= 1;
            }
            while(result != paNoError && hostOutputSampleFormat <= paUInt8);
        }

        if(result != paNoError)
        {
            hostOutputSampleFormat =
                PaUtil_SelectClosestAvailableFormat( pFilter->formats, outputSampleFormat );

            /* Try a WAVE_FORMAT_PCM instead */
            wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
            wfx.Format.cbSize = 0;
            wfx.Samples.wValidBitsPerSample = 0;
            wfx.dwChannelMask = 0;
            wfx.SubFormat = GUID_NULL;
            stream->renderPin = FilterCreateRenderPin(pFilter,(WAVEFORMATEX*)&wfx,&result);
            if(stream->renderPin == NULL) result = paSampleFormatNotSupported;
        }

        if( result != paNoError )
        {
            /* Some or all KS devices can only handle the exact number of channels
            * they specify. But PortAudio clients expect to be able to
            * at least specify mono I/O on a multi-channel device
            * If this is the case, then we will do the channel mapping internally
            **/
            if( stream->userOutputChannels < pFilter->maxOutputChannels )
            {
                FillWFEXT(&wfx,hostOutputSampleFormat,sampleRate,pFilter->maxOutputChannels);
                stream->bytesPerOutputFrame = wfx.Format.nBlockAlign;
                stream->renderPin = FilterCreateRenderPin(pFilter,(const WAVEFORMATEX*)&wfx,&result);
                stream->deviceOutputChannels = pFilter->maxOutputChannels;
                if( result != paNoError )
                {
                    /* Try a WAVE_FORMAT_PCM instead */
                    wfx.Format.wFormatTag = WAVE_FORMAT_PCM;
                    wfx.Format.cbSize = 0;
                    wfx.Samples.wValidBitsPerSample = 0;
                    wfx.dwChannelMask = 0;
                    wfx.SubFormat = GUID_NULL;
                    stream->renderPin = FilterCreateRenderPin(pFilter,(const WAVEFORMATEX*)&wfx,&result);
                }
            }
        }

        if(stream->renderPin == NULL)
        {
            PaWindWDM_SetLastErrorInfo(result, "Failed to create render pin: sr=%u,ch=%u,bits=%u,align=%u",
                wfx.Format.nSamplesPerSec, wfx.Format.nChannels, wfx.Format.wBitsPerSample, wfx.Format.nBlockAlign);
            goto error;
        }

        switch(hostOutputSampleFormat)
        {
        case paInt16: stream->outputSampleSize = 2; break;
        case paInt24: stream->outputSampleSize = 3; break;
        case paInt32:
        case paFloat32: stream->outputSampleSize = 4; break;
        }

        stream->renderPin->frameSize /= stream->bytesPerOutputFrame;
        PA_DEBUG(("Pin output frames: %d\n",stream->renderPin->frameSize));
    }
    else
    {
        stream->renderPin = NULL;
        stream->bytesPerOutputFrame = 0;
    }

    /* Calculate the framesPerHostXxxxBuffer size based upon the suggested latency values */
    /* Record the buffer length */
    if(inputParameters)
    {
        /* Calculate the frames from the user's value - add a bit to round up */
        stream->framesPerHostIBuffer = (unsigned long)((inputParameters->suggestedLatency*sampleRate)+0.0001);
        if(stream->framesPerHostIBuffer > (unsigned long)sampleRate)
        { /* Upper limit is 1 second */
            stream->framesPerHostIBuffer = (unsigned long)sampleRate;
        }
        else if(stream->framesPerHostIBuffer < stream->capturePin->frameSize)
        {
            stream->framesPerHostIBuffer = stream->capturePin->frameSize;
        }
        PA_DEBUG(("Input frames chosen:%ld\n",stream->framesPerHostIBuffer));
    }

    if(outputParameters)
    {
        /* Calculate the frames from the user's value - add a bit to round up */
        stream->framesPerHostOBuffer = (unsigned long)((outputParameters->suggestedLatency*sampleRate)+0.0001);
        if(stream->framesPerHostOBuffer > (unsigned long)sampleRate)
        { /* Upper limit is 1 second */
            stream->framesPerHostOBuffer = (unsigned long)sampleRate;
        }
        else if(stream->framesPerHostOBuffer < stream->renderPin->frameSize)
        {
            stream->framesPerHostOBuffer = stream->renderPin->frameSize;
        }
        PA_DEBUG(("Output frames chosen:%ld\n",stream->framesPerHostOBuffer));
    }

#if 0
    /* Calculate the number of frames the processor should work with */
    {
        unsigned minFrameSize = min(stream->framesPerHostOBuffer,stream->framesPerHostIBuffer);
        unsigned gcdFrameSize = gcd(stream->framesPerHostOBuffer,stream->framesPerHostIBuffer);
        
        if (minFrameSize > 0)
        {
            /* full duplex case */
            if (gcdFrameSize < minFrameSize)
            {
                unsigned nIn, nOut;
                /* Make sure we don't have less than framesPer(user)Buffer */
                if (framesPerUserBuffer != paFramesPerBufferUnspecified) {
                    minFrameSize = max(framesPerUserBuffer, minFrameSize);
                }
                nOut = (stream->framesPerHostOBuffer + (minFrameSize - 1)) / minFrameSize;
                nIn = (stream->framesPerHostIBuffer + (minFrameSize - 1)) / minFrameSize;
                stream->framesPerHostIBuffer = nIn * minFrameSize;
                stream->framesPerHostOBuffer = nOut * minFrameSize;
                framesForProcessor = minFrameSize;
            }
            else {
                if (framesPerUserBuffer == paFramesPerBufferUnspecified) {
                    framesForProcessor = gcdFrameSize;
                }
                else {
                    unsigned nIn, nOut;
                    nOut = (stream->framesPerHostOBuffer + (framesPerUserBuffer - 1)) / framesPerUserBuffer;
                    nIn = (stream->framesPerHostIBuffer + (framesPerUserBuffer - 1)) / framesPerUserBuffer;
                    stream->framesPerHostIBuffer = nIn * framesPerUserBuffer;
                    stream->framesPerHostOBuffer = nOut * framesPerUserBuffer;
                    framesForProcessor = minFrameSize;
                }
            }
        }
        else {
            assert(gcdFrameSize > 0);
            /* Make sure we don't have less than framesPer(user)Buffer */
            framesForProcessor = max(gcdFrameSize, framesPerUserBuffer);
        }
    }
#endif
    /* Host buffer size is bounded to the largest of the input and output
    frame sizes */

    result =  PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
        stream->userInputChannels, inputSampleFormat, hostInputSampleFormat,
        stream->userOutputChannels, outputSampleFormat, hostOutputSampleFormat,
        sampleRate, streamFlags, framesPerUserBuffer,
        max(stream->framesPerHostIBuffer, stream->framesPerHostOBuffer), 
        paUtilBoundedHostBufferSize,
        streamCallback, userData );
    if( result != paNoError )
    {
        PaWindWDM_SetLastErrorInfo(result, "PaUtil_InitializeBufferProcessor failed: ich=%u, isf=%u, hisf=%u, och=%u, osf=%u, hosf=%u, sr=%lf, flags=0x%X, fpub=%u, fphb=%u",
            stream->userInputChannels, inputSampleFormat, hostInputSampleFormat,
            stream->userOutputChannels, outputSampleFormat, hostOutputSampleFormat,
            sampleRate, streamFlags, framesPerUserBuffer,
            max(stream->framesPerHostIBuffer, stream->framesPerHostOBuffer));
        goto error;
    }

    stream->streamRepresentation.streamInfo.inputLatency =
        ((double)stream->framesPerHostIBuffer) / sampleRate;
    stream->streamRepresentation.streamInfo.outputLatency =
        ((double)stream->framesPerHostOBuffer) / sampleRate;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    PA_DEBUG(("BytesPerInputFrame = %d\n",stream->bytesPerInputFrame));
    PA_DEBUG(("BytesPerOutputFrame = %d\n",stream->bytesPerOutputFrame));

    /* Allocate/get all the buffers for host I/O */
    if (stream->userInputChannels > 0)
    {
        if (!stream->capturePin->parentFilter->isWaveRT)
        {
            unsigned size = 2 * stream->framesPerHostIBuffer * stream->bytesPerInputFrame;
            /* Allocate input host buffer */
            stream->hostInBuffer = (char*)PaUtil_AllocateMemory(size);
            PA_DEBUG(("Input buffer allocated (size = %u)\n", size));
            if( !stream->hostInBuffer )
            {
                PA_DEBUG(("Cannot allocate host input buffer!\n"));
                PaWindWDM_SetLastErrorInfo(paInsufficientMemory, "Failed to allocate input buffer");
                result = paInsufficientMemory;
                goto error;
            }
            PA_DEBUG(("Input buffer start = %p\n",stream->hostInBuffer));
            stream->capturePin->fnEventHandler = PaPinCaptureEventHandler_WaveCyclic;
            stream->capturePin->fnSubmitHandler = PaPinCaptureSubmitHandler_WaveCyclic;
        }
        else
        {
            const DWORD dwTotalSize = 2 * stream->framesPerHostIBuffer * stream->bytesPerInputFrame;
            DWORD dwRequestedSize = dwTotalSize;
            BOOL bCallMemoryBarrier = FALSE;
            ULONG hwFifoLatency = 0;
            ULONG dummy;
            result = PinGetBufferWithNotification(stream->capturePin, (void**)&stream->hostInBuffer, &dwRequestedSize, &bCallMemoryBarrier);
            if (!result) 
            {
                PA_DEBUG(("Input buffer start = %p, size = %u\n", stream->hostInBuffer, dwRequestedSize));
                if (dwRequestedSize != dwTotalSize)
                {
                    PA_DEBUG(("Buffer length changed by driver from %u to %u !\n", dwTotalSize, dwRequestedSize));
                    /* Recalculate to what the driver has given us */
                    stream->framesPerHostIBuffer = dwRequestedSize / (2 * stream->bytesPerInputFrame);
                }
                stream->capturePin->fnEventHandler = PaPinCaptureEventHandler_WaveRT;
                stream->capturePin->fnSubmitHandler = PaPinCaptureSubmitHandler_WaveRT;
                stream->capturePin->fnMemBarrier = bCallMemoryBarrier ? MemoryBarrierRead : MemoryBarrierDummy;
            }
            else 
            {
                PA_DEBUG(("Failed to get input buffer (with notification)\n"));
                PaWindWDM_SetLastErrorInfo(paUnanticipatedHostError, "Failed to get input buffer (with notification)");
                result = paUnanticipatedHostError;
                goto error;
            }

            /* Get latency */
            result = PinGetHwLatency(stream->capturePin, &hwFifoLatency, &dummy, &dummy);
            if (!result)
            {
                stream->capturePin->hwLatency = hwFifoLatency;
            }
            else
            {
                PA_DEBUG(("Failed to get size of FIFO hardware buffer (is set to zero)\n"));
                stream->capturePin->hwLatency = 0;
            }
        }
    }
    else 
    {
        stream->hostInBuffer = 0;
    }
    if (stream->userOutputChannels > 0)
    {
        if (!stream->renderPin->parentFilter->isWaveRT)
        {
            unsigned size = 2 * stream->framesPerHostOBuffer * stream->bytesPerOutputFrame;
            /* Allocate output device buffer */
            stream->hostOutBuffer = (char*)PaUtil_AllocateMemory(size);
            PA_DEBUG(("Output buffer allocated (size = %u)\n", size));
            if( !stream->hostOutBuffer )
            {
                PA_DEBUG(("Cannot allocate host output buffer!\n"));
                PaWindWDM_SetLastErrorInfo(paInsufficientMemory, "Failed to allocate output buffer");
                result = paInsufficientMemory;
                goto error;
            }
            PA_DEBUG(("Output buffer start = %p\n",stream->hostOutBuffer));

            /* Allocate buffer used for silence */
            stream->hostSilenceBuffer = (char*)PaUtil_AllocateMemory(size);

            stream->renderPin->fnEventHandler = PaPinRenderEventHandler_WaveCyclic;
            stream->renderPin->fnSubmitHandler = PaPinRenderSubmitHandler_WaveCyclic;
        }
        else
        {
            const DWORD dwTotalSize = 2 * stream->framesPerHostOBuffer * stream->bytesPerOutputFrame;
            DWORD dwRequestedSize = dwTotalSize;
            BOOL bCallMemoryBarrier = FALSE;
            ULONG hwFifoLatency = 0;
            ULONG dummy;
            result = PinGetBufferWithNotification(stream->renderPin, (void**)&stream->hostOutBuffer, &dwRequestedSize, &bCallMemoryBarrier);
            if (!result) 
            {
                PA_DEBUG(("Output buffer start = %p, size = %u\n", stream->hostOutBuffer, dwRequestedSize));
                if (dwRequestedSize != dwTotalSize)
                {
                    PA_DEBUG(("Buffer length changed by driver from %u to %u !\n", dwTotalSize, dwRequestedSize));
                    /* Recalculate to what the driver has given us */
                    stream->framesPerHostOBuffer = dwRequestedSize / (2 * stream->bytesPerOutputFrame);
                }
                stream->renderPin->fnEventHandler = PaPinRenderEventHandler_WaveRT;
                stream->renderPin->fnSubmitHandler = PaPinRenderSubmitHandler_WaveRT;
                stream->renderPin->fnMemBarrier = bCallMemoryBarrier ? MemoryBarrierWrite : MemoryBarrierDummy;
            }
            else 
            {
                PA_DEBUG(("Failed to get output buffer (with notification)\n"));
                PaWindWDM_SetLastErrorInfo(paUnanticipatedHostError, "Failed to get output buffer (with notification)");
                result = paUnanticipatedHostError;
                goto error;
            }

            /* Get latency */
            result = PinGetHwLatency(stream->renderPin, &hwFifoLatency, &dummy, &dummy);
            if (!result)
            {
                stream->renderPin->hwLatency = hwFifoLatency;
            }
            else
            {
                PA_DEBUG(("Failed to get size of FIFO hardware buffer (is set to zero)\n"));
                stream->renderPin->hwLatency = 0;
            }
        }
    }
    else 
    {
        stream->hostOutBuffer = 0;
    }
    /* memset(stream->hostBuffer,0,size); */

    /* Abort */
    stream->eventAbort = CreateEvent(NULL, TRUE, FALSE, NULL);
    if(stream->userInputChannels > 0)
    {
        if (!stream->capturePin->parentFilter->isWaveRT)
        {
            /* WaveCyclic case */
            unsigned i;
            for (i = 0; i < 2; ++i) {
                /* Set up the packets */
                DATAPACKET *p = stream->packetsCapture + i;

                /* Record event */
                stream->eventsCapture[i] = CreateEvent(NULL, FALSE, FALSE, NULL);

                p->Signal.hEvent = stream->eventsCapture[i];
                p->Header.Data = stream->hostInBuffer + (i*stream->framesPerHostIBuffer*stream->bytesPerInputFrame);
                p->Header.FrameExtent = stream->framesPerHostIBuffer*stream->bytesPerInputFrame;
                p->Header.DataUsed = 0;
                p->Header.Size = sizeof(p->Header);
                p->Header.PresentationTime.Numerator = 1;
                p->Header.PresentationTime.Denominator = 1;
            }
        }
        else {
            /* Set up the "packets" */
            DATAPACKET *p = stream->packetsCapture + 0;

            /* Record event: WaveRT has a single event for 2 notification per buffer */
            stream->eventsCapture[0] = CreateEvent(NULL, FALSE, FALSE, NULL);

            p->Header.Data = stream->hostInBuffer;
            p->Header.FrameExtent = stream->framesPerHostIBuffer*stream->bytesPerInputFrame;
            p->Header.DataUsed = 0;
            p->Header.Size = sizeof(p->Header);
            p->Header.PresentationTime.Numerator = 1;
            p->Header.PresentationTime.Denominator = 1;

            ++p;
            p->Header.Data = stream->hostInBuffer + stream->framesPerHostIBuffer*stream->bytesPerInputFrame;
            p->Header.FrameExtent = stream->framesPerHostIBuffer*stream->bytesPerInputFrame;
            p->Header.DataUsed = 0;
            p->Header.Size = sizeof(p->Header);
            p->Header.PresentationTime.Numerator = 1;
            p->Header.PresentationTime.Denominator = 1;

            result = PinRegisterNotificationHandle(stream->capturePin, stream->eventsCapture[0]);

            if (result != paNoError)
            {
                PA_DEBUG(("Failed to register capture notification handle\n"));
                PaWindWDM_SetLastErrorInfo(paUnanticipatedHostError, "Failed to register capture notification handle");
                result = paUnanticipatedHostError;
                goto error;
            }

            result = PinRegisterPositionRegister(stream->capturePin);

            if (result != paNoError)
            {
                PA_DEBUG(("Failed to register capture position register, using PinGetAudioPositionViaIOCTL\n"));
                stream->capturePin->fnAudioPosition = PinGetAudioPositionViaIOCTL;
            }
            else
            {
                stream->capturePin->fnAudioPosition = PinGetAudioPositionDirect;
            }
        }
    }
    if(stream->userOutputChannels > 0)
    {
        if (!stream->renderPin->parentFilter->isWaveRT)
        {
            /* WaveCyclic case */
            const unsigned frameBufferBytes = stream->framesPerHostOBuffer*stream->bytesPerOutputFrame;
            unsigned i;
            for (i = 0; i < 2; ++i)
            {
                /* Set up the packets */
                DATAPACKET *p = stream->packetsRender + i;

                /* Playback event */
                stream->eventsRender[i] = CreateEvent(NULL, FALSE, FALSE, NULL);

                /* In this case, we just use the packets as ptr to the device buffer */
                p->Signal.hEvent = stream->eventsRender[i];
                p->Header.Data = stream->hostOutBuffer + (i*frameBufferBytes);
                p->Header.FrameExtent = frameBufferBytes;
                p->Header.DataUsed = frameBufferBytes;
                p->Header.Size = sizeof(p->Header);
                p->Header.PresentationTime.Numerator = 1;
                p->Header.PresentationTime.Denominator = 1;
            }
        }
        else
        {
            /* WaveRT case */

            /* Set up the "packets" */
            DATAPACKET *p = stream->packetsRender;

            /* The only playback event */
            stream->eventsRender[0] = CreateEvent(NULL, FALSE, FALSE, NULL);

            /* In this case, we just use the packets as ptr to the device buffer */
            p->Header.Data = stream->hostOutBuffer;
            p->Header.FrameExtent = stream->framesPerHostOBuffer*stream->bytesPerOutputFrame;
            p->Header.DataUsed = stream->framesPerHostOBuffer*stream->bytesPerOutputFrame;
            p->Header.Size = sizeof(p->Header);
            p->Header.PresentationTime.Numerator = 1;
            p->Header.PresentationTime.Denominator = 1;
            
            ++p;
            p->Header.Data = stream->hostOutBuffer + stream->framesPerHostOBuffer*stream->bytesPerOutputFrame;
            p->Header.FrameExtent = stream->framesPerHostOBuffer*stream->bytesPerOutputFrame;
            p->Header.DataUsed = stream->framesPerHostOBuffer*stream->bytesPerOutputFrame;
            p->Header.Size = sizeof(p->Header);
            p->Header.PresentationTime.Numerator = 1;
            p->Header.PresentationTime.Denominator = 1;

            result = PinRegisterNotificationHandle(stream->renderPin, stream->eventsRender[0]);

            if (result != paNoError)
            {
                PA_DEBUG(("Failed to register rendering notification handle\n"));
                PaWindWDM_SetLastErrorInfo(paUnanticipatedHostError, "Failed to register rendering notification handle");
                result = paUnanticipatedHostError;
                goto error;
            }

            result = PinRegisterPositionRegister(stream->renderPin);

            if (result != paNoError)
            {
                PA_DEBUG(("Failed to register rendering position register, using PinGetAudioPositionViaIOCTL\n"));
                stream->renderPin->fnAudioPosition = PinGetAudioPositionViaIOCTL;
            }
            else
            {
                stream->renderPin->fnAudioPosition = PinGetAudioPositionDirect;
            }
        }
    }

#if 0
    /* Follow KS guidlines, and move pin states to pause via aquire */
    if (stream->capturePin)
    {
        if (PinSetState(stream->capturePin, KSSTATE_ACQUIRE) != paNoError)
        {
            PA_DEBUG(("Failed to set recording pin to KSSTATE_ACQUIRE\n"));
            result = paUnanticipatedHostError;
            goto error;
        }

        if (PinSetState(stream->capturePin, KSSTATE_PAUSE) != paNoError)
        {
            PA_DEBUG(("Failed to set recording pin to KSSTATE_PAUSE\n"));
            result = paUnanticipatedHostError;
            goto error;
        }
    }
    if (stream->renderPin)
    {
        if (PinSetState(stream->renderPin, KSSTATE_ACQUIRE) != paNoError)
        {
            PA_DEBUG(("Failed to set playback pin to KSSTATE_ACQUIRE\n"));
            result = paUnanticipatedHostError;
            goto error;
        }

        if (PinSetState(stream->renderPin, KSSTATE_PAUSE) != paNoError)
        {
            PA_DEBUG(("Failed to set playback pin to KSSTATE_PAUSE\n"));
            result = paUnanticipatedHostError;
            goto error;
        }
    }
#endif
    stream->streamStarted = 0;
    stream->streamActive = 0;
    stream->streamStop = 0;
    stream->streamAbort = 0;
    stream->streamFlags = streamFlags;
    stream->oldProcessPriority = REALTIME_PRIORITY_CLASS;

    *s = (PaStream*)stream;

    PA_LOGL_;
    return result;

error:
    CloseStreamEvents(stream);
    
    if(stream->hostInBuffer && !stream->capturePin->parentFilter->isWaveRT)
        PaUtil_FreeMemory( stream->hostInBuffer );

    if(stream->hostOutBuffer && !stream->renderPin->parentFilter->isWaveRT)
        PaUtil_FreeMemory( stream->hostOutBuffer );

    if(stream->renderPin)
        PinClose(stream->renderPin);
    if(stream->capturePin)
        PinClose(stream->capturePin);

    if( stream )
        PaUtil_FreeMemory( stream );

    PA_LOGL_;
    return result;
}

/*
When CloseStream() is called, the multi-api layer ensures that
the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    PaError result = paNoError;
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;

    assert(!stream->streamStarted);
    assert(!stream->streamActive);

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    CloseStreamEvents(stream);

    if(stream->hostInBuffer && !stream->capturePin->parentFilter->isWaveRT)
        PaUtil_FreeMemory( stream->hostInBuffer );

    if(stream->hostOutBuffer && !stream->renderPin->parentFilter->isWaveRT)
        PaUtil_FreeMemory( stream->hostOutBuffer );

    if(stream->renderPin)
        PinClose(stream->renderPin);
    if(stream->capturePin)
        PinClose(stream->capturePin);

    PaUtil_FreeMemory( stream );

    PA_LOGL_;
    return result;
}

/*
Write the supplied packet to the pin
Asynchronous
Should return paNoError on success
*/
static PaError PinWrite(HANDLE h, DATAPACKET* p)
{
    PaError result = paNoError;
    unsigned long cbReturned = 0;
    BOOL fRes = DeviceIoControl(h,
        IOCTL_KS_WRITE_STREAM,
        NULL,
        0,
        &p->Header,
        p->Header.Size,
        &cbReturned,
        &p->Signal);
    if (!fRes) {
        DWORD dwError = GetLastError();

        if (dwError != ERROR_IO_PENDING) {
            result = paInternalError;
        }
    }
    return result;
}

/*
Read to the supplied packet from the pin
Asynchronous
Should return paNoError on success
*/
static PaError PinRead(HANDLE h, DATAPACKET* p)
{
    PaError result = paNoError;
    unsigned long cbReturned = 0;
    BOOL fRes = DeviceIoControl(h,
        IOCTL_KS_READ_STREAM,
        NULL,
        0,
        &p->Header,
        p->Header.Size,
        &cbReturned,
        &p->Signal);
    if (!fRes) {
        DWORD dwError = GetLastError();

        if (dwError != ERROR_IO_PENDING) {
            result = paInternalError;
        }
    }
    return result;
}

/*
Copy the first interleaved channel of 16 bit data to the other channels
*/
static void DuplicateFirstChannelInt16(void* buffer, int channels, int samples)
{
    unsigned short* data = (unsigned short*)buffer;
    int channel;
    unsigned short sourceSample;
    while( samples-- )
    {
        sourceSample = *data++;
        channel = channels-1;
        while( channel-- )
        {
            *data++ = sourceSample;
        }
    }
}

/*
Copy the first interleaved channel of 24 bit data to the other channels
*/
static void DuplicateFirstChannelInt24(void* buffer, int channels, int samples)
{
    unsigned char* data = (unsigned char*)buffer;
    int channel;
    unsigned char sourceSample[3];
    while( samples-- )
    {
        sourceSample[0] = data[0];
        sourceSample[1] = data[1];
        sourceSample[2] = data[2];
        data += 3;
        channel = channels-1;
        while( channel-- )
        {
            data[0] = sourceSample[0];
            data[1] = sourceSample[1];
            data[2] = sourceSample[2];
            data += 3;
        }
    }
}

/*
Copy the first interleaved channel of 32 bit data to the other channels
*/
static void DuplicateFirstChannelInt32(void* buffer, int channels, int samples)
{
    unsigned long* data = (unsigned long*)buffer;
    int channel;
    unsigned long sourceSample;
    while( samples-- )
    {
        sourceSample = *data++;
        channel = channels-1;
        while( channel-- )
        {
            *data++ = sourceSample;
        }
    }
}

/*
Increase the priority of the calling thread to RT 
*/
static HANDLE BumpThreadPriority() 
{
    HANDLE hThread = GetCurrentThread();
    DWORD dwTask = 0;
    HANDLE hAVRT = NULL;

    /* If we have access to AVRT.DLL (Vista and later), use it */
    if (FunctionAvSetMmThreadCharacteristics != NULL) 
    {
        hAVRT = FunctionAvSetMmThreadCharacteristics("Pro Audio", &dwTask);
        if (hAVRT != NULL) 
        {
            BOOL bret = FunctionAvSetMmThreadPriority(hAVRT, PA_AVRT_PRIORITY_CRITICAL);
            if (!bret)
            {
                PA_DEBUG(("Set mm thread prio to critical failed!\n"));
            }
        }
        else
        {
            PA_DEBUG(("Set mm thread characteristic to 'Pro Audio' failed!\n"));
        }
    }
    else
    {
        /* For XP and earlier */
        if (timeBeginPeriod(1) != TIMERR_NOERROR) {
            PA_DEBUG(("timeBeginPeriod(1) failed!\n"));
        }

        if (!SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL)) {
            PA_DEBUG(("SetThreadPriority failed!\n"));
        }

    }
    return hAVRT;
}

/*
Decrease the priority of the calling thread to normal
*/
static void DropThreadPriority(HANDLE hAVRT)
{
    HANDLE hThread = GetCurrentThread();

    if (hAVRT != NULL) 
    {
        FunctionAvSetMmThreadPriority(hAVRT, PA_AVRT_PRIORITY_NORMAL);
        FunctionAvRevertMmThreadCharacteristics(hAVRT);
    }
    else 
    {
        SetThreadPriority(hThread, THREAD_PRIORITY_NORMAL);
        timeEndPeriod(1);
    }

}

static PaError StartPin(PaWinWdmPin* pin)
{
    PaError result;
    result = PinSetState(pin, KSSTATE_ACQUIRE);
    if (result != paNoError)
    {
        PinSetState(pin, KSSTATE_STOP);
        return result;
    }
    result = PinSetState(pin, KSSTATE_PAUSE);
    if (result != paNoError)
    {
        PinSetState(pin, KSSTATE_STOP);
        return result;
    }
    result = PinSetState(pin, KSSTATE_RUN);
    return result;
}

static PaError StartPins(PaProcessThreadInfo* pInfo)
{
    PaError result = paNoError;
    /* Start the pins as synced as possible */
    if (pInfo->stream->capturePin)
    {
        result = StartPin(pInfo->stream->capturePin);
    }
    if(pInfo->stream->renderPin)
    {
        result = StartPin(pInfo->stream->renderPin);
    }
    /* Submit buffers */
    if(pInfo->stream->capturePin)
    {
        if (!pInfo->stream->capturePin->parentFilter->isWaveRT)
        {
            PinRead(pInfo->stream->capturePin->handle, pInfo->stream->packetsCapture + 0);
            PinRead(pInfo->stream->capturePin->handle, pInfo->stream->packetsCapture + 1);
            /* FIXME - do error checking */
        }
        pInfo->pending += 2;
    }
    if(pInfo->stream->renderPin)
    {
        pInfo->priming += 2;
        ++pInfo->pending;
        SetEvent(pInfo->stream->eventsRender[0]);
        if (!pInfo->stream->renderPin->parentFilter->isWaveRT) 
        {
            SetEvent(pInfo->stream->eventsRender[1]);
            ++pInfo->pending;
        }
    }

    PA_DEBUG(("StartPins = %d\n", result));

    return result;
}

static PaError StopPin(PaWinWdmPin* pin)
{
    PinSetState(pin, KSSTATE_PAUSE);
    PinSetState(pin, KSSTATE_STOP);
    return paNoError;
}


static PaError StopPins(PaProcessThreadInfo* pInfo)
{
    PaError result = paNoError;
    if(pInfo->stream->renderPin)
    {
        StopPin(pInfo->stream->renderPin);
    }
    if(pInfo->stream->capturePin)
    {
        StopPin(pInfo->stream->capturePin);
    }
    return result;
}

static void PaDoProcessing(PaProcessThreadInfo* pInfo)
{
    int i, framesProcessed, doChannelCopy = 0;
    /* Do necessary buffer processing (which will invoke user callback if necessary) */
    while (pInfo->cbResult == paContinue &&
           (pInfo->renderHead != pInfo->renderTail || pInfo->captureHead != pInfo->captureTail))
    {
        PaUtil_BeginCpuLoadMeasurement( &pInfo->stream->cpuLoadMeasurer );
        PaUtil_BeginBufferProcessing(&pInfo->stream->bufferProcessor, &pInfo->ti, pInfo->underover);

        pInfo->underover = 0; /* Reset the (under|over)flow status */

        if (pInfo->renderTail != pInfo->renderHead)
        {
            DATAPACKET* packet = pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask];

            assert(packet != 0);
            assert(packet->Header.Data != 0);

            PaUtil_SetOutputFrameCount(&pInfo->stream->bufferProcessor, pInfo->stream->framesPerHostOBuffer);

            if( pInfo->stream->userOutputChannels == 1 )
            {
                /* Write the single user channel to the first interleaved block */
                PaUtil_SetOutputChannel(&pInfo->stream->bufferProcessor, 
                    0, 
                    packet->Header.Data, 
                    pInfo->stream->deviceOutputChannels);
                /* We will do a copy to the other channels after the data has been written */
                doChannelCopy = 1;
            }
            else
            {
                for(i=0;i<pInfo->stream->userOutputChannels;i++)
                {
                    /* Only write the user output channels. Leave the rest blank */
                    PaUtil_SetOutputChannel(&pInfo->stream->bufferProcessor,
                        i,
                        ((unsigned char*)(packet->Header.Data))+(i*pInfo->stream->outputSampleSize),
                        pInfo->stream->deviceOutputChannels);
                }
            }
        }

        if (pInfo->captureTail != pInfo->captureHead)
        {
            DATAPACKET* packet = pInfo->capturePackets[pInfo->captureTail & cPacketsArrayMask];

            assert(packet != 0);
            assert(packet->Header.Data != 0);

            PaUtil_SetInputFrameCount(&pInfo->stream->bufferProcessor, pInfo->stream->framesPerHostIBuffer);
            for(i=0;i<pInfo->stream->userInputChannels;i++)
            {
                /* Only read as many channels as the user wants */
                PaUtil_SetInputChannel(&pInfo->stream->bufferProcessor,
                    i,
                    ((unsigned char*)(packet->Header.Data))+(i*pInfo->stream->inputSampleSize),
                    pInfo->stream->deviceInputChannels);
            }
        }


        if (pInfo->stream->capturePin && pInfo->stream->renderPin && (!pInfo->priming)) /* full duplex */
        {
            /* Only call the EndBufferProcessing function when the total input frames == total output frames */
            const unsigned long totalInputFrameCount = pInfo->stream->bufferProcessor.hostInputFrameCount[0] + pInfo->stream->bufferProcessor.hostInputFrameCount[1];
            const unsigned long totalOutputFrameCount = pInfo->stream->bufferProcessor.hostOutputFrameCount[0] + pInfo->stream->bufferProcessor.hostOutputFrameCount[1];

            if(totalInputFrameCount == totalOutputFrameCount)
            {
                framesProcessed = PaUtil_EndBufferProcessing(&pInfo->stream->bufferProcessor, &pInfo->cbResult);
            }
            else
            {
                framesProcessed = 0;
            }
        }
        else 
        {
            framesProcessed = PaUtil_EndBufferProcessing(&pInfo->stream->bufferProcessor, &pInfo->cbResult);
        }
        
        __PA_DEBUG(("Frames processed: %u %s\n", framesProcessed, (pInfo->priming ? "(priming)":"")));

        if( doChannelCopy )
        {
            DATAPACKET* packet = pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask];
            /* Copy the first output channel to the other channels */
            switch (pInfo->stream->outputSampleSize)
            {
            case 2:
                DuplicateFirstChannelInt16(packet->Header.Data, pInfo->stream->deviceOutputChannels, pInfo->stream->framesPerHostOBuffer);
                break;
            case 3:
                DuplicateFirstChannelInt24(packet->Header.Data, pInfo->stream->deviceOutputChannels, pInfo->stream->framesPerHostOBuffer);
                break;
            case 4:
                DuplicateFirstChannelInt32(packet->Header.Data, pInfo->stream->deviceOutputChannels, pInfo->stream->framesPerHostOBuffer);
                break;
            default:
                assert(0); /* Unsupported format! */
                break;
            }
        }
        PaUtil_EndCpuLoadMeasurement( &pInfo->stream->cpuLoadMeasurer, framesProcessed );

        if (pInfo->captureTail != pInfo->captureHead)
        {
            if (!pInfo->stream->streamStop)
            {
                pInfo->stream->capturePin->fnSubmitHandler(pInfo, pInfo->captureTail);
            }
            pInfo->captureTail++;
        }

        if (pInfo->renderTail != pInfo->renderHead)
        {
            if (!pInfo->stream->streamStop)
            {
                pInfo->stream->renderPin->fnSubmitHandler(pInfo, pInfo->renderTail);
            }
            pInfo->renderTail++;
        }

    }
}

PA_THREAD_FUNC ProcessingThread(void* pParam)
{
    PaError result = paNoError;
    HANDLE hAVRT = NULL;
    HANDLE handles[5];
    unsigned noOfHandles = 0;
    unsigned captureEvents = 0;
    unsigned renderEvents = 0;

    PaProcessThreadInfo info;
    memset(&info, 0, sizeof(PaProcessThreadInfo));
    info.stream = (PaWinWdmStream*)pParam;

    PA_LOGE_;

    info.ti.inputBufferAdcTime = 0.0;
    info.ti.currentTime = 0.0;
    info.ti.outputBufferDacTime = 0.0;
    
    PA_DEBUG(("Out buffer len: %.3f ms\n",(2000*info.stream->framesPerHostOBuffer) / info.stream->streamRepresentation.streamInfo.sampleRate));
    PA_DEBUG(("In buffer len: %.3f ms\n",(2000*info.stream->framesPerHostIBuffer) / info.stream->streamRepresentation.streamInfo.sampleRate));
    info.timeout = max(
        ((2000*(DWORD)info.stream->framesPerHostOBuffer) / (DWORD)info.stream->streamRepresentation.streamInfo.sampleRate),
        ((2000*(DWORD)info.stream->framesPerHostIBuffer) / (DWORD)info.stream->streamRepresentation.streamInfo.sampleRate));
    info.timeout = max(info.timeout+1,1);
    PA_DEBUG(("Timeout = %ld ms\n",info.timeout));

    /* Setup handle array for WFMO */
    if (info.stream->capturePin != 0) {
        handles[noOfHandles++] = info.stream->eventsCapture[0];
        if (!info.stream->capturePin->parentFilter->isWaveRT)
        {
            handles[noOfHandles++] = info.stream->eventsCapture[1];
        }
        captureEvents = noOfHandles;
        renderEvents = noOfHandles;
        info.inBufferSize = 2 * info.stream->bytesPerInputFrame * info.stream->framesPerHostIBuffer;
    }

    if (info.stream->renderPin != 0) {
        handles[noOfHandles++] = info.stream->eventsRender[0];
        if (!info.stream->renderPin->parentFilter->isWaveRT) {
            handles[noOfHandles++] = info.stream->eventsRender[1];
        }
        renderEvents = noOfHandles;
        info.outBufferSize = 2 * info.stream->bytesPerOutputFrame * info.stream->framesPerHostOBuffer;
    }
    handles[noOfHandles++] = info.stream->eventAbort;
    assert(noOfHandles <= ARRAYSIZE(handles));

    /* Heighten priority here */
    hAVRT = BumpThreadPriority();

    /* Start render and capture pins */
    if (StartPins(&info) != paNoError) 
    {
        PA_DEBUG(("Failed to start device(s)!\n"));
    }

    while(!info.stream->streamAbort)
    {
        unsigned eventSignalled;
        unsigned long wait = WaitForMultipleObjects(noOfHandles, handles, FALSE, 0);
        eventSignalled = wait - WAIT_OBJECT_0;

        if (wait == WAIT_FAILED) 
        {
            PA_DEBUG(("Wait failed = %ld! \n",wait));
            break;
        }

        if (wait == WAIT_TIMEOUT)
        {
            wait = WaitForMultipleObjects(noOfHandles, handles, FALSE, info.timeout);
            eventSignalled = wait - WAIT_OBJECT_0;
        }
        else 
        {
            if (eventSignalled < captureEvents)
            {
                if (info.captureHead - info.captureTail > 1)
                {
                    PA_DEBUG(("Input overflow!\n"));
                    info.underover |= paInputOverflow;
                }
            }
            else if (eventSignalled < renderEvents) {
                if (!info.priming && info.renderHead - info.renderTail > 1)
                {
                    PA_DEBUG(("Output underflow!\n"));
                    info.underover |= paOutputUnderflow;
                }
            }
            
        }
        
        if (wait == WAIT_TIMEOUT)
        {
            continue;
        }

        if (eventSignalled < captureEvents)
        {
            info.stream->capturePin->fnEventHandler(&info, eventSignalled);
        }
        else if (eventSignalled < renderEvents)
        {
            eventSignalled -= captureEvents;
            info.stream->renderPin->fnEventHandler(&info, eventSignalled);
        }
        else {
            assert(info.stream->streamAbort);
            PA_DEBUG(("Stream abort!"));
            continue;
        }

        /* Handle processing */
        PaDoProcessing(&info);

        if(info.stream->streamStop && info.cbResult != paComplete)
        {
            PA_DEBUG(("Stream stop! pending=%d\n",info.pending));
            info.cbResult = paComplete; /* Stop, but play remaining buffers */
        }

        if(info.pending<=0)
        {
            PA_DEBUG(("pending==0 finished...;\n"));
            break;
        }
        if((!info.stream->renderPin)&&(info.cbResult!=paContinue))
        {
            PA_DEBUG(("record only cbResult=%d...;\n",info.cbResult));
            break;
        }
    }

    PA_DEBUG(("Finished processing loop\n"));

    StopPins(&info);

    /* Lower prio here */
    DropThreadPriority(hAVRT);

    info.stream->streamActive = 0;

    if((!info.stream->streamStop)&&(!info.stream->streamAbort))
    {
        /* Invoke the user stream finished callback */
        /* Only do it from here if not being stopped/aborted by user */
        if( info.stream->streamRepresentation.streamFinishedCallback != 0 )
            info.stream->streamRepresentation.streamFinishedCallback( info.stream->streamRepresentation.userData );
    }
    info.stream->streamStop = 0;
    info.stream->streamAbort = 0;

    PA_LOGL_;
    return 0;
}


static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    DWORD dwID;
 
    PA_LOGE_;

    stream->streamStop = 0;
    stream->streamAbort = 0;

    ResetStreamEvents(stream);

    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    stream->oldProcessPriority = GetPriorityClass(GetCurrentProcess());
    /* Uncomment the following line to enable dynamic boosting of the process
    * priority to real time for best low latency support
    * Disabled by default because RT processes can easily block the OS */
    /*ret = SetPriorityClass(GetCurrentProcess(),REALTIME_PRIORITY_CLASS);
    PA_DEBUG(("Class ret = %d;",ret));*/

    stream->streamStarted = 1;
    stream->streamThread = CREATE_THREAD_FUNCTION (NULL, 0, ProcessingThread, stream, CREATE_SUSPENDED, &dwID);
    if(stream->streamThread == NULL)
    {
        stream->streamStarted = 0;
        result = paInsufficientMemory;
        goto end;
    }
    ResumeThread(stream->streamThread);

    /* Setting thread prios in BumpThreadPriority now 
    ret = SetThreadPriority(stream->streamThread,THREAD_PRIORITY_TIME_CRITICAL);
    PA_DEBUG(("Priority ret = %d;",ret)); */

    /* Make the stream active */
    stream->streamActive = 1;

end:
    PA_LOGL_;
    return result;
}


static PaError StopStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    int doCb = 0;

    PA_LOGE_;

    if(stream->streamActive)
    {
        doCb = 1;
        stream->streamStop = 1;
        if (WaitForSingleObject(stream->streamThread, 1000) != WAIT_OBJECT_0)
        {
            PA_DEBUG(("StopStream: stream thread terminated\n"));
            TerminateThread(stream->streamThread, -1);
            result = paTimedOut;
        }
    }

    CloseHandle(stream->streamThread);
    stream->streamThread = 0;
    stream->streamStarted = 0;

    if(doCb)
    {
        /* Do user callback now after all state has been reset */
        /* This means it should be safe for the called function */
        /* to invoke e.g. StartStream */
        if( stream->streamRepresentation.streamFinishedCallback != 0 )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    }

    PA_LOGL_;
    return result;
}

static PaError AbortStream( PaStream *s )
{
    PaError result = paNoError;
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    int doCb = 0;

    PA_LOGE_;

    if(stream->streamActive)
    {
        doCb = 1;
        stream->streamAbort = 1;
        SetEvent(stream->eventAbort); /* Signal immediately */
        if (WaitForSingleObject(stream->streamThread, 10000) != WAIT_OBJECT_0)
        {
            PA_DEBUG(("AbortStream: stream thread terminated\n"));
            TerminateThread(stream->streamThread, -1);
            result = paTimedOut;
        }
        assert(!stream->streamActive);
    }
    CloseHandle(stream->streamThread);
    stream->streamThread = NULL;
    stream->streamStarted = 0;

    if(doCb)
    {
        /* Do user callback now after all state has been reset */
        /* This means it should be safe for the called function */
        /* to invoke e.g. StartStream */
        if( stream->streamRepresentation.streamFinishedCallback != 0 )
            stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    }

    stream->streamActive = 0;
    stream->streamStarted = 0;

    PA_LOGL_;
    return result;
}


static PaError IsStreamStopped( PaStream *s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    int result = 0;

    PA_LOGE_;

    if(!stream->streamStarted)
        result = 1;

    PA_LOGL_;
    return result;
}


static PaError IsStreamActive( PaStream *s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    int result = 0;

    PA_LOGE_;

    if(stream->streamActive)
        result = 1;

    PA_LOGL_;
    return result;
}


static PaTime GetStreamTime( PaStream* s )
{
    PA_LOGE_;
    PA_LOGL_;
    (void)s;
    return PaUtil_GetTime();
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;
    double result;
    PA_LOGE_;
    result = PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
    PA_LOGL_;
    return result;
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
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    PA_LOGL_;
    return paInternalError;
}


static PaError WriteStream( PaStream* s,
                           const void *buffer,
                           unsigned long frames )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    PA_LOGL_;
    return paInternalError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;

    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    PA_LOGL_;
    return 0;
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaWinWdmStream *stream = (PaWinWdmStream*)s;

    PA_LOGE_;
    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/
    PA_LOGL_;
    return 0;
}

/***************************************************************************************/
/* Event and submit handlers for WaveCyclic                                            */
/***************************************************************************************/

static void PaPinCaptureEventHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    assert( eventIndex < 2 );
    pInfo->capturePackets[pInfo->captureHead & cPacketsArrayMask] = pInfo->stream->packetsCapture + eventIndex;

    __PA_DEBUG(("Capture event: idx=%u\n", eventIndex));
    ++pInfo->captureHead;
    --pInfo->pending;
}

static void PaPinCaptureSubmitHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    DATAPACKET* packet = pInfo->capturePackets[pInfo->captureTail & cPacketsArrayMask];
    pInfo->capturePackets[pInfo->captureTail & cPacketsArrayMask] = 0;
    assert(packet != 0);
    __PA_DEBUG(("Capture submit: %u\n", eventIndex));
    packet->Header.DataUsed = 0; /* Reset for reuse */
    PinRead(pInfo->stream->capturePin->handle, packet);
    ++pInfo->pending;
}

static void PaPinRenderEventHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    assert( eventIndex < 2 );

    pInfo->renderPackets[pInfo->renderHead & cPacketsArrayMask] = pInfo->stream->packetsRender + eventIndex;
    __PA_DEBUG(("Render event : idx=%u head=%u\n", eventIndex, pInfo->renderHead));
    ++pInfo->renderHead;
    --pInfo->pending;
}

static void PaPinRenderSubmitHandler_WaveCyclic(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    DATAPACKET* packet = pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask];
    pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask] = 0;
    assert(packet != 0);

    __PA_DEBUG(("Render submit : %u idx=%u\n", pInfo->renderTail, (unsigned)(packet - pInfo->stream->packetsRender)));
    PinWrite(pInfo->stream->renderPin->handle, packet);
    ++pInfo->pending;
    if (pInfo->priming)
    {
        --pInfo->priming;
    }
}

/***************************************************************************************/
/* Event and submit handlers for WaveRT                                                */
/***************************************************************************************/

static void PaPinCaptureEventHandler_WaveRT(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    unsigned long pos;
    unsigned realInBuf;
    const unsigned halfInputBuffer = pInfo->inBufferSize >> 1;
    PaWinWdmPin* pin = pInfo->stream->capturePin;

    /* Get hold of current ADC position */
    pin->fnAudioPosition(pin, &pos);
    /* Wrap it (robi: why not use hw latency compensation here ?? because pos then gets _way_ off from
       where it should be, i.e. at beginning or half buffer position. Why? No idea.)  */
    pos %= pInfo->inBufferSize;
    /* Then realInBuf will point to "other" half of double buffer */
    realInBuf = pos < halfInputBuffer ? 1U : 0U;

    /* Call barrier (or dummy) */
    pin->fnMemBarrier();

    /* Put it in queue */
    pInfo->capturePackets[pInfo->captureHead & cPacketsArrayMask] = pInfo->stream->packetsCapture + realInBuf;

    __PA_DEBUG(("Capture event (WaveRT): idx=%u head=%u (pos = %4.1lf%%)\n", realInBuf, pInfo->captureHead, (pos * 100.0 / pInfo->inBufferSize)));

    ++pInfo->captureHead;
    --pInfo->pending;
}

static void PaPinCaptureSubmitHandler_WaveRT(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    pInfo->capturePackets[pInfo->captureTail & cPacketsArrayMask] = 0;
    ++pInfo->pending;
}

static void PaPinRenderEventHandler_WaveRT(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    unsigned long pos;
    unsigned realOutBuf;
    const unsigned halfOutputBuffer = pInfo->outBufferSize >> 1;
    PaWinWdmPin* pin = pInfo->stream->renderPin;

    /* Get hold of current DAC position */
    pin->fnAudioPosition(pin, &pos);
    /* Compensate for HW FIFO to get to last read buffer position */
    pos += pin->hwLatency;
    /* Wrap it */
    pos %= pInfo->outBufferSize;
    /* Then realOutBuf will point to "other" half of double buffer */
    realOutBuf = pos < halfOutputBuffer ? 1U : 0U;

    if (pInfo->priming)
    {
        realOutBuf = pInfo->renderHead & 0x1;
    }
    pInfo->renderPackets[pInfo->renderHead & cPacketsArrayMask] = pInfo->stream->packetsRender + realOutBuf;

    __PA_DEBUG(("Render event (WaveRT) : idx=%u head=%u (pos = %4.1lf%%)\n", realOutBuf, pInfo->renderHead, (pos * 100.0 / pInfo->outBufferSize) ));

    ++pInfo->renderHead;
    --pInfo->pending;
}

static void PaPinRenderSubmitHandler_WaveRT(PaProcessThreadInfo* pInfo, unsigned eventIndex)
{
    PaWinWdmPin* pin = pInfo->stream->renderPin;
    pInfo->renderPackets[pInfo->renderTail & cPacketsArrayMask] = 0;
    /* Call barrier (if needed) */
    pin->fnMemBarrier();
    __PA_DEBUG(("Render submit (WaveRT) : submit=%u\n", pInfo->renderTail));
    ++pInfo->pending;
    if (pInfo->priming)
    {
        --pInfo->priming;
        if (pInfo->priming)
        {
            __PA_DEBUG(("Setting WaveRT event for priming (2)\n"));
            SetEvent(pInfo->stream->eventsRender[0]);
        }
    }
}

#endif /* PA_NO_WDMKS */


