#include <windows.h>
#include <mmsystem.h> /* for timeGetTime() */

#include "pa_util.h"


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


int PaUtil_CountMemoryLeaks( void )
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
static double microsecondsPerTick_;

void PaUtil_InitializeMicrosecondClock( void )
{
    LARGE_INTEGER frequency;

    if( QueryPerformanceFrequency( &frequency ) != 0 )
    {
        usePerformanceCounter_ = 1;
        microsecondsPerTick_ = (double)frequency.QuadPart * 0.000001;
    }
    else
    {
        usePerformanceCounter_ = 0;
    }
}


double PaUtil_MicrosecondTime( void )
{
    LARGE_INTEGER time;

    if( usePerformanceCounter_ )
    {
        QueryPerformanceCounter( &time );
        return time.QuadPart * microsecondsPerTick_;
    }
    else
    {
        return timeGetTime() * 1000.;
    }
}
