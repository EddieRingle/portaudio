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
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <signal.h> /* For sig_atomic_t */

#include "portaudio.h"
#include "pa_util.h"
/*#include "../pa_unix/pa_unix_util.h"*/
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_linux_alsa.h"

#define MIN(x,y) ( (x) < (y) ? (x) : (y) )
#define MAX(x,y) ( (x) > (y) ? (x) : (y) )

/* Utilize GCC branch prediction for error tests */
#if defined __GNUC__ && __GNUC__ >= 3
#define UNLIKELY(expr) __builtin_expect( (expr), 0 )
#else
#define UNLIKELY(expr) (expr)
#endif

#define STRINGIZE_HELPER(expr) #expr
#define STRINGIZE(expr) STRINGIZE_HELPER(expr)

/* Check return value of ALSA function, and map it to PaError */
#define ENSURE(expr, code) \
    if( UNLIKELY( (aErr_ = (expr)) < 0 ) ) \
    { \
        /* PaUtil_SetLastHostErrorInfo should only be used in the main thread */ \
        if( (code) == paUnanticipatedHostError && pthread_self() == mainThread_ ) \
        { \
            PaUtil_SetLastHostErrorInfo( paALSA, aErr_, snd_strerror( aErr_ ) ); \
        } \
        PaUtil_DebugPrint(( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = (code); \
        goto error; \
    }

/* Check PaError */
#define ENSURE_PA(expr) \
    if( UNLIKELY( (paErr_ = (expr)) < paNoError ) ) \
    { \
        PaUtil_DebugPrint(( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = paErr_; \
        goto error; \
    }

#define UNLESS(expr, code) \
    if( UNLIKELY( (expr) == 0 ) ) \
    { \
        PaUtil_DebugPrint(( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = (code); \
        goto error; \
    }

#define ASSERT_CALL(expr, success) \
    aErr_ = (expr); \
    assert( aErr_ == success );

static int aErr_;               /* Used with ENSURE */
static PaError paErr_;          /* Used with ENSURE_PA */
static pthread_t mainThread_;

typedef enum
{
    streamModeIn,
    streamModeOut
} StreamMode;

/* Threading utility struct */
typedef struct PaAlsaThreading
{
    pthread_t watchdogThread;
    pthread_t callbackThread;
    int watchdogRunning;
    int rtSched;
    int rtPrio;
    int useWatchdog;
    unsigned long throttledSleepTime;
    volatile PaTime callbackTime;
    volatile PaTime callbackCpuTime;
    PaUtilCpuLoadMeasurer *cpuLoadMeasurer;
} PaAlsaThreading;

/* Implementation specific stream structure */
typedef struct PaAlsaStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;
    PaAlsaThreading threading;

    snd_pcm_t *pcm_capture;
    snd_pcm_t *pcm_playback;

    snd_pcm_uframes_t frames_per_period;
    snd_pcm_uframes_t playbackBufferSize;
    snd_pcm_uframes_t captureBufferSize;
    snd_pcm_format_t captureNativeFormat;
    snd_pcm_format_t playbackNativeFormat;
    snd_pcm_uframes_t startThreshold;

    int captureUserChannels;
    int captureHostChannels;
    int playbackUserChannels;
    int playbackHostChannels;

    int capture_interleaved;    /* bool: is capture interleaved? */
    int playback_interleaved;   /* bool: is playback interleaved? */

    int callback_mode;              /* bool: are we running in callback mode? */
    int pcmsSynced;	            /* Have we successfully synced pcms */

    /* the callback thread uses these to poll the sound device(s), waiting
     * for data to be ready/available */
    unsigned int capture_nfds;
    unsigned int playback_nfds;
    struct pollfd *pfds;
    int pollTimeout;

    /* Used in communication between threads */
    volatile sig_atomic_t callback_finished; /* bool: are we in the "callback finished" state? */
    volatile sig_atomic_t callbackAbort;    /* Drop frames? */
    volatile sig_atomic_t isActive;         /* Is stream in active state? (Between StartStream and StopStream || !paContinue) */
    pthread_mutex_t stateMtx;               /* Used to synchronize access to stream state */
    pthread_mutex_t startMtx;               /* Used to synchronize stream start in callback mode */
    pthread_cond_t startCond;               /* Wait untill audio is started in callback thread */

    /* Used by callback thread for underflow/overflow handling */
    snd_pcm_sframes_t playbackAvail;
    snd_pcm_sframes_t captureAvail;
    int neverDropInput;

    PaTime underrun;
    PaTime overrun;
}
PaAlsaStream;

/* PaAlsaHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct PaAlsaHostApiRepresentation
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
    char *alsaName;
    int isPlug;
    int minInputChannels;
    int minOutputChannels;
}
PaAlsaDeviceInfo;

/* Threading utilities */

static void InitializeThreading( PaAlsaThreading *th, PaUtilCpuLoadMeasurer *clm )
{
    th->watchdogRunning = 0;
    th->rtSched = 0;
    th->callbackTime = 0;
    th->callbackCpuTime = 0;
    th->useWatchdog = 1;
    th->throttledSleepTime = 0;
    th->cpuLoadMeasurer = clm;

    th->rtPrio = (sched_get_priority_max( SCHED_FIFO ) - sched_get_priority_min( SCHED_FIFO )) / 2
            + sched_get_priority_min( SCHED_FIFO );
}

static PaError KillCallbackThread( PaAlsaThreading *th, PaError *exitResult, PaError *watchdogExitResult )
{
    PaError result = paNoError;
    void *pret;

    if( exitResult )
        *exitResult = paNoError;
    if( watchdogExitResult )
        *watchdogExitResult = paNoError;

    if( th->watchdogRunning )
    {
        pthread_cancel( th->watchdogThread );
        ASSERT_CALL( pthread_join( th->watchdogThread, &pret ), 0 );

        if( pret && pret != PTHREAD_CANCELED )
        {
            if( watchdogExitResult )
                *watchdogExitResult = *(PaError *) pret;
            free( pret );
        }
    }

    pthread_cancel( th->callbackThread );   /* XXX: Safe to call this if the thread has exited on its own? */
    ASSERT_CALL( pthread_join( th->callbackThread, &pret ), 0 );

    if( pret && pret != PTHREAD_CANCELED )
    {
        if( exitResult )
            *exitResult = *(PaError *) pret;
        free( pret );
    }

    return result;
}

static void OnWatchdogExit( void *userData )
{
    PaAlsaThreading *th = (PaAlsaThreading *) userData;
    struct sched_param spm = { 0 };
    assert( th );

    ASSERT_CALL( pthread_setschedparam( th->callbackThread, SCHED_OTHER, &spm ), 0 );    /* Lower before exiting */
    PA_DEBUG(( "Watchdog exiting\n" ));
}

static PaError BoostPriority( PaAlsaThreading *th )
{
    PaError result = paNoError;
    struct sched_param spm = { 0 };
    spm.sched_priority = th->rtPrio;

    assert( th );

    if( pthread_setschedparam( th->callbackThread, SCHED_FIFO, &spm ) != 0 )
    {
        UNLESS( errno == EPERM, paInternalError );  /* Lack permission to raise priority */
        PA_DEBUG(( "Failed bumping priority\n" ));
        result = 0;
    }
    else
        result = 1; /* Success */
error:
    return result;
}

static void *WatchdogFunc( void *userData )
{
    PaError result = paNoError, *pres = NULL;
    int err;
    PaAlsaThreading *th = (PaAlsaThreading *) userData;
    unsigned intervalMsec = 500;
    const PaTime maxSeconds = 3.;   /* Max seconds between callbacks */
    PaTime timeThen = PaUtil_GetTime(), timeNow, timeElapsed, cpuTimeThen, cpuTimeNow, cpuTimeElapsed;
    double cpuLoad, avgCpuLoad = 0.;
    int throttled = 0;

    assert( th );

    pthread_cleanup_push( &OnWatchdogExit, th );	/* Execute OnWatchdogExit when exiting */

    /* Boost priority of callback thread */
    ENSURE_PA( result = BoostPriority( th ) );
    if( !result )
    {
        pthread_exit( NULL );   /* Boost failed, might as well exit */
    }

    cpuTimeThen = th->callbackCpuTime;
    {
        int policy;
        struct sched_param spm = { 0 };
        pthread_getschedparam( pthread_self(), &policy, &spm );
        PA_DEBUG(( "%s: Watchdog priority is %d\n", __FUNCTION__, spm.sched_priority ));
    }

    while( 1 )
    {
        double lowpassCoeff = 0.9, lowpassCoeff1 = 0.99999 - lowpassCoeff;
        
        /* Test before and after in case whatever underlying sleep call isn't interrupted by pthread_cancel */
        pthread_testcancel();
        Pa_Sleep( intervalMsec );
        pthread_testcancel();

        if( PaUtil_GetTime() - th->callbackTime > maxSeconds )
        {
            PA_DEBUG(( "Watchdog: Terminating callback thread\n" ));
            /* Tell thread to terminate */
            err = pthread_kill( th->callbackThread, SIGKILL );
            pthread_exit( NULL );
        }

        PA_DEBUG(( "%s: PortAudio reports CPU load: %g\n", __FUNCTION__, PaUtil_GetCpuLoad( th->cpuLoadMeasurer ) ));

        /* Check if we should throttle, or unthrottle :P */
        cpuTimeNow = th->callbackCpuTime;
        cpuTimeElapsed = cpuTimeNow - cpuTimeThen;
        cpuTimeThen = cpuTimeNow;

        timeNow = PaUtil_GetTime();
        timeElapsed = timeNow - timeThen;
        timeThen = timeNow;
        cpuLoad = cpuTimeElapsed / timeElapsed;
        avgCpuLoad = avgCpuLoad * lowpassCoeff + cpuLoad * lowpassCoeff1;
        /*
        if( throttled )
            PA_DEBUG(( "Watchdog: CPU load: %g, %g\n", avgCpuLoad, cpuTimeElapsed ));
            */
        if( PaUtil_GetCpuLoad( th->cpuLoadMeasurer ) > .925 )
        {
            static int policy;
            static struct sched_param spm = { 0 };
            static const struct sched_param defaultSpm = { 0 };
            PA_DEBUG(( "%s: Throttling audio thread, priority %d\n", __FUNCTION__, spm.sched_priority ));

            pthread_getschedparam( th->callbackThread, &policy, &spm );
            if( !pthread_setschedparam( th->callbackThread, SCHED_OTHER, &defaultSpm ) )
            {
                throttled = 1;
            }
            else
                PA_DEBUG(( "Watchdog: Couldn't lower priority of audio thread: %s\n", strerror( errno ) ));

            /* Give other processes a go, before raising priority again */
            PA_DEBUG(( "%s: Watchdog sleeping for %lu msecs before unthrottling\n", __FUNCTION__, th->throttledSleepTime ));
            Pa_Sleep( th->throttledSleepTime );

            /* Reset callback priority */
            if( pthread_setschedparam( th->callbackThread, SCHED_FIFO, &spm ) != 0 )
            {
                PA_DEBUG(( "%s: Couldn't raise priority of audio thread: %s\n", __FUNCTION__, strerror( errno ) ));
            }

            if( PaUtil_GetCpuLoad( th->cpuLoadMeasurer ) >= .99 )
                intervalMsec = 50;
            else
                intervalMsec = 100;

            /*
            lowpassCoeff = .97;
            lowpassCoeff1 = .99999 - lowpassCoeff;
            */
        }
        else if( throttled && avgCpuLoad < .8 )
        {
            intervalMsec = 500;
            throttled = 0;

            /*
            lowpassCoeff = .9;
            lowpassCoeff1 = .99999 - lowpassCoeff;
            */
        }
    }

    pthread_cleanup_pop( 1 );   /* Execute cleanup on exit */

error:
    /* Shouldn't get here in the normal case */

    /* Pass on error code */
    pres = malloc( sizeof (PaError) );
    *pres = result;
    
    pthread_exit( pres );
}

static PaError CreateCallbackThread( PaAlsaThreading *th, void *(*CallbackThreadFunc)( void * ), PaStream *s )
{
    PaError result = paNoError;
    pthread_attr_t attr;
    int started = 0;

#if defined _POSIX_MEMLOCK && (_POSIX_MEMLOCK != -1)
    if( th->rtSched )
    {
        if( mlockall( MCL_CURRENT | MCL_FUTURE ) < 0 )
        {
            int savedErrno = errno;             /* In case errno gets overwritten */
            assert( savedErrno != EINVAL );     /* Most likely a programmer error */
            UNLESS( (savedErrno == EPERM), paInternalError );
            PA_DEBUG(( "%s: Failed locking memory\n", __FUNCTION__ ));
        }
        else
            PA_DEBUG(( "%s: Successfully locked memory\n", __FUNCTION__ ));
    }
#endif

    UNLESS( !pthread_attr_init( &attr ), paInternalError );
    /* Priority relative to other processes */
    UNLESS( !pthread_attr_setscope( &attr, PTHREAD_SCOPE_SYSTEM ), paInternalError );   

    UNLESS( !pthread_create( &th->callbackThread, &attr, CallbackThreadFunc, s ), paInternalError );
    started = 1;

    if( th->rtSched )
    {
        if( th->useWatchdog )
        {
            int err;
            struct sched_param wdSpm = { 0 };
            /* Launch watchdog, watchdog sets callback thread priority */
            int prio = MIN( th->rtPrio + 4, sched_get_priority_max( SCHED_FIFO ) );
            wdSpm.sched_priority = prio;

            UNLESS( !pthread_attr_init( &attr ), paInternalError );
            UNLESS( !pthread_attr_setinheritsched( &attr, PTHREAD_EXPLICIT_SCHED ), paInternalError );
            UNLESS( !pthread_attr_setscope( &attr, PTHREAD_SCOPE_SYSTEM ), paInternalError );
            UNLESS( !pthread_attr_setschedpolicy( &attr, SCHED_FIFO ), paInternalError );
            UNLESS( !pthread_attr_setschedparam( &attr, &wdSpm ), paInternalError );
            if( (err = pthread_create( &th->watchdogThread, &attr, &WatchdogFunc, th )) )
            {
                UNLESS( err == EPERM, paInternalError );
                /* Permission error, go on without realtime privileges */
                PA_DEBUG(( "Failed bumping priority\n" ));
            }
            else
            {
                int policy;
                th->watchdogRunning = 1;
                ASSERT_CALL( pthread_getschedparam( th->watchdogThread, &policy, &wdSpm ), 0 );
                /* Check if priority is right, policy could potentially differ from SCHED_FIFO (but that's alright) */
                if( wdSpm.sched_priority != prio )
                {
                    PA_DEBUG(( "Watchdog priority not set correctly (%d)\n", wdSpm.sched_priority ));
                    ENSURE_PA( paInternalError );
                }
            }
        }
        else
            ENSURE_PA( BoostPriority( th ) );
    }

end:
    return result;
error:
    if( started )
        KillCallbackThread( th, NULL, NULL );

    goto end;
}

static void CallbackUpdate( PaAlsaThreading *th )
{
    th->callbackTime = PaUtil_GetTime();
    th->callbackCpuTime = PaUtil_GetCpuLoad( th->cpuLoadMeasurer );
}

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
static void CleanUpStream( PaAlsaStream *stream );
static int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate );
static int GetExactSampleRate( snd_pcm_hw_params_t *hwParams, double *sampleRate );

/* Callback prototypes */
static void *CallbackThreadFunc( void *userData );

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

    ENSURE_PA( BuildDeviceList( alsaHostApi ) );

    mainThread_ = pthread_self();

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
}

/*! Determine max channels and default latencies.
 *
 * This function provides functionality to grope an opened (might be opened for capture or playback) pcm device for 
 * traits like max channels, suitable default latencies and default sample rate. Upon error, max channels is set to zero,
 * and a suitable result returned. The device is closed before returning.
 */
static PaError GropeDevice( snd_pcm_t *pcm, int *minChannels, int *maxChannels, double *defaultLowLatency,
        double *defaultHighLatency, double *defaultSampleRate, int isPlug )
{
    PaError result = paNoError;
    snd_pcm_hw_params_t *hwParams;
    snd_pcm_uframes_t lowLatency = 1024, highLatency = 16384;
    unsigned int minChans, maxChans;
    double defaultSr = *defaultSampleRate;

    assert( pcm );

    ENSURE( snd_pcm_nonblock( pcm, 0 ), paUnanticipatedHostError );

    snd_pcm_hw_params_alloca( &hwParams );
    snd_pcm_hw_params_any( pcm, hwParams );

    if( defaultSr != -1. )
    {
        /* Could be that the device opened in one mode supports samplerates that the other mode wont have,
         * so try again .. */
        if( SetApproximateSampleRate( pcm, hwParams, defaultSr ) < 0 )
        {
            defaultSr = -1.;
            PA_DEBUG(( "%s: Original default samplerate failed, trying again ..\n", __FUNCTION__ ));
        }
    }

    if( defaultSr == -1. )           /* Default sample rate not set */
    {
        unsigned int sampleRate = 44100;        /* Will contain approximate rate returned by alsa-lib */
        ENSURE( snd_pcm_hw_params_set_rate_near( pcm, hwParams, &sampleRate, NULL ), paUnanticipatedHostError );
        ENSURE( GetExactSampleRate( hwParams, &defaultSr ), paUnanticipatedHostError );
    }

    ENSURE( snd_pcm_hw_params_get_channels_min( hwParams, &minChans ), paUnanticipatedHostError );
    ENSURE( snd_pcm_hw_params_get_channels_max( hwParams, &maxChans ), paUnanticipatedHostError );
    assert( maxChans <= INT_MAX );
    assert( maxChans > 0 );    /* Weird linking issue could cause wrong version of ALSA symbols to be called,
                                   resulting in zeroed values */
    maxChans = isPlug ? 128 : maxChans;   /* XXX: Limit to sensible number (ALSA plugins accept a crazy amount of channels)? */
    if( isPlug )
        PA_DEBUG(( "%s: Limiting number of plugin channels to %u\n", __FUNCTION__, maxChans ));

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

    *minChannels = (int)minChans;
    *maxChannels = (int)maxChans;
    *defaultSampleRate = defaultSr;
    *defaultLowLatency = (double) lowLatency / *defaultSampleRate;
    *defaultHighLatency = (double) highLatency / *defaultSampleRate;

end:
    snd_pcm_close( pcm );
    return result;

error:
    goto end;
}

/* Initialize device info with invalid values (maxInputChannels and maxOutputChannels are set to zero since these indicate
 * wether input/output is available) */
static void InitializeDeviceInfo( PaDeviceInfo *deviceInfo )
{
    deviceInfo->structVersion = -1;
    deviceInfo->name = NULL;
    deviceInfo->hostApi = -1;
    deviceInfo->maxInputChannels = 0;
    deviceInfo->maxOutputChannels = 0;
    deviceInfo->defaultLowInputLatency = -1.;
    deviceInfo->defaultLowOutputLatency = -1.;
    deviceInfo->defaultHighInputLatency = -1.;
    deviceInfo->defaultHighOutputLatency = -1.;
    deviceInfo->defaultSampleRate = -1.;
}

/* Helper struct */
typedef struct
{
    char *alsaName;
    char *name;
    int isPlug;
} DeviceNames;

/* Build PaDeviceInfo list, ignore devices for which we cannot determine capabilities (possibly busy, sigh) */
static PaError BuildDeviceList( PaAlsaHostApiRepresentation *alsaApi )
{
    PaUtilHostApiRepresentation *commonApi = &alsaApi->commonHostApiRep;
    PaAlsaDeviceInfo *deviceInfoArray;
    int cardIdx = -1, devIdx = 0;
    snd_ctl_t *ctl;
    snd_ctl_card_info_t *card_info;
    PaError result = paNoError;
    size_t numDeviceNames = 0, maxDeviceNames = 1, i;
    DeviceNames *deviceNames = NULL;
    snd_config_t *top;
    int res;
    int blocking = SND_PCM_NONBLOCK;
    if( getenv( "PA_ALSA_INITIALIZE_BLOCK" ) && atoi( getenv( "PA_ALSA_INITIALIZE_BLOCK" ) ) )
        blocking = 0;

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
    cardIdx = -1;
    snd_ctl_card_info_alloca( &card_info );
    while( snd_card_next( &cardIdx ) == 0 && cardIdx >= 0 )
    {
        const char *cardName;
        char *alsaDeviceName, *deviceName;

        UNLESS( alsaDeviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                        50 ), paInsufficientMemory );
        snprintf( alsaDeviceName, 50, "hw:%d", cardIdx );

        /* Acquire name of card */
        if( snd_ctl_open( &ctl, alsaDeviceName, 0 ) < 0 )
            continue;   /* Unable to open card :( */
        snd_ctl_card_info( ctl, card_info );
        snd_ctl_close( ctl );
        cardName = snd_ctl_card_info_get_name( card_info );

        UNLESS( deviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                        strlen(cardName) + 1 ), paInsufficientMemory );
        strcpy( deviceName, cardName );

        ++numDeviceNames;
        if( !deviceNames || numDeviceNames > maxDeviceNames )
        {
            maxDeviceNames *= 2;
            UNLESS( deviceNames = (DeviceNames *) realloc( deviceNames, maxDeviceNames * sizeof (DeviceNames) ),
                    paInsufficientMemory );
        }

        deviceNames[ numDeviceNames - 1 ].alsaName = alsaDeviceName;
        deviceNames[ numDeviceNames - 1 ].name = deviceName;
        deviceNames[ numDeviceNames - 1 ].isPlug = 0;
    }

    /* Iterate over plugin devices */
    if( (res = snd_config_search( snd_config, "pcm", &top )) >= 0 )
    {
        snd_config_iterator_t i, next;
        const char *s;

        snd_config_for_each( i, next, top )
        {
            char *alsaDeviceName, *deviceName;
            snd_config_t *n = snd_config_iterator_entry( i ), *tp;
            if( snd_config_get_type( n ) != SND_CONFIG_TYPE_COMPOUND )
                continue;

            /* Restrict search to nodes of type "plug" for now */
            ENSURE( snd_config_search( n, "type", &tp ), paUnanticipatedHostError );
            ENSURE( snd_config_get_string( tp, &s ), paUnanticipatedHostError );
            if( strcmp( s, "plug" ) )
                continue;

            /* Disregard standard plugins
             * XXX: Might want to make the "default" plugin available, if we can make it work
             */
            ENSURE( snd_config_get_id( n, &s ), paUnanticipatedHostError );
            if( !strcmp( s, "plughw" ) || !strcmp( s, "plug" ) || !strcmp( s, "default" ) )
                continue;

            UNLESS( alsaDeviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                            strlen(s) + 6 ), paInsufficientMemory );
            strcpy( alsaDeviceName, s );
            UNLESS( deviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                            strlen(s) + 1 ), paInsufficientMemory );
            strcpy( deviceName, s );

            ++numDeviceNames;
            if( !deviceNames || numDeviceNames > maxDeviceNames )
            {
                maxDeviceNames *= 2;
                UNLESS( deviceNames = (DeviceNames *) realloc( deviceNames, maxDeviceNames * sizeof (DeviceNames) ),
                        paInsufficientMemory );
            }

            deviceNames[numDeviceNames - 1].alsaName = alsaDeviceName;
            deviceNames[numDeviceNames - 1].name = deviceName;
            deviceNames[numDeviceNames - 1].isPlug = 1;
        }
    }
    else
        PA_DEBUG(( "%s: Iterating over ALSA plugins failed: %s\n", __FUNCTION__, snd_strerror( res ) ));

    /* allocate deviceInfo memory based on the number of devices */
    UNLESS( commonApi->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaDeviceInfo*) * (numDeviceNames) ), paInsufficientMemory );

    /* allocate all device info structs in a contiguous block */
    UNLESS( deviceInfoArray = (PaAlsaDeviceInfo*)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaAlsaDeviceInfo) * numDeviceNames ), paInsufficientMemory );

    /* Loop over list of cards, filling in info, if a device is deemed unavailable (can't get name),
     * it's ignored.
     */
    /* while( snd_card_next( &cardIdx ) == 0 && cardIdx >= 0 ) */
    for( i = 0, devIdx = 0; i < numDeviceNames; ++i )
    {
        snd_pcm_t *pcm;
        PaAlsaDeviceInfo *deviceInfo = &deviceInfoArray[devIdx];
        PaDeviceInfo *commonDeviceInfo = &deviceInfo->commonDeviceInfo;

        /* Zero fields */
        InitializeDeviceInfo( commonDeviceInfo );

        /* to determine device capabilities, we must open the device and query the
         * hardware parameter configuration space */

        /* Query capture */
        if( snd_pcm_open( &pcm, deviceNames[ i ].alsaName, SND_PCM_STREAM_CAPTURE, blocking ) >= 0 )
        {
            if( GropeDevice( pcm, &deviceInfo->minInputChannels, &commonDeviceInfo->maxInputChannels,
                        &commonDeviceInfo->defaultLowInputLatency, &commonDeviceInfo->defaultHighInputLatency,
                        &commonDeviceInfo->defaultSampleRate, deviceNames[i].isPlug ) != paNoError )
                    continue;   /* Error */
        }
                
        /* Query playback */
        if( snd_pcm_open( &pcm, deviceNames[ i ].alsaName, SND_PCM_STREAM_PLAYBACK, blocking ) >= 0 )
        {
            if( GropeDevice( pcm, &deviceInfo->minOutputChannels, &commonDeviceInfo->maxOutputChannels,
                        &commonDeviceInfo->defaultLowOutputLatency, &commonDeviceInfo->defaultHighOutputLatency,
                        &commonDeviceInfo->defaultSampleRate, deviceNames[i].isPlug ) != paNoError )
                    continue;   /* Error */
        }

        commonDeviceInfo->structVersion = 2;
        commonDeviceInfo->hostApi = alsaApi->hostApiIndex;
        commonDeviceInfo->name = deviceNames[i].name;
        deviceInfo->alsaName = deviceNames[i].alsaName;
        deviceInfo->isPlug = deviceNames[i].isPlug;

        /* A: Storing pointer to PaAlsaDeviceInfo object as pointer to PaDeviceInfo object.
         * Should now be safe to add device info, unless the device supports neither capture nor playback
         */
        if( commonDeviceInfo->maxInputChannels > 0 || commonDeviceInfo->maxOutputChannels > 0 )
        {
            if( commonApi->info.defaultInputDevice == paNoDevice && commonDeviceInfo->maxInputChannels > 0 )
                commonApi->info.defaultInputDevice = devIdx;
            if(  commonApi->info.defaultOutputDevice == paNoDevice && commonDeviceInfo->maxOutputChannels > 0 )
                commonApi->info.defaultOutputDevice = devIdx;

            commonApi->deviceInfos[devIdx++] = (PaDeviceInfo *) deviceInfo;
        }
    }
    free( deviceNames );

    commonApi->info.deviceCount = devIdx;   /* Number of successfully queried devices */

