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
 
#include "pa_converters.h"
#include "pa_dither.h"


PaSampleFormat PaUtil_SelectClosestAvailableFormat( PaSampleFormat availableFormats, PaSampleFormat format )
{
    PaSampleFormat result;

    format &= ~paNonInterleaved;
    availableFormats &= ~paNonInterleaved;
    
    if( (format & availableFormats) == 0 )
    {
        /* NOTE: this code depends on the sample format constants being in
            descending order of quality - ie best quality is 0 */

        if( format != 0x01 )
        {
            /* scan for better formats */
            result = format;
            do
            {
                result >>= 1;
            }
            while( (result & availableFormats) == 0 && result != 0 );
        }
        else
        {
            result = 0;
        }
        
        if( result == 0 ){
            /* scan for worse formats */
            result = format;
            do
            {
                result <<= 1;
            }
            while( (result & availableFormats) == 0 && result != paCustomFormat );

            if( (result & availableFormats) == 0 )
                result = paSampleFormatNotSupported;
        }
        
    }else{
        result = format;
    }

    return result;
}


#define PA_SELECT_FORMAT_( format, float32, int32, int24, packedInt24, int16, int8, uint8 )\
    switch( format & ~paNonInterleaved ){                                                       \
    case paFloat32:                                                     \
        float32                                                         \
    case paInt32:                                                       \
        int32                                                           \
    case paInt24:                                                       \
        int24                                                           \
    case paPackedInt24:                                                 \
        packedInt24                                                     \
    case paInt16:                                                       \
        int16                                                           \
    case paInt8:                                                        \
        int8                                                            \
    case paUInt8:                                                       \
        uint8                                                           \
    default: return 0;                                                  \
    }


#define PA_SELECT_CONVERTER_DITHER_CLIP_( flags, source, destination )\
    if( flags & paClipOff ){ /* no clip */                                 \
        if( flags & paDitherOff ){ /* no dither */                          \
            return paConverters. ## source ## _ ## destination;             \
        }else{ /* dither */                                                 \
            return paConverters. ## source ## _ ## destination ## _Dither;  \
        }                                                                   \
    }else{ /* clip */                                                       \
        if( flags & paDitherOff ){ /* no dither */                          \
            return paConverters. ## source ## _ ## destination ## _Clip;    \
        }else{ /* dither */                                                 \
            return paConverters. ## source ## _ ## destination ## _DitherClip;\
        }                                                                   \
    }


#define PA_SELECT_CONVERTER_DITHER_( flags, source, destination )\
    if( flags & paDitherOff ){ /* no dither */                              \
        return paConverters. ## source ## _ ## destination;                 \
    }else{ /* dither */                                                     \
        return paConverters. ## source ## _ ## destination ## _Dither;      \
    }


#define PA_USE_CONVERTER_( source, destination )\
    return paConverters. ## source ## _ ## destination;


#define PA_NO_CONVERSION_DEFINED_ return 0;


#define PA_UNITY_CONVERSION_ return 0;


