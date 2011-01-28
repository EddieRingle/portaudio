/*
 * $Id$
 * Portable Audio I/O Library
 * Win32 platform-specific support functions
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2008 Ross Bencina
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
 @ingroup win_src

 @brief Win32 implementation of platform-specific PaUtil support functions.

    @todo Implement workaround for QueryPerformanceCounter() skipping forward
    bug. (see msdn kb Q274323).
*/
 
#include <windows.h>
#include <mmsystem.h> /* for timeGetTime() */
#include <assert.h>

#include "pa_util.h"

#if (defined(WIN32) && (defined(_MSC_VER) && (_MSC_VER >= 1200))) && !defined(_WIN32_WCE) /* MSC version 6 and above */
#pragma comment( lib, "winmm.lib" )
#endif


/*
   Track memory allocations to avoid leaks.
 */

#if PA_TRACK_MEMORY
static int numAllocations_ = 0;
#endif


void *PaUtil_AllocateMemory( long size )
{
    void *result = GlobalAlloc( GPTR, size );

#if PA_TRACK_MEMORY
    if( result != NULL ) numAllocations_ += 1;
#endif
    return result;
}


void PaUtil_FreeMemory( void *block )
{
    if( block != NULL )
    {
        GlobalFree( block );
#if PA_TRACK_MEMORY
        numAllocations_ -= 1;
#endif

    }
}


int PaUtil_CountCurrentlyAllocatedBlocks( void )
{
#if PA_TRACK_MEMORY
    return numAllocations_;
#else
    return 0;
#endif
}


void Pa_Sleep( long msec )
{
    Sleep( msec );
}

static int usePerformanceCounter_;
static double secondsPerTick_;

void PaUtil_InitializeClock( void )
{
    LARGE_INTEGER ticksPerSecond;

    if( QueryPerformanceFrequency( &ticksPerSecond ) != 0 )
    {
        usePerformanceCounter_ = 1;
        secondsPerTick_ = 1.0 / (double)ticksPerSecond.QuadPart;
    }
    else
    {
        usePerformanceCounter_ = 0;
    }
}


double PaUtil_GetTime( void )
{
    LARGE_INTEGER time;

    if( usePerformanceCounter_ )
    {
        /* FIXME:
            according to this knowledge-base article, QueryPerformanceCounter
            can skip forward by seconds!
            http://support.microsoft.com/default.aspx?scid=KB;EN-US;Q274323&

            it may be better to use the rtdsc instruction using inline asm,
            however then a method is needed to calculate a ticks/seconds ratio.
        */
        QueryPerformanceCounter( &time );
        return time.QuadPart * secondsPerTick_;
    }
    else
    {
#ifndef UNDER_CE    	
        return timeGetTime() * .001;
#else
        return GetTickCount() * .001;
#endif                
    }
}

#include <process.h>

/* use CreateThread for CYGWIN/Windows Mobile, _beginthreadex for all others */
#if !defined(__CYGWIN__) && !defined(_WIN32_WCE)
#define CREATE_THREAD_FUNCTION (HANDLE)_beginthreadex
#define THREAD_FUNCTION_RETURN_TYPE unsigned
#define PA_THREAD_FUNC static THREAD_FUNCTION_RETURN_TYPE WINAPI
#define EXIT_THREAD_FUNCTION    _endthreadex
#else
#define CREATE_THREAD_FUNCTION CreateThread
#define THREAD_FUNCTION_RETURN_TYPE DWORD
#define PA_THREAD_FUNC static THREAD_FUNCTION_RETURN_TYPE WINAPI
#define EXIT_THREAD_FUNCTION    ExitThread
#endif

typedef struct _tag_PaThread
{
    PaThreadFunction function;
    void*            data;
    HANDLE           handle;
    DWORD            dwThreadId;
} PaThreadStruct;

PA_THREAD_FUNC ThreadFunction(void* ptr)
{
    PaThreadStruct* p = (PaThreadStruct*)ptr;
    THREAD_FUNCTION_RETURN_TYPE retval = p->function(p->data);
    p->function = 0;
    EXIT_THREAD_FUNCTION(retval);
    return retval;
}

int PaUtil_CreateThread(PaThread* thread, PaThreadFunction threadFunction, void* data, unsigned createSuspended)
{
    PaThreadStruct* p;
    if (thread == NULL || threadFunction == NULL)
        return paUnanticipatedHostError;

    p = (PaThreadStruct*)PaUtil_AllocateMemory(sizeof(PaThreadStruct));
    if (p == NULL)
        return paInsufficientMemory;

    p->function = threadFunction;
    p->data = data;
    p->handle = CREATE_THREAD_FUNCTION(NULL, 0, ThreadFunction, p, createSuspended ? CREATE_SUSPENDED : 0, &p->dwThreadId);
    if (p->handle == 0 || p->handle == INVALID_HANDLE_VALUE)
    {
        PaUtil_FreeMemory(p);
        return paUnanticipatedHostError;
    }
    *thread = p;
    return paNoError;
}

int PaUtil_DestroyThread(PaThread thread)
{
    PaThreadStruct* p = (PaThreadStruct*)thread;
    if (p->function != 0)
    {
        /* This means that function is called while thread is still running */
        assert(0);
        return paUnanticipatedHostError;
    }

    PaUtil_FreeMemory(thread);
    return paNoError;
}

int PaUtil_StartThread(PaThread thread)
{
    PaThreadStruct* p = (PaThreadStruct*)thread;
    return (ResumeThread(p->handle) != (DWORD)-1) ? paNoError : paUnanticipatedHostError;
}

int PaUtil_WaitForThreadToExit( PaThread thread, unsigned timeOutMilliseconds )
{
    PaThreadStruct* p = (PaThreadStruct*)thread;
    return (WaitForSingleObject(p->handle, timeOutMilliseconds) == WAIT_OBJECT_0) ? paNoError : paTimedOut;
}

int PaUtil_TerminateThread(PaThread thread)
{
    PaThreadStruct* p = (PaThreadStruct*)thread;
    TerminateThread(p->handle, -1);
    p->function = 0;
    return paNoError;
}

extern const PaThread paCurrentThread = (PaThread)(size_t)(-2);

static const int kPriorityMapping[Priority_kCnt] = { 
    THREAD_PRIORITY_IDLE,
    THREAD_PRIORITY_BELOW_NORMAL,
    THREAD_PRIORITY_NORMAL,
    THREAD_PRIORITY_ABOVE_NORMAL,
    THREAD_PRIORITY_TIME_CRITICAL,
};

int PaUtil_SetThreadPriority(PaThread thread, PaThreadPriority priority)
{
    HANDLE hThread = (thread == paCurrentThread) ? GetCurrentThread() : ((PaThreadStruct*)thread)->handle;
    return SetThreadPriority(hThread, kPriorityMapping[priority]) ? paNoError : paUnanticipatedHostError;
}

PaThreadPriority PaUtil_GetThreadPriority(PaThread thread)
{
    int i;
    HANDLE hThread = (thread == paCurrentThread) ? GetCurrentThread() : ((PaThreadStruct*)thread)->handle;
    int winPrio = GetThreadPriority(hThread);
    for (i = 0; i < Priority_kCnt; ++i)
    {
        if (kPriorityMapping[i] >= winPrio)
            return (PaThreadPriority)i;
    }
    assert(0);
    return Priority_kNormal;
}

