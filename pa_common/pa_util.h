#ifndef PA_UTIL_H
#define PA_UTIL_H
/*
 *
 * Portable Audio I/O Library implementation utilities header
 * common implementation utilities and interfaces
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

#include "portaudio.h"

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


void PaUtil_SetHostError( long error ); /* deprecated */


/**
PA_DEBUG() provides a simple debug message printing facility. The macro
passes it's argument to a printf-like function called PaUtil_DebugPrint()
which prints to stderr and always flushes the stream after printing.
Because preprocessor macros cannot directly accept variable length argument
lists, calls to the macro must include an additional set of parenthesis, eg:
PA_DEBUG(("errorno: %d", 1001 ));
*/

void PaUtil_DebugPrint( const char *format, ... );

#if (0) /* set to 1 to print debug messages */
#define PA_DEBUG(x) PaUtil_DebugPrint x ;
#else
#define PA_DEBUG(x)
#endif


/* the following functions are implemented in a per-platform .c file */

void *PaUtil_AllocateMemory( long size );
void PaUtil_FreeMemory( void *block );

int PaUtil_CountMemoryLeaks( void ); /* PA_TRACK_MEMORY must be defined for this to work */

void PaUtil_InitializeMicrosecondClock( void );
double PaUtil_MicrosecondTime( void ); /* used to implement CPU load functions */

/* void Pa_Sleep( long msec );  must also be implemented in per-platform .c file */



#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_UTIL_H */