PaUtilConverter* PaUtil_SelectConverter( PaSampleFormat sourceFormat,
        PaSampleFormat destinationFormat, PaStreamFlags flags )
{
    PA_SELECT_FORMAT_( sourceFormat,
                       /* paFloat32: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_UNITY_CONVERSION_,
                                          /* paInt32: */          PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, Int32 ),
                                          /* paInt24: */          PA_NO_CONVERSION_DEFINED_,
                                          /* paPackedInt24: */    PA_NO_CONVERSION_DEFINED_,
                                          /* paInt16: */          PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, Int16 ),
                                          /* paInt8: */           PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, Int8 ),
                                          /* paUInt8: */          PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, UInt8 )
                                        ),
                       /* paInt32: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( Int32, Float32 ),
                                          /* paInt32: */          PA_UNITY_CONVERSION_,
                                          /* paInt24: */          PA_NO_CONVERSION_DEFINED_,
                                          /* paPackedInt24: */    PA_NO_CONVERSION_DEFINED_,
                                          /* paInt16: */          PA_SELECT_CONVERTER_DITHER_( flags, Int32, Int16 ),
                                          /* paInt8: */           PA_SELECT_CONVERTER_DITHER_( flags, Int32, Int8 ),
                                          /* paUInt8: */          PA_SELECT_CONVERTER_DITHER_( flags, Int32, UInt8 )
                                        ),
                       /* paInt24: */
                       PA_NO_CONVERSION_DEFINED_,
                       /* paPackedInt24: */
                       PA_NO_CONVERSION_DEFINED_,
                       /* paInt16: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( Int16, Float32 ),
                                          /* paInt32: */          PA_USE_CONVERTER_( Int16, Int32 ),
                                          /* paInt24: */          PA_NO_CONVERSION_DEFINED_,
                                          /* paPackedInt24: */    PA_NO_CONVERSION_DEFINED_,
                                          /* paInt16: */          PA_UNITY_CONVERSION_,
                                          /* paInt8: */           PA_SELECT_CONVERTER_DITHER_( flags, Int16, Int8 ),
                                          /* paUInt8: */          PA_SELECT_CONVERTER_DITHER_( flags, Int16, UInt8 )
                                        ),
                       /* paInt8: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( Int8, Float32 ),
                                          /* paInt32: */          PA_USE_CONVERTER_( Int8, Int32 ),
                                          /* paInt24: */          PA_NO_CONVERSION_DEFINED_,
                                          /* paPackedInt24: */    PA_NO_CONVERSION_DEFINED_,
                                          /* paInt16: */          PA_USE_CONVERTER_( Int8, Int16 ),
                                          /* paInt8: */           PA_UNITY_CONVERSION_,
                                          /* paUInt8: */          PA_USE_CONVERTER_( Int8, UInt8 )
                                        ),
                       /* paUInt8: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( UInt8, Float32 ),
                                          /* paInt32: */          PA_USE_CONVERTER_( UInt8, Int32 ),
                                          /* paInt24: */          PA_NO_CONVERSION_DEFINED_,
                                          /* paPackedInt24: */    PA_NO_CONVERSION_DEFINED_,
                                          /* paInt16: */          PA_USE_CONVERTER_( UInt8, Int16 ),
                                          /* paInt8: */           PA_USE_CONVERTER_( UInt8, Int8 ),
                                          /* paUInt8: */          PA_UNITY_CONVERSION_
                                        )
                     )
}


#ifdef PA_NO_STANDARD_CONVERTERS

PaUtilConverterTable paConverters = {
                                        0, /* PaUtilConverter *Float32_Int32; */
                                        0, /* PaUtilConverter *Float32_Int32_Dither; */
                                        0, /* PaUtilConverter *Float32_Int32_Clip; */
                                        0, /* PaUtilConverter *Float32_Int32_DitherClip; */

                                        0, /* PaUtilConverter *Float32_Int16; */
                                        0, /* PaUtilConverter *Float32_Int16_Dither; */
                                        0, /* PaUtilConverter *Float32_Int16_Clip; */
                                        0, /* PaUtilConverter *Float32_Int16_DitherClip; */

                                        0, /* PaUtilConverter *Float32_Int8; */
                                        0, /* PaUtilConverter *Float32_Int8_Dither; */
                                        0, /* PaUtilConverter *Float32_Int8_Clip; */
                                        0, /* PaUtilConverter *Float32_Int8_DitherClip; */

                                        0, /* PaUtilConverter *Float32_UInt8; */
                                        0, /* PaUtilConverter *Float32_UInt8_Dither; */
                                        0, /* PaUtilConverter *Float32_UInt8_Clip; */
                                        0, /* PaUtilConverter *Float32_UInt8_DitherClip; */

                                        0, /* PaUtilConverter *Int32_Float32; */
                                        0, /* PaUtilConverter *Int32_Int16; */
                                        0, /* PaUtilConverter *Int32_Int16_Dither; */
                                        0, /* PaUtilConverter *Int32_Int8; */
                                        0, /* PaUtilConverter *Int32_Int8_Dither; */
                                        0, /* PaUtilConverter *Int32_UInt8; */
                                        0, /* PaUtilConverter *Int32_UInt8_Dither; */

                                        0, /* PaUtilConverter *Int16_Float32; */
                                        0, /* PaUtilConverter *Int16_Int32; */
                                        0, /* PaUtilConverter *Int16_Int8; */
                                        0, /* PaUtilConverter *Int16_Int8_Dither; */
                                        0, /* PaUtilConverter *Int16_UInt8; */
                                        0, /* PaUtilConverter *Int16_UInt8_Dither; */

                                        0, /* PaUtilConverter *Int8_Float32; */
                                        0, /* PaUtilConverter *Int8_Int32; */
                                        0, /* PaUtilConverter *Int8_Int16; */
                                        0, /* PaUtilConverter *Int8_UInt8; */

                                        0, /* PaUtilConverter *UInt8_Float32; */
                                        0, /* PaUtilConverter *UInt8_Int16; */
                                        0, /* PaUtilConverter *UInt8_Int32; */
                                        0, /* PaUtilConverter *UInt8_Int8; */
                                    };

