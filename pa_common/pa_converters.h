#ifndef PA_CONVERTERS_H
#define PA_CONVERTERS_H
/*
 * $Id$
 * Portable Audio I/O Library sample conversion mechanism
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

#include "portaudio.h"  /* for PaSampleFormat */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


/**
Choose a format from availableFormats which can best be used to represent
format. If the requested format is not available better formats are
searched for before worse formats.
*/
PaSampleFormat PaUtil_SelectClosestAvailableFormat(
        PaSampleFormat availableFormats, PaSampleFormat format );


/* high level conversions functions for use by implementations */

typedef void PaUtilConverter(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, struct PaUtilTriangularDitherGenerator *ditherGenerator );
/**< The generic converter prototype. Converters convert count samples from
    sourceBuffer to destinationBuffer. The actual type of the data pointed to
    by these parameters varys for different converter functions.
    @param destinationBuffer A pointer to the first sample of the destination.
    @param destinationStride An offset between successive destination samples
    expressed in samples (not bytes.) It may be negative.
    @param sourceBuffer A pointer to the first sample of the source.
    @param sourceStride An offset between successive source samples
    expressed in samples (not bytes.) It may be negative.
    @param count The number of samples to convert.
    @param ditherState State information used to calculate dither. Converters
    that do not perform dithering will ignore this parameter, in which case
    NULL or invalid dither state may be passed.
*/


PaUtilConverter* PaUtil_SelectConverter( PaSampleFormat sourceFormat,
        PaSampleFormat destinationFormat, PaStreamFlags flags );
/**< Find a converter function for the given source and destinations formats
    and flags (clip and dither.)
    @return
    A pointer to a PaUtil_Converter which will perform the requested
    conversion, or NULL if the given format conversion is not supported.
    For conversions where clipping or dithering is not necessary, the
    clip and dither flags are ignored and a non-clipping or dithering
    version is returned.
    If the source and destination formats are the same, a function which
    copies data of the appropriate size will be returned.
*/


/* low level functions and data structures which may be used for
    substituting additional conversion functions */

    
typedef struct{
    PaUtilConverter *Float32_To_Int32;
    PaUtilConverter *Float32_To_Int32_Dither;
    PaUtilConverter *Float32_To_Int32_Clip;
    PaUtilConverter *Float32_To_Int32_DitherClip;

    PaUtilConverter *Float32_To_Int24;
    PaUtilConverter *Float32_To_Int24_Dither;
    PaUtilConverter *Float32_To_Int24_Clip;
    PaUtilConverter *Float32_To_Int24_DitherClip;
    
    PaUtilConverter *Float32_To_Int16;
    PaUtilConverter *Float32_To_Int16_Dither;
    PaUtilConverter *Float32_To_Int16_Clip;
    PaUtilConverter *Float32_To_Int16_DitherClip;

    PaUtilConverter *Float32_To_Int8;
    PaUtilConverter *Float32_To_Int8_Dither;
    PaUtilConverter *Float32_To_Int8_Clip;
    PaUtilConverter *Float32_To_Int8_DitherClip;

    PaUtilConverter *Float32_To_UInt8;
    PaUtilConverter *Float32_To_UInt8_Dither;
    PaUtilConverter *Float32_To_UInt8_Clip;
    PaUtilConverter *Float32_To_UInt8_DitherClip;

    PaUtilConverter *Int32_To_Float32;
    PaUtilConverter *Int32_To_Int24;
    PaUtilConverter *Int32_To_Int24_Dither;
    PaUtilConverter *Int32_To_Int16;
    PaUtilConverter *Int32_To_Int16_Dither;
    PaUtilConverter *Int32_To_Int8;
    PaUtilConverter *Int32_To_Int8_Dither;
    PaUtilConverter *Int32_To_UInt8;
    PaUtilConverter *Int32_To_UInt8_Dither;

    PaUtilConverter *Int24_To_Float32;
    PaUtilConverter *Int24_To_Int32;
    PaUtilConverter *Int24_To_Int16;
    PaUtilConverter *Int24_To_Int16_Dither;
    PaUtilConverter *Int24_To_Int8;
    PaUtilConverter *Int24_To_Int8_Dither;
    PaUtilConverter *Int24_To_UInt8;
    PaUtilConverter *Int24_To_UInt8_Dither;

    PaUtilConverter *Int16_To_Float32;
    PaUtilConverter *Int16_To_Int32;
    PaUtilConverter *Int16_To_Int24;
    PaUtilConverter *Int16_To_Int8;
    PaUtilConverter *Int16_To_Int8_Dither;
    PaUtilConverter *Int16_To_UInt8;
    PaUtilConverter *Int16_To_UInt8_Dither;

    PaUtilConverter *Int8_To_Float32;
    PaUtilConverter *Int8_To_Int32;
    PaUtilConverter *Int8_To_Int24;
    PaUtilConverter *Int8_To_Int16;
    PaUtilConverter *Int8_To_UInt8;
    
    PaUtilConverter *UInt8_To_Float32;
    PaUtilConverter *UInt8_To_Int32;
    PaUtilConverter *UInt8_To_Int24;
    PaUtilConverter *UInt8_To_Int16;
    PaUtilConverter *UInt8_To_Int8;

    PaUtilConverter *Copy_8_To_8;       /* copy without any conversion */
    PaUtilConverter *Copy_16_To_16;     /* copy without any conversion */
    PaUtilConverter *Copy_24_To_24;     /* copy without any conversion */
    PaUtilConverter *Copy_32_To_32;     /* copy without any conversion */
} PaUtilConverterTable;
/**< The type used to store all sample conversion functions.
    @see paConverters;
*/


extern PaUtilConverterTable paConverters;
/**< A table of pointers to all required converter functions.
    PaUtil_SelectConverter() uses this table to lookup the appropriate
    conversion functions. The fields of this structure are initialized
    with default conversion functions. Fields may be NULL, indicating that
    no conversion function is available. User code may substitue optimised
    conversion functions by assigning different function pointers to
    these fields.

    @note
    If the PA_NO_STANDARD_CONVERTERS preprocessor variable is defined,
    PortAudio's standard converters will not be compiled, and all fields
    of this structure will be initialized to NULL. In such cases, users
    should supply their own conversion functions if the require PortAudio
    to open a stream that requires sample conversion.

    @see PaUtilConverterTable, PaUtilConverter, PaUtil_SelectConverter
*/


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_CONVERTERS_H */