end:
    return result;

error:
    goto end;   /* No particular action */
}


/* Check against known device capabilities */
static PaError ValidateParameters( const PaStreamParameters *parameters, const PaAlsaDeviceInfo *deviceInfo, StreamMode mode,
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

    maxChans = (mode == streamModeIn ? deviceInfo->commonDeviceInfo.maxInputChannels :
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

/* Open an ALSA pcm handle.
 * 
 * The device to be open can be specified in a custom PaAlsaStreamInfo struct, or it will be a device number. In case of a
 * device number, it maybe specified through an env variable (PA_ALSA_PLUGHW) that we should open the corresponding plugin
 * device.
 */
static PaError AlsaOpen(snd_pcm_t **pcm, const PaAlsaDeviceInfo *deviceInfo, const PaAlsaStreamInfo
        *streamInfo, snd_pcm_stream_t streamType )
{
    PaError result = paNoError;
    int ret;
    const char *deviceName = alloca( 50 );

    if( !streamInfo )
    {
        int usePlug = 0;
        
        /* If device name starts with hw: and PA_ALSA_PLUGHW is 1, we open the plughw device instead */
        if( !strncmp( "hw:", deviceInfo->alsaName, 3 ) && getenv( "PA_ALSA_PLUGHW" ) )
            usePlug = atoi( getenv( "PA_ALSA_PLUGHW" ) );
        if( usePlug )
            snprintf( (char *) deviceName, 50, "plug%s", deviceInfo->alsaName );
        else
            deviceName = deviceInfo->alsaName;
    }
    else
        deviceName = streamInfo->deviceString;

    if( (ret = snd_pcm_open( pcm, deviceName, streamType, SND_PCM_NONBLOCK )) < 0 )
    {
        *pcm = NULL;     /* Not to be closed */
        ENSURE( ret, ret == -EBUSY ? paDeviceUnavailable : paBadIODeviceCombination );
    }
    ENSURE( snd_pcm_nonblock( *pcm, 0 ), paUnanticipatedHostError );

end:
    return result;

error:
    goto end;
}

static PaError TestParameters( const PaStreamParameters *parameters, const PaAlsaDeviceInfo *deviceInfo, const PaAlsaStreamInfo
        *streamInfo, double sampleRate, snd_pcm_stream_t streamType )
{
    PaError result = paNoError;
    snd_pcm_t *pcm = NULL;
    PaSampleFormat availableFormats;
    PaSampleFormat paFormat;
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca( &params );

    ENSURE_PA( AlsaOpen( &pcm, deviceInfo, streamInfo, streamType ) );

    snd_pcm_hw_params_any( pcm, params );

    ENSURE( SetApproximateSampleRate( pcm, params, sampleRate ), paInvalidSampleRate );
    ENSURE( snd_pcm_hw_params_set_channels( pcm, params, parameters->channelCount ), paInvalidChannelCount );

    /* See if we can find a best possible match */
    availableFormats = GetAvailableFormats( pcm );
    ENSURE_PA( paFormat = PaUtil_SelectClosestAvailableFormat( availableFormats, parameters->sampleFormat ) );

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
            inputDeviceInfo = (PaAlsaDeviceInfo *)hostApi->deviceInfos[inputParameters->device];
        }
        else
            inputStreamInfo = inputParameters->hostApiSpecificStreamInfo;

        ENSURE_PA( ValidateParameters( inputParameters, inputDeviceInfo, streamModeIn, inputStreamInfo ) );

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

        ENSURE_PA( ValidateParameters( outputParameters, outputDeviceInfo, streamModeOut, outputStreamInfo ) );

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
        ENSURE_PA( TestParameters( inputParameters, inputDeviceInfo, inputStreamInfo, sampleRate, SND_PCM_STREAM_CAPTURE ) );
    }

    if ( outputChannelCount )
    {
        ENSURE_PA( TestParameters( outputParameters, outputDeviceInfo, outputStreamInfo, sampleRate, SND_PCM_STREAM_PLAYBACK ) );
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
    ENSURE( snd_pcm_hw_params_set_periods_near( pcm, hwParams, &numPeriods, NULL ), paUnanticipatedHostError );

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
    if( !primeBuffers ) /* XXX: Make sense? */
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
    ASSERT_CALL( pthread_mutex_init( &stream->stateMtx, NULL ), 0 );
    ASSERT_CALL( pthread_mutex_init( &stream->startMtx, NULL ), 0 );
    ASSERT_CALL( pthread_cond_init( &stream->startCond, NULL ), 0 );
    stream->neverDropInput = streamFlags & paNeverDropInput;
    stream->underrun = stream->overrun = 0.0;

    InitializeThreading( &stream->threading, &stream->cpuLoadMeasurer );
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
    PaAlsaStream *stream = NULL;
    PaSampleFormat hostInputSampleFormat = 0, hostOutputSampleFormat = 0;
    PaSampleFormat inputSampleFormat = 0, outputSampleFormat = 0;
    int numInputChannels = 0, numOutputChannels = 0, numHostInputChannels = 0,numHostOutputChannels = 0;
    unsigned long framesPerHostBuffer = framesPerBuffer;
    PaAlsaStreamInfo *inputStreamInfo = NULL, *outputStreamInfo = NULL;
    PaTime inputLatency, outputLatency;

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */

    if( inputParameters )
    {
        if( inputParameters->device != paUseHostApiSpecificDeviceSpecification )
        {
            assert( inputParameters->device < hostApi->info.deviceCount );
            inputDeviceInfo = (PaAlsaDeviceInfo*)hostApi->deviceInfos[inputParameters->device];
        }
        else
            inputStreamInfo = inputParameters->hostApiSpecificStreamInfo;

        ENSURE_PA( ValidateParameters( inputParameters, inputDeviceInfo, streamModeIn, inputStreamInfo ) );

        numInputChannels = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
    }
    if( outputParameters )
    {
        if( outputParameters->device != paUseHostApiSpecificDeviceSpecification )
        {
            assert( outputParameters->device < hostApi->info.deviceCount );
            outputDeviceInfo = (PaAlsaDeviceInfo*)hostApi->deviceInfos[outputParameters->device];
        }
        else
            outputStreamInfo = outputParameters->hostApiSpecificStreamInfo;

        ENSURE_PA( ValidateParameters( outputParameters, outputDeviceInfo, streamModeOut, outputStreamInfo ) );

        numOutputChannels = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
    }

    /* allocate and do basic initialization of the stream structure */

    UNLESS( stream = (PaAlsaStream*)PaUtil_AllocateMemory( sizeof(PaAlsaStream) ), paInsufficientMemory );
    InitializeStream( stream, callback != NULL, streamFlags );    /* Initialize structure */

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
                                               NULL, userData );
    }
    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    /* open the devices now, so we can obtain info about the available formats */

    if( numInputChannels > 0 )
    {
        ENSURE_PA( AlsaOpen( &stream->pcm_capture, inputDeviceInfo, inputStreamInfo, SND_PCM_STREAM_CAPTURE ) );
        stream->capture_nfds = snd_pcm_poll_descriptors_count( stream->pcm_capture );
        hostInputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats( stream->pcm_capture ),
                                                 inputSampleFormat );

        stream->captureNativeFormat = Pa2AlsaFormat( hostInputSampleFormat ); /* Used when silencing buffer */
    }

    if( numOutputChannels > 0 )
    {
        ENSURE_PA( AlsaOpen( &stream->pcm_playback, outputDeviceInfo, outputStreamInfo, SND_PCM_STREAM_PLAYBACK ) );
        stream->playback_nfds = snd_pcm_poll_descriptors_count( stream->pcm_playback );
        hostOutputSampleFormat =
            PaUtil_SelectClosestAvailableFormat( GetAvailableFormats( stream->pcm_playback ),
                                                 outputSampleFormat );
        stream->playbackNativeFormat = Pa2AlsaFormat( hostOutputSampleFormat ); /* Used when silencing buffer */
    }

    /* If the number of frames per buffer is unspecified, we have to come up with
     * one.  This is both a blessing and a curse: a blessing because we can optimize
     * the number to best meet the requirements, but a curse because that's really
     * hard to do well.  For this reason we also support an interface where the user
     * specifies these by setting environment variables. */
    if( framesPerBuffer == paFramesPerBufferUnspecified )
    {
        if( getenv("PA_ALSA_PERIODSIZE") != NULL )
            framesPerHostBuffer = atoi( getenv("PA_ALSA_PERIODSIZE") );
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

                if( optimalPeriodSize > periodSize )
                    periodSize = optimalPeriodSize;

                if( periodSize <= maxPeriodSize )
                {
                    /* Looks good */
                    framesPerHostBuffer = periodSize;
                }
                else    /* XXX: Some more descriptive error code might be appropriate */
                    ENSURE_PA( paBadIODeviceCombination );
            }
            else
            {
                /* half-duplex is a slightly simpler case */
                unsigned long bufferSize, channels;
                snd_pcm_t *pcm;
                snd_pcm_hw_params_t *hwParams;

                snd_pcm_hw_params_alloca( &hwParams );

                if( stream->pcm_capture )
                {
                    pcm = stream->pcm_capture;
                    bufferSize = inputParameters->suggestedLatency * sampleRate;
                    channels = inputParameters->channelCount;
                }
                else
                {
                    pcm = stream->pcm_playback;
                    bufferSize = outputParameters->suggestedLatency * sampleRate;
                    channels = outputParameters->channelCount;
                }

                ENSURE( snd_pcm_hw_params_any( pcm, hwParams ), paUnanticipatedHostError );
                ENSURE( SetApproximateSampleRate( pcm, hwParams, sampleRate ), paBadIODeviceCombination );
                ENSURE( snd_pcm_hw_params_set_channels( pcm, hwParams, channels ), paBadIODeviceCombination );

                ENSURE( snd_pcm_hw_params_set_period_size_integer( pcm, hwParams ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_set_periods_integer( pcm, hwParams ), paUnanticipatedHostError );

                /* Using 5 as a base number of periods, we try to approximate the suggested latency (+1 period),
                   finding a combination of period/buffer size which best fits these constraints */
                framesPerHostBuffer = bufferSize / 4;
                bufferSize += framesPerHostBuffer;   /* One period doesn't count as latency */
                ENSURE( snd_pcm_hw_params_set_buffer_size_near( pcm, hwParams, &bufferSize ), paUnanticipatedHostError );
                ENSURE( snd_pcm_hw_params_set_period_size_near( pcm, hwParams, &framesPerHostBuffer, NULL ), paUnanticipatedHostError );
            }
        }
    }
    else
    {
        framesPerHostBuffer = framesPerBuffer;
    }

    if( numInputChannels > 0 )
    {
        int interleaved = !(inputSampleFormat & paNonInterleaved);
        PaSampleFormat plain_format = hostInputSampleFormat & ~paNonInterleaved;

        inputLatency = inputParameters->suggestedLatency; /* Real latency in seconds returned from ConfigureStream */

        numHostInputChannels = MAX( numInputChannels, inputDeviceInfo->minInputChannels );
        ENSURE_PA( ConfigureStream( stream->pcm_capture, numInputChannels, &interleaved,
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

        numHostOutputChannels = MAX( numOutputChannels, outputDeviceInfo->minOutputChannels );
        ENSURE_PA( ConfigureStream( stream->pcm_playback, numHostOutputChannels, &interleaved,
                             &sampleRate, plain_format, framesPerHostBuffer, &stream->playbackBufferSize,
                             &outputLatency, primeBuffers, stream->callback_mode ) );

        /* If the user wants to prime the buffer before stream start, the start threshold will equal buffer size */
        if( primeBuffers )
            stream->startThreshold = stream->playbackBufferSize;
        stream->playback_interleaved = interleaved;
    }
    /* Should be exact now */
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;
    /* Time before watchdog unthrottles realtime thread == 1/4 of period time in msecs */
    stream->threading.throttledSleepTime = (unsigned long) (framesPerHostBuffer / sampleRate / 4 * 1000);

    ENSURE_PA( PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
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
    stream->captureUserChannels = numInputChannels;
    stream->captureHostChannels = numHostInputChannels;
    stream->playbackUserChannels = numOutputChannels;
    stream->playbackHostChannels = numHostOutputChannels;
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
    snd_pcm_uframes_t frames = snd_pcm_avail_update( stream->pcm_playback ), offset;

    snd_pcm_mmap_begin( stream->pcm_playback, &areas, &offset, &frames );
    snd_pcm_areas_silence( areas, offset, stream->playbackHostChannels, frames, stream->playbackNativeFormat );
    snd_pcm_mmap_commit( stream->pcm_playback, offset, frames );
}

/*! Start/prepare pcm(s) for streaming.
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
    return result;
error:
    goto end;
}

/*! Utility function for determining if pcms are in running state.
 */
static int IsRunning( PaAlsaStream *stream )
{
    int result = 0;

    ASSERT_CALL( pthread_mutex_lock( &stream->stateMtx ), 0 ); /* Synchronize access to pcm state */
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
    ASSERT_CALL( pthread_mutex_unlock( &stream->stateMtx ), 0 );

    return result;
}

static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*)s;
    int streamStarted = 0;  /* So we can know wether we need to take the stream down */

    /* Ready the processor */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    /* Set now, so we can test for activity further down */
    stream->isActive = 1;

    if( stream->callback_mode )
    {
        int res = 0;
        PaTime pt = PaUtil_GetTime();
        struct timespec ts;

        ENSURE_PA( CreateCallbackThread( &stream->threading, &CallbackThreadFunc, stream ) );
        streamStarted = 1;

        /* Wait for stream to be started */
        ts.tv_sec = (time_t) floor( pt + 1 );
        ts.tv_nsec = (long) ((pt - floor( pt )) * 1000000000);

        /* Since we'll be holding a lock on the startMtx (when not waiting on the condition), IsRunning won't be checking
         * stream state at the same time as the callback thread affects it. We also check IsStreamActive, in the unlikely
         * case the callback thread exits in the meantime (the stream will be considered inactive after the thread exits) */
        ASSERT_CALL( pthread_mutex_lock( &stream->startMtx ), 0 );
        while( !IsRunning( stream ) && IsStreamActive( s ) && !res )    /* Due to possible spurious wakeups, we enclose in a loop */
        {
            res = pthread_cond_timedwait( &stream->startCond, &stream->startMtx, &ts );
        }
        ASSERT_CALL( pthread_mutex_unlock( &stream->startMtx ), 0 );

        UNLESS( !res || res == ETIMEDOUT, paInternalError );
        PA_DEBUG(( "%s: Waited for %g seconds for stream to start\n", __FUNCTION__, PaUtil_GetTime() - pt ));

        if( res == ETIMEDOUT )
        {
            ENSURE_PA( paTimedOut );
        }
    }
    else
    {
        ENSURE_PA( AlsaStart( stream, 0 ) );
        streamStarted = 1;
    }

end:
    return result;
error:
    if( streamStarted )
        AbortStream( stream );
    stream->isActive = 0;
    
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

/*! Stop or abort stream.
 *
 * If a stream is in callback mode we will have to inspect wether the background thread has
 * finished, or we will have to take it out. In either case we join the thread before
 * returning. In blocking mode, we simply tell ALSA to stop abruptly (abort) or finish
 * buffers (drain)
 *
 * Stream will be considered inactive (!PaAlsaStream::isActive) after a call to this function
 */
static PaError RealStop( PaAlsaStream *stream, int abort )
{
    PaError result = paNoError;

    /* First deal with the callback thread, cancelling and/or joining
     * it if necessary
     */
    if( stream->callback_mode )
    {
        PaError threadRes, watchdogRes;
        stream->callbackAbort = abort;

        ENSURE_PA( KillCallbackThread( &stream->threading, &threadRes, &watchdogRes ) );
        if( threadRes != paNoError )
            PA_DEBUG(( "Callback thread returned: %d\n", threadRes ));
        if( watchdogRes != paNoError )
            PA_DEBUG(( "Watchdog thread returned: %d\n", watchdogRes ));

        stream->callback_finished = 0;
    }
    else
    {
        ENSURE_PA( AlsaStop( stream, abort ) );
    }

    stream->isActive = 0;

end:
    return result;

error:
    goto end;
}

static PaError StopStream( PaStream *s )
{
    return RealStop( (PaAlsaStream *) s, 0 );
}

static PaError AbortStream( PaStream *s )
{
    return RealStop( (PaAlsaStream * ) s, 1 );
}

/*! The stream is considered stopped before StartStream, or AFTER a call to Abort/StopStream (callback
 * returning !paContinue is not considered)
 */
static PaError IsStreamStopped( PaStream *s )
{
    PaAlsaStream *stream = (PaAlsaStream *)s;

    /* callback_finished indicates we need to join callback thread (ie. in Abort/StopStream) */
    return !IsStreamActive( s ) && !stream->callback_finished;
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
     * libasound routines are being called from multiple threads.
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
    return timestamp.tv_sec + (PaTime)timestamp.tv_usec / 1000000.0;
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}

/*! Free resources associated with stream, and eventually stream itself.
 *
 * Frees allocated memory, and closes opened pcms.
 */
static void CleanUpStream( PaAlsaStream *stream )
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
    ASSERT_CALL( pthread_mutex_destroy( &stream->stateMtx ), 0 );
    ASSERT_CALL( pthread_mutex_destroy( &stream->startMtx ), 0 );
    ASSERT_CALL( pthread_cond_destroy( &stream->startCond ), 0 );

    PaUtil_FreeMemory( stream );
}