#else

/* -------------------------------------------------------------------------- */

#define PA_CLIP_( val, min, max )\
    { val = ((val) < (min)) ? min : (((val) < (max)) ? (max) : (val)); }


static const float const_1_div_128_ = 1.0f / 128.0f;

static const float const_1_div_32768_ = 1.0f / 32768.f;

/* -------------------------------------------------------------------------- */

static void Float32_Int32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int32_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int32_Clip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int32_DitherClip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int16(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed short *dest =  (signed short*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        short samp = (short) (*src * (32767.0f));
        *dest = samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int16_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed short *dest = (signed short*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        float dither  = PaUtil_GenerateFloatTriangularDither( ditherGenerator );
        /* use smaller scaler to prevent overflow when we add the dither */
        float dithered = (*src * (32766.0f)) + dither;
        *dest = (signed short) dithered;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int16_Clip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed short *dest =  (signed short*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        long samp = (signed long) (*src * (32767.0f));
        PA_CLIP_( samp, -0x8000, 0x7FFF );
        *dest = (signed short) samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int16_DitherClip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed short *dest =  (signed short*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        float dither  = PaUtil_GenerateFloatTriangularDither( ditherGenerator );
        /* use smaller scaler to prevent overflow when we add the dither */
        float dithered = (*src * (32766.0f)) + dither;
        signed long samp = (signed long) dithered;
        PA_CLIP_( samp, -0x8000, 0x7FFF );
        *dest = (signed short) samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed char *dest =  (signed char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        signed char samp = (signed char) (*src * (127.0f));
        *dest = samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int8_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed char *dest =  (signed char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        float dither  = PaUtil_GenerateFloatTriangularDither( ditherGenerator );
        /* use smaller scaler to prevent overflow when we add the dither */
        float dithered = (*src * (126.0f)) + dither;
        signed long samp = (signed long) dithered;
        *dest = (signed char) samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int8_Clip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed char *dest =  (signed char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        signed long samp = (signed long)(*src * (127.0f));
        PA_CLIP_( samp, -0x80, 0x7F );
        *dest = (signed char) samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_Int8_DitherClip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed char *dest =  (signed char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        float dither  = PaUtil_GenerateFloatTriangularDither( ditherGenerator );
        /* use smaller scaler to prevent overflow when we add the dither */
        float dithered = (*src * (126.0f)) + dither;
        signed long samp = (signed long) dithered;
        PA_CLIP_( samp, -0x80, 0x7F );
        *dest = (signed char) samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_UInt8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        unsigned char samp = (unsigned char)(128 + ((unsigned char) (*src * (127.0f))));
        *dest = samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_UInt8_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_UInt8_Clip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_UInt8_DitherClip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int32_Float32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed long *src = (signed long*)sourceBuffer;
    float *dest =  (float*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int32_Int16(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed long *src = (signed long*)sourceBuffer;
    signed short *dest =  (signed short*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int32_Int16_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed long *src = (signed long*)sourceBuffer;
    signed short *dest =  (signed short*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int32_Int8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed long *src = (signed long*)sourceBuffer;
    signed char *dest =  (signed char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int32_Int8_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed long *src = (signed long*)sourceBuffer;
    signed char *dest =  (signed char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int32_UInt8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed long *src = (signed long*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int32_UInt8_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed long *src = (signed long*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int16_Float32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed short *src = (signed short*)sourceBuffer;
    float *dest =  (float*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        float samp = *src * const_1_div_32768_; /* FIXME: i'm concerned about this being asymetrical with float->int16 -rb */
        *dest = samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int16_Int32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed short *src = (signed short*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int16_Int8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed short *src = (signed short*)sourceBuffer;
    signed char *dest =  (signed char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int16_Int8_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed short *src = (signed short*)sourceBuffer;
    signed char *dest =  (signed char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int16_UInt8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed short *src = (signed short*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int16_UInt8_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed short *src = (signed short*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int8_Float32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed char *src = (signed char*)sourceBuffer;
    float *dest =  (float*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        float samp = *src * const_1_div_128_;
        *dest = samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int8_Int32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed char *src = (signed char*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int8_Int16(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed char *src = (signed char*)sourceBuffer;
    signed short *dest =  (signed short*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int8_UInt8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed char *src = (signed char*)sourceBuffer;
    unsigned char *dest =  (unsigned char*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void UInt8_Float32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    float *dest =  (float*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        float samp = (*src - 128) * const_1_div_128_;
        *dest = samp;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void UInt8_Int32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void UInt8_Int16(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    signed short *dest =  (signed short*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void UInt8_Int8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    float *dest =  (float*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {

        /* IMPLEMENT ME */

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */


PaUtilConverterTable paConverters = {
                                        Float32_Int32,              /* PaUtilConverter *Float32_Int32; */
                                        Float32_Int32_Dither,       /* PaUtilConverter *Float32_Int32_Dither; */
                                        Float32_Int32_Clip,         /* PaUtilConverter *Float32_Int32_Clip; */
                                        Float32_Int32_DitherClip,   /* PaUtilConverter *Float32_Int32_DitherClip; */

                                        Float32_Int16,              /* PaUtilConverter *Float32_Int16; */
                                        Float32_Int16_Dither,       /* PaUtilConverter *Float32_Int16_Dither; */
                                        Float32_Int16_Clip,         /* PaUtilConverter *Float32_Int16_Clip; */
                                        Float32_Int16_DitherClip,   /* PaUtilConverter *Float32_Int16_DitherClip; */

                                        Float32_Int8,               /* PaUtilConverter *Float32_Int8; */
                                        Float32_Int8_Dither,        /* PaUtilConverter *Float32_Int8_Dither; */
                                        Float32_Int8_Clip,          /* PaUtilConverter *Float32_Int8_Clip; */
                                        Float32_Int8_DitherClip,    /* PaUtilConverter *Float32_Int8_DitherClip; */

                                        Float32_UInt8,              /* PaUtilConverter *Float32_UInt8; */
                                        Float32_UInt8_Dither,       /* PaUtilConverter *Float32_UInt8_Dither; */
                                        Float32_UInt8_Clip,         /* PaUtilConverter *Float32_UInt8_Clip; */
                                        Float32_UInt8_DitherClip,   /* PaUtilConverter *Float32_UInt8_DitherClip; */

                                        Int32_Float32,              /* PaUtilConverter *Int32_Float32; */
                                        Int32_Int16,                /* PaUtilConverter *Int32_Int16; */
                                        Int32_Int16_Dither,         /* PaUtilConverter *Int32_Int16_Dither; */
                                        Int32_Int8,                 /* PaUtilConverter *Int32_Int8; */
                                        Int32_Int8_Dither,          /* PaUtilConverter *Int32_Int8_Dither; */
                                        Int32_UInt8,                /* PaUtilConverter *Int32_UInt8; */
                                        Int32_UInt8_Dither,         /* PaUtilConverter *Int32_UInt8_Dither; */

                                        Int16_Float32,              /* PaUtilConverter *Int16_Float32; */
                                        Int16_Int32,                /* PaUtilConverter *Int16_Int32; */
                                        Int16_Int8,                 /* PaUtilConverter *Int16_Int8; */
                                        Int16_Int8_Dither,          /* PaUtilConverter *Int16_Int8_Dither; */
                                        Int16_UInt8,                /* PaUtilConverter *Int16_UInt8; */
                                        Int16_UInt8_Dither,         /* PaUtilConverter *Int16_UInt8_Dither; */

                                        Int8_Float32,               /* PaUtilConverter *Int8_Float32; */
                                        Int8_Int32,                 /* PaUtilConverter *Int8_Int32; */
                                        Int8_Int16,                 /* PaUtilConverter *Int8_Int16; */
                                        Int8_UInt8,                 /* PaUtilConverter *Int8_UInt8; */

                                        UInt8_Float32,              /* PaUtilConverter *UInt8_Float32; */
                                        UInt8_Int32,                /* PaUtilConverter *UInt8_Int32; */
                                        UInt8_Int16,                /* PaUtilConverter *UInt8_Int16; */
                                        UInt8_Int8,                 /* PaUtilConverter *UInt8_Int8; */
                                    };

#endif /* PA_NO_STANDARD_CONVERTERS */
