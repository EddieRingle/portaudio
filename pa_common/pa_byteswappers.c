/*
 * $Id$ 
 * Portable Audio I/O Library sample byte swapping mechanism
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Phil Burk, Ross Bencina
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
 
#include "pa_byteswappers.h"


PaUtilByteSwapper* PaUtil_SelectByteSwapper( PaSampleFormat sampleFormat )
{
    signed int bytesPerSample = Pa_GetSampleSize( sampleFormat );

    switch( bytesPerSample )
    {
    case 2: return paByteSwappers.SwapBytes2;
    case 3: return paByteSwappers.SwapBytes3;
    case 4: return paByteSwappers.SwapBytes4;
    default: return 0;
    }
}


#ifdef PA_NO_STANDARD_BYTESWAPPERS

PaUtilByteSwapperTable paByteSwappers = {
                                            0, /* PaUtilByteSwapper *SwapBytes2; */
                                            0, /* PaUtilByteSwapper *SwapBytes3; */
                                            0 /* PaUtilByteSwapper *SwapBytes4; */
                                        };

#else

/*
FIXME: the following functions are not necessarily correct
or the most efficient. just a first attempt - rossb
*/

static void SwapBytes2( void *buffer, unsigned int count )
{
    unsigned short *p = (unsigned short*)buffer;
    unsigned short temp;
    while( count-- > 0)
    {
        temp = *p;
        *p++ = (unsigned short)((temp<<8) | (temp>>8));
    }
}

static void SwapBytes3( void *buffer, unsigned int count )
{
    unsigned char *p = buffer;
    unsigned char temp;

    while( count-- )
    {
        temp = *p;
        *p = *(p+2);
        *(p+2) = temp;
        p += 3;
    }
}

static void SwapBytes4( void *buffer, unsigned int count )
{
    unsigned long *p = buffer;
    unsigned long temp;

    while( count-- )
    {
        temp = *p;
        *p++ = (temp>>24) | ((temp>>8)&0xFF00) | ((temp<<8)&0xFF0000) | (temp<<24);
    }
}

PaUtilByteSwapperTable paByteSwappers = {
                                            SwapBytes2, /* PaUtilByteSwapper *SwapBytes2; */
                                            SwapBytes3, /* PaUtilByteSwapper *SwapBytes3; */
                                            SwapBytes4 /* PaUtilByteSwapper *SwapBytes4; */
                                        };

#endif /* PA_NO_STANDARD_BYTESWAPPERS */