static int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate )
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
static int GetExactSampleRate( snd_pcm_hw_params_t *hwParams, double *sampleRate )
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

    ASSERT_CALL( pthread_mutex_lock( &stream->stateMtx ), 0 );
    ENSURE_PA( AlsaStop( stream, 0 ) );
    ENSURE_PA( AlsaStart( stream, 0 ) );

    PA_DEBUG(( "%s: Restarted audio\n", __FUNCTION__ ));

end:
    ASSERT_CALL( pthread_mutex_unlock( &stream->stateMtx ), 0 );
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

    ENSURE_PA( AlsaRestart( stream ) );

end:
    return result;
error:
    goto end;
}

/*! Poll on I/O filedescriptors.

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
        int pfdOfs = 0;

        /* get the fds, packing all applicable fds into a single array,
         * so we can check them all with a single poll() call 
         */
        if( stream->pcm_capture && pollCapture )
        {
            snd_pcm_poll_descriptors( stream->pcm_capture, stream->pfds, stream->capture_nfds );
            pfdOfs += stream->capture_nfds;
            totalFds += stream->capture_nfds;
        }
        if( stream->pcm_playback && pollPlayback )
        {
            snd_pcm_poll_descriptors( stream->pcm_playback, stream->pfds + pfdOfs, stream->playback_nfds );
            totalFds += stream->playback_nfds;
        }

        /* if the main thread has requested that we stop, do so now */
        pthread_testcancel();

        /* now poll on the combination of playback and capture fds. */
        if( poll( stream->pfds, totalFds, stream->pollTimeout ) < 0 )
        {
            if( errno == EINTR ) {
                continue;
            }

            ENSURE_PA( paInternalError );
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
            else if( stream->pcm_playback ) /* Timed out, go on with playback? */ 
            {
                /* Less than 1 written period left? */
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
                        pfdOfs, stream->playback_nfds, &revents ), paUnanticipatedHostError );
            if( revents )
            {
                if( revents & POLLERR )
                    xrun = 1;

                pollPlayback = 0;
            }
            else if( stream->pcm_capture )  /* Timed out, go on with capture? */
            {
                /* Less than 1 empty period left? */
                if( snd_pcm_avail_update( stream->pcm_capture ) >= stream->captureBufferSize - stream->frames_per_period )
                {
                    PA_DEBUG(( "Playback timed out\n" ));
                    pollPlayback = 0;   /* Go on without me, son .. */
                }
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

        playbackAvail = playbackAvail == 0 ? INT_MAX : playbackAvail;   /* Disregard if zero */
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

/* Set up channels for capture or playback */
static PaError SetChannels( PaAlsaStream *stream, snd_pcm_t *pcm, int userChans, int hostChans, int interleaved,
        snd_pcm_format_t format, void (*setChannel)(PaUtilBufferProcessor *, unsigned int, void *, unsigned int),
        snd_pcm_uframes_t *frames, snd_pcm_uframes_t *offset )
{
    PaError result = paNoError;
    long unusedChans;
    const snd_pcm_channel_area_t *areas, *area;
    unsigned char *buffer, *p;
    int i;

    unusedChans = hostChans - userChans;
    assert( unusedChans >= 0 );
    
    ENSURE( snd_pcm_mmap_begin( pcm, &areas, offset, frames ), paUnanticipatedHostError );

    if( interleaved )
    {
        int swidth = snd_pcm_format_size( format, 1 );
        p = buffer = ExtractAddress( areas, *offset );
        for( i = 0; i < userChans; ++i )
        {
            /* We're setting the channels < userChans, but the stride will be hostChans samples */
            setChannel( &stream->bufferProcessor, i, p, hostChans );
            p += swidth;
        }

        /* The number of host channels doesn't necessarily equal the number of user channels */
        if( unusedChans > 0 && pcm == stream->pcm_playback )  /* Silence unused output channels */
        {
            p = buffer + userChans * swidth;
            for( i = 0; i < *frames; ++i )
            {
                memset( p, 0, swidth * unusedChans );
                p += hostChans * swidth;
            }
        }
    }
    else
    {
        /* noninterleaved */
        for( i = 0; i < userChans; ++i )
        {
            area = areas + i;
            buffer = ExtractAddress( area, *offset );
            setChannel( &stream->bufferProcessor, i, buffer, 1 );
        }

        /* The number of host channels doesn't necessarily equal the number of user channels */
        if( unusedChans > 0 && pcm == stream->pcm_playback )  /* Silence unused output channels */
            snd_pcm_areas_silence( areas + userChans, *offset, unusedChans, *frames,
                    format );
    }
    
error:
    return result;
}

/*! Get buffers from ALSA for read/write, and determine the amount of frames available.
 *
 * Request (up to) requested number of frames from ALSA, for opened pcms. The number of frames returned
 * will normally be the lowest availble (possibly aligned) of available capture and playback frames.
 * Underflow/underflow complicates matters however; if we are out of capture frames we will go on with
 * output, input overflow will either result in discarded frames or we will deliver them (paNeverDropInput).
*/
static PaError SetUpBuffers( PaAlsaStream *stream, snd_pcm_uframes_t requested, int alignFrames,
        snd_pcm_uframes_t *frames, snd_pcm_uframes_t *captureOffset, snd_pcm_uframes_t *playbackOffset )
{
    PaError result = paNoError;
    snd_pcm_uframes_t captureFrames = requested, playbackFrames = requested, commonFrames;

    assert( stream && frames );

    if( stream->pcm_capture )
    {
        assert( captureOffset );
        ENSURE_PA( SetChannels( stream, stream->pcm_capture, stream->captureUserChannels, stream->captureHostChannels,
                    stream->capture_interleaved, stream->captureNativeFormat, PaUtil_SetInputChannel,
                    &captureFrames, captureOffset ) );
    }
    if( stream->pcm_playback )
    {
        assert( playbackOffset );
        ENSURE_PA( SetChannels( stream, stream->pcm_playback, stream->playbackUserChannels, stream->playbackHostChannels,
                    stream->playback_interleaved, stream->playbackNativeFormat, PaUtil_SetOutputChannel,
                    &playbackFrames, playbackOffset ) );
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
         * If we're out of input, go on. Input buffer will be zeroed.
         * In the case of output underflow, drop input frames unless stream->neverDropInput.
         * If we're starved for output, while keeping input, we'll discard output samples.
         */
        if( !commonFrames ) 
        {
            if( !captureFrames )    /* Input underflow */
                commonFrames = playbackFrames;  /* We still want output */
            else if( stream->neverDropInput )    /* Output underflow, but do not drop input */
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
        if( captureFrames || !commonFrames )    /* We have input, or neither */
            PaUtil_SetInputFrameCount( &stream->bufferProcessor, commonFrames );
        else    /* We have input underflow */
            PaUtil_SetNoInput( &stream->bufferProcessor );
    }
    if( stream->pcm_playback )
    {
        if( playbackFrames || !commonFrames )   /* We have output, or neither */
            PaUtil_SetOutputFrameCount( &stream->bufferProcessor, commonFrames );
        else    /* We have output underflow, but keeping input data (paNeverDropInput) */
        {
            PaUtil_SetNoOutput( &stream->bufferProcessor );
        }
    }

    /* PA_DEBUG(( "SetUpBuffers: captureAvail: %d, playbackAvail: %d, commonFrames: %d\n\n", captureFrames, playbackFrames, commonFrames )); */
    /* Either of these could be zero, otherwise equal to commonFrames */
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

    PaUtil_ResetCpuLoadMeasurer( &stream->cpuLoadMeasurer );

    stream->callback_finished = 1;  /* Let the outside world know stream was stopped in callback */
    AlsaStop( stream, stream->callbackAbort );
    stream->callbackAbort = 0;      /* Clear state */
    
    PA_DEBUG(( "OnExit: Stoppage\n" ));

    /* Eventually notify user all buffers have played */
    if( stream->streamRepresentation.streamFinishedCallback )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
    stream->isActive = 0;
}

static void CalculateTimeInfo( PaAlsaStream *stream, PaStreamCallbackTimeInfo *timeInfo )
{
    snd_pcm_status_t *capture_status, *playback_status;
    snd_timestamp_t capture_timestamp, playback_timestamp;
    PaTime capture_time = 0., playback_time = 0.;

    snd_pcm_status_alloca( &capture_status );
    snd_pcm_status_alloca( &playback_status );

    if( stream->pcm_capture )
    {
        snd_pcm_sframes_t capture_delay;

        snd_pcm_status( stream->pcm_capture, capture_status );
        snd_pcm_status_get_tstamp( capture_status, &capture_timestamp );

        capture_time = capture_timestamp.tv_sec +
            ((PaTime)capture_timestamp.tv_usec / 1000000.0);
        timeInfo->currentTime = capture_time;

        capture_delay = snd_pcm_status_get_delay( capture_status );
        timeInfo->inputBufferAdcTime = timeInfo->currentTime -
            (PaTime)capture_delay / stream->streamRepresentation.streamInfo.sampleRate;
    }
    if( stream->pcm_playback )
    {
        snd_pcm_sframes_t playback_delay;

        snd_pcm_status( stream->pcm_playback, playback_status );
        snd_pcm_status_get_tstamp( playback_status, &playback_timestamp );

        playback_time = playback_timestamp.tv_sec +
            ((PaTime)playback_timestamp.tv_usec / 1000000.0);

        if( stream->pcm_capture ) /* Full duplex */
        {
            /* Hmm, we have both a playback and a capture timestamp.
             * Hopefully they are the same... */
            if( fabs( capture_time - playback_time ) > 0.01 )
                PA_DEBUG(("Capture time and playback time differ by %f\n", fabs(capture_time-playback_time)));
        }
        else
            timeInfo->currentTime = playback_time;

        playback_delay = snd_pcm_status_get_delay( playback_status );
        timeInfo->outputBufferDacTime = timeInfo->currentTime +
            (PaTime)playback_delay / stream->streamRepresentation.streamInfo.sampleRate;
    }
}

/* Callback thread's function.
 *
 * Roughly, the workflow consists of waiting untill ALSA reports available frames, and then consuming these frames in an inner loop
 * till we must wait for more. If the inner loop detects an xrun condition however, the data consumption will stop and we go
 * back to the waiting state.
 */
static void *CallbackThreadFunc( void *userData )
{
    PaError result = paNoError, *pres = NULL;
    PaAlsaStream *stream = (PaAlsaStream*) userData;
    snd_pcm_uframes_t framesAvail, framesGot, framesProcessed;
    snd_pcm_sframes_t startThreshold = stream->startThreshold;
    snd_pcm_uframes_t captureOffset, playbackOffset;
    PaStreamCallbackTimeInfo timeInfo = {0,0,0};
    PaStreamCallbackFlags cbFlags = 0;  /* We might want to keep state across iterations */

    assert( userData );

    pthread_cleanup_push( &OnExit, stream );	/* Execute OnExit when exiting */

    if( startThreshold <= 0 )
    {
        ASSERT_CALL( pthread_mutex_lock( &stream->startMtx ), 0 );
        ENSURE_PA( AlsaStart( stream, 0 ) );    /* Buffer will be zeroed */
        ASSERT_CALL( pthread_cond_signal( &stream->startCond ), 0 );
        ASSERT_CALL( pthread_mutex_unlock( &stream->startMtx ), 0 );
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
        pthread_testcancel();

        ENSURE_PA( Wait( stream, &framesAvail ) );  /* Wait on available frames */
        /* Set callback flags after one of these has been detected (in Wait()) */
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

        /* Consume available frames */
        while( framesAvail > 0 )
        {
            PaError callbackResult = paContinue;

            pthread_testcancel();

            /* Priming output */
            if( startThreshold > 0 )
            {
                PA_DEBUG(( "CallbackThreadFunc: Priming\n" ));
                cbFlags |= paPrimingOutput;
                framesAvail = MIN( framesAvail, startThreshold );
            }

            /* now we know the soundcard is ready to produce/receive at least
             * one period.  we just need to get the buffers for the client
             * to read/write. */
            ENSURE_PA( SetUpBuffers( stream, framesAvail, 1, &framesGot, &captureOffset, &playbackOffset ) );
            /* Check for under/overflow */
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

            CalculateTimeInfo( stream, &timeInfo );
            PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, cbFlags );

            PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

            CallbackUpdate( &stream->threading );   /* Report to watchdog */

            /* this calls the callback */
            framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                                          &callbackResult );
            cbFlags = 0;    /* Reset callback flags now that they should be received by the callback */
            PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
            assert( framesProcessed == framesGot ); /* These should be the same, since we don't use block adaption */

            /* Inform ALSA how many frames we read/wrote
               Now, this number may differ between capture and playback, due to under/overflow.
               If we're dropping input frames, we effectively sink them here.
             */
            if( stream->pcm_capture )
            {
                int res = snd_pcm_mmap_commit( stream->pcm_capture, captureOffset, MIN( stream->captureAvail,
                            framesProcessed ) );

                /* Non-fatal error? Terminate loop (go back to polling for frames)*/
                if( res == -EPIPE || res == -ESTRPIPE )
                    framesAvail = 0;
                else
                    ENSURE( res, paUnanticipatedHostError );
            }
            if( stream->pcm_playback )
            {
                int res = snd_pcm_mmap_commit( stream->pcm_playback, playbackOffset, MIN( stream->playbackAvail,
                            framesProcessed ) );

                /* Non-fatal error? Terminate loop (go back to polling for frames) */
                if( res == -EPIPE || res == -ESTRPIPE )
                    framesAvail = 0;
                else
                    ENSURE( res, paUnanticipatedHostError );
            }

            /* If threshold for starting stream specified (priming buffer), decrement and compare */
            if( startThreshold > 0 )
            {
                if( (startThreshold -= framesGot) <= 0 )
                {
                    ASSERT_CALL( pthread_mutex_lock( &stream->startMtx ), 0 );
                    ENSURE_PA( AlsaStart( stream, 1 ) );    /* Buffer will be zeroed */
                    ASSERT_CALL( pthread_cond_signal( &stream->startCond ), 0 );
                    ASSERT_CALL( pthread_mutex_unlock( &stream->startMtx ), 0 );
                }
            }

            if( callbackResult != paContinue )
            {
                stream->callbackAbort = (callbackResult == paAbort);
                goto end;
            }

            framesAvail -= framesGot;
        }
    }

    /* This code is unreachable, but important to include regardless because it
     * is possibly a macro with a closing brace to match the opening brace in
     * pthread_cleanup_push() above.  The documentation states that they must
     * always occur in pairs. */
    pthread_cleanup_pop( 1 );

end:
    pthread_exit( pres );

error:
    /* Pass on error code */
    pres = malloc( sizeof (PaError) );
    *pres = result;
    
    goto end;
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
    snd_pcm_t *save = stream->pcm_playback;
    snd_pcm_uframes_t offset;

    assert( stream );

    UNLESS( stream->pcm_capture, paCanNotReadFromAnOutputOnlyStream );

    /* Disregard playback */
    stream->pcm_playback = NULL;

    if( stream->overrun )
    {
        result = paInputOverflowed;
        stream->overrun = 0.0;
    }

    if( stream->bufferProcessor.userInputIsInterleaved )
        userBuffer = buffer;
    else /* Copy channels into local array */
    {
        int numBytes = sizeof (void *) * stream->captureUserChannels;
        UNLESS( userBuffer = alloca( numBytes ), paInsufficientMemory );
        memcpy( userBuffer, buffer, sizeof (void *) * stream->captureUserChannels );
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
        ENSURE_PA( err );
        framesAvail = (snd_pcm_uframes_t) err;

        if( framesAvail == 0 )
            ENSURE_PA( Wait( stream, &framesAvail ) );
        framesAvail = MIN( framesAvail, frames );

        ENSURE_PA( SetUpBuffers( stream, framesAvail, 0, &framesGot, &offset, NULL ) );
        framesGot = PaUtil_CopyInput( &stream->bufferProcessor, &userBuffer, framesGot );
        ENSURE( snd_pcm_mmap_commit( stream->pcm_capture, offset, framesGot ),
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
    /*int i;*/
    snd_pcm_t *save = stream->pcm_capture;
    snd_pcm_uframes_t offset;
    
    assert( stream );

    UNLESS( stream->pcm_playback, paCanNotWriteToAnInputOnlyStream );

    /* Disregard capture */
    stream->pcm_capture = NULL;

    if( stream->underrun )
    {
        result = paOutputUnderflowed;
        stream->underrun = 0.0;
    }

    if( stream->bufferProcessor.userOutputIsInterleaved )
        userBuffer = buffer;
    else /* Copy channels into local array */
    {
        int numBytes = sizeof (void *) * stream->playbackUserChannels;
        UNLESS( userBuffer = alloca( numBytes ), paInsufficientMemory );
        memcpy( (void *)userBuffer, buffer, sizeof (void *) * stream->playbackUserChannels );
        /*
        for( i = 0; i < stream->playbackUserChannels; ++i )
            ((const void **) userBuffer)[i] = ((const void **) buffer)[i];
            */
    }

    while( frames > 0 )
    {
        snd_pcm_uframes_t hwAvail;

        ENSURE_PA( err = GetStreamWriteAvailable( stream ) );
        framesAvail = err;
        if( framesAvail == 0 )
            ENSURE_PA( Wait( stream, &framesAvail ) );
        framesAvail = MIN( framesAvail, frames );

        ENSURE_PA( SetUpBuffers( stream, framesAvail, 0, &framesGot, NULL, &offset ) );
        framesGot = PaUtil_CopyOutput( &stream->bufferProcessor, &userBuffer, framesGot );
        ENSURE( snd_pcm_mmap_commit( stream->pcm_playback, offset, framesGot ),
                paUnanticipatedHostError );

        frames -= framesGot;

        /* Frames residing in buffer */
        ENSURE_PA( err = GetStreamWriteAvailable( stream ) );
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
            ENSURE_PA( HandleXrun( stream ) );
            avail = snd_pcm_avail_update( stream->pcm_capture );
        }

        if( avail == -EPIPE )
            ENSURE_PA( paInputOverflowed );
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
            ENSURE_PA( HandleXrun( stream ) );
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
    info->deviceString = NULL;
}

void PaAlsa_EnableRealtimeScheduling( PaStream *s, int enable )
{
    PaAlsaStream *stream = (PaAlsaStream *) s;
    stream->threading.rtSched = enable;
}

void PaAlsa_EnableWatchdog( PaStream *s, int enable )
{
    PaAlsaStream *stream = (PaAlsaStream *) s;
    stream->threading.useWatchdog = enable;
}
