/*
 * $Id$
 * Portable Audio I/O Library triangular dither generator
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

#include "pa_dither.h"

#define PA_DITHER_BITS_   (15)


void PaUtil_InitializeTriangularDitherState( PaUtilTriangularDitherGenerator *state )
{
    state->previous = 0;
    state->randSeed1 = 22222;
    state->randSeed2 = 5555555;
}


signed long PaUtil_Generate16BitTriangularDither( PaUtilTriangularDitherGenerator *state )
{
    signed long current, highPass;

    /* Generate two random numbers. */
    state->randSeed1 = (state->randSeed1 * 196314165) + 907633515;
    state->randSeed2 = (state->randSeed2 * 196314165) + 907633515;

    /* Generate triangular distribution about 0.
     * Shift before adding to prevent overflow which would skew the distribution.
     * Also shift an extra bit for the high pass filter. 
     */
#define DITHER_SHIFT_  ((32 - PA_DITHER_BITS_) + 1)
    current = (((signed long)state->randSeed1)>>DITHER_SHIFT_) +
              (((signed long)state->randSeed2)>>DITHER_SHIFT_);

    /* High pass filter to reduce audibility. */
    highPass = current - state->previous;
    state->previous = current;
    return highPass;
}

/* Multiply by PA_FLOAT_DITHER_SCALE_ to get a float between -2.0 and +1.99999 */
#define PA_FLOAT_DITHER_SCALE_  (1.0f / ((1<<PA_DITHER_BITS_)-1))
static const float const_float_dither_scale_ = PA_FLOAT_DITHER_SCALE_;

float PaUtil_GenerateFloatTriangularDither( PaUtilTriangularDitherGenerator *state )
{
    signed long current, highPass;

    /* Generate two random numbers. */
    state->randSeed1 = (state->randSeed1 * 196314165) + 907633515;
    state->randSeed2 = (state->randSeed2 * 196314165) + 907633515;

    /* Generate triangular distribution about 0.
     * Shift before adding to prevent overflow which would skew the distribution.
     * Also shift an extra bit for the high pass filter. 
     */
#define DITHER_SHIFT_  ((32 - PA_DITHER_BITS_) + 1)
    current = (((signed long)state->randSeed1)>>DITHER_SHIFT_) +
              (((signed long)state->randSeed2)>>DITHER_SHIFT_);

    /* High pass filter to reduce audibility. */
    highPass = current - state->previous;
    state->previous = current;
    return ((float)highPass) * const_float_dither_scale_;
}
