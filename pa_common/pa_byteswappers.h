#ifndef PA_BYTESWAPPERS_H
#define PA_BYTESWAPPERS_H
/*
 *
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

#include "portaudio.h" /* for PaSampleFormat */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


/* high level functions for use by implementations */


typedef void PaUtilByteSwapper( void *buffer, unsigned int count );
/**< The generic byte swapper prototype. Byte swappers convert a buffer from
    one byte order to another in-place. The actial type of the data pointed
    to by the buffer parameter varies from function to function.
    @param buffer A pointer to the first sample of the buffer to be byte swapped
    @param count The number of samples to be byte swapped.
*/


PaUtilByteSwapper* PaUtil_SelectByteSwapper( PaSampleFormat sampleFormat );
/**< Find a byte swapper for samples in the specified format.
    @param sampleFormat The format of the samples to be byte swapped.
    @return When sampleFormat indicates to a multi byte sample, a pointer to a
    valid byte swapping function will be returned. NULL will be returned
    for single byte samples, as they do not need to be byte swapped.
*/


/* low level functions and data structures which may be used for
    substituting additional conversion functions */


typedef struct{
    PaUtilByteSwapper *SwapBytes2;
    PaUtilByteSwapper *SwapBytes3;
    PaUtilByteSwapper *SwapBytes4;
}PaUtilByteSwapperTable;
/**< The type used to store all byte swapping functions.
    @see PaUtilByteSwapper, paByteSwappers
*/


extern PaUtilByteSwapperTable paByteSwappers;
/**< A table of pointers to all required byte swapping functions.
    PaUtil_SelectByteSwapper() uses this table to lookup the appropriate
    byte swapping functions.

    @note
    If the PA_NO_STANDARD_BYTESWAPPERS preprocessor variable is defined,
    PortAudio's standard byte swappers will not be compiled, and all fields
    of this structure will be initialized to NULL. In such cases, users should
    supply their own byte swapping functions if they require PortAudio to open
    a stream that requires byte swapping.
    
    @see paByteSwappers, PaUtil_SelectByteSwapper, PaUtilByteSwapper
*/


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_BYTESWAPPERS_H */