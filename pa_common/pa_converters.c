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

/*
TODO:
    - implement the converters marked IMPLEMENT ME
    - review the converters marked REVIEW
*/


#include "pa_converters.h"
#include "pa_dither.h"


PaSampleFormat PaUtil_SelectClosestAvailableFormat(
        PaSampleFormat availableFormats, PaSampleFormat format )
{
    PaSampleFormat result;

    format &= ~paNonInterleaved;
    availableFormats &= ~paNonInterleaved;
    
    if( (format & availableFormats) == 0 )
    {
        /* NOTE: this code depends on the sample format constants being in
            descending order of quality - ie best quality is 0
            FIXME: should write an assert which checks that all of the
            known constants conform to that requirement.
        */

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

/* -------------------------------------------------------------------------- */

#define PA_SELECT_FORMAT_( format, float32, int32, int24, int16, int8, uint8 ) \
    switch( format & ~paNonInterleaved ){                                      \
    case paFloat32:                                                            \
        float32                                                                \
    case paInt32:                                                              \
        int32                                                                  \
    case paInt24:                                                              \
        int24                                                                  \
    case paInt16:                                                              \
        int16                                                                  \
    case paInt8:                                                               \
        int8                                                                   \
    case paUInt8:                                                              \
        uint8                                                                  \
    default: return 0;                                                         \
    }

/* -------------------------------------------------------------------------- */

#define PA_SELECT_CONVERTER_DITHER_CLIP_( flags, source, destination )         \
    if( flags & paClipOff ){ /* no clip */                                     \
        if( flags & paDitherOff ){ /* no dither */                             \
            return paConverters. ## source ## _To_ ## destination;             \
        }else{ /* dither */                                                    \
            return paConverters. ## source ## _To_ ## destination ## _Dither;  \
        }                                                                      \
    }else{ /* clip */                                                          \
        if( flags & paDitherOff ){ /* no dither */                             \
            return paConverters. ## source ## _To_ ## destination ## _Clip;    \
        }else{ /* dither */                                                    \
            return paConverters. ## source ## _To_ ## destination ## _DitherClip;\
        }                                                                      \
    }

/* -------------------------------------------------------------------------- */

#define PA_SELECT_CONVERTER_DITHER_( flags, source, destination )              \
    if( flags & paDitherOff ){ /* no dither */                                 \
        return paConverters. ## source ## _To_ ## destination;                 \
    }else{ /* dither */                                                        \
        return paConverters. ## source ## _To_ ## destination ## _Dither;      \
    }

/* -------------------------------------------------------------------------- */

#define PA_USE_CONVERTER_( source, destination )\
    return paConverters. ## source ## _To_ ## destination;

/* -------------------------------------------------------------------------- */

#define PA_UNITY_CONVERSION_( wordlength )\
    return paConverters.Copy_ ## wordlength ## _To_ ## wordlength;

/* -------------------------------------------------------------------------- */

PaUtilConverter* PaUtil_SelectConverter( PaSampleFormat sourceFormat,
        PaSampleFormat destinationFormat, PaStreamFlags flags )
{
    PA_SELECT_FORMAT_( sourceFormat,
                       /* paFloat32: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_UNITY_CONVERSION_( 32 ),
                                          /* paInt32: */          PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, Int32 ),
                                          /* paInt24: */          PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, Int24 ),
                                          /* paInt16: */          PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, Int16 ),
                                          /* paInt8: */           PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, Int8 ),
                                          /* paUInt8: */          PA_SELECT_CONVERTER_DITHER_CLIP_( flags, Float32, UInt8 )
                                        ),
                       /* paInt32: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( Int32, Float32 ),
                                          /* paInt32: */          PA_UNITY_CONVERSION_( 32 ),
                                          /* paInt24: */          PA_SELECT_CONVERTER_DITHER_( flags, Int32, Int24 ),
                                          /* paInt16: */          PA_SELECT_CONVERTER_DITHER_( flags, Int32, Int16 ),
                                          /* paInt8: */           PA_SELECT_CONVERTER_DITHER_( flags, Int32, Int8 ),
                                          /* paUInt8: */          PA_SELECT_CONVERTER_DITHER_( flags, Int32, UInt8 )
                                        ),
                       /* paInt24: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( Int24, Float32 ),
                                          /* paInt32: */          PA_USE_CONVERTER_( Int24, Int32 ),
                                          /* paInt24: */          PA_UNITY_CONVERSION_( 24 ),
                                          /* paInt16: */          PA_SELECT_CONVERTER_DITHER_( flags, Int24, Int16 ),
                                          /* paInt8: */           PA_SELECT_CONVERTER_DITHER_( flags, Int24, Int8 ),
                                          /* paUInt8: */          PA_SELECT_CONVERTER_DITHER_( flags, Int24, UInt8 )
                                        ),
                       /* paInt16: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( Int16, Float32 ),
                                          /* paInt32: */          PA_USE_CONVERTER_( Int16, Int32 ),
                                          /* paInt24: */          PA_USE_CONVERTER_( Int16, Int24 ),
                                          /* paInt16: */          PA_UNITY_CONVERSION_( 16 ),
                                          /* paInt8: */           PA_SELECT_CONVERTER_DITHER_( flags, Int16, Int8 ),
                                          /* paUInt8: */          PA_SELECT_CONVERTER_DITHER_( flags, Int16, UInt8 )
                                        ),
                       /* paInt8: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( Int8, Float32 ),
                                          /* paInt32: */          PA_USE_CONVERTER_( Int8, Int32 ),
                                          /* paInt24: */          PA_USE_CONVERTER_( Int8, Int24 ),
                                          /* paInt16: */          PA_USE_CONVERTER_( Int8, Int16 ),
                                          /* paInt8: */           PA_UNITY_CONVERSION_( 8 ),
                                          /* paUInt8: */          PA_USE_CONVERTER_( Int8, UInt8 )
                                        ),
                       /* paUInt8: */
                       PA_SELECT_FORMAT_( destinationFormat,
                                          /* paFloat32: */        PA_USE_CONVERTER_( UInt8, Float32 ),
                                          /* paInt32: */          PA_USE_CONVERTER_( UInt8, Int32 ),
                                          /* paInt24: */          PA_USE_CONVERTER_( UInt8, Int24 ),
                                          /* paInt16: */          PA_USE_CONVERTER_( UInt8, Int16 ),
                                          /* paInt8: */           PA_USE_CONVERTER_( UInt8, Int8 ),
                                          /* paUInt8: */          PA_UNITY_CONVERSION_( 8 )
                                        )
                     )
}

/* -------------------------------------------------------------------------- */

#ifdef PA_NO_STANDARD_CONVERTERS

/* -------------------------------------------------------------------------- */

PaUtilConverterTable paConverters = {
    0, /* PaUtilConverter *Float32_To_Int32; */
    0, /* PaUtilConverter *Float32_To_Int32_Dither; */
    0, /* PaUtilConverter *Float32_To_Int32_Clip; */
    0, /* PaUtilConverter *Float32_To_Int32_DitherClip; */

    0, /* PaUtilConverter *Float32_To_Int24; */
    0, /* PaUtilConverter *Float32_To_Int24_Dither; */
    0, /* PaUtilConverter *Float32_To_Int24_Clip; */
    0, /* PaUtilConverter *Float32_To_Int24_DitherClip; */

    0, /* PaUtilConverter *Float32_To_Int16; */
    0, /* PaUtilConverter *Float32_To_Int16_Dither; */
    0, /* PaUtilConverter *Float32_To_Int16_Clip; */
    0, /* PaUtilConverter *Float32_To_Int16_DitherClip; */

    0, /* PaUtilConverter *Float32_To_Int8; */
    0, /* PaUtilConverter *Float32_To_Int8_Dither; */
    0, /* PaUtilConverter *Float32_To_Int8_Clip; */
    0, /* PaUtilConverter *Float32_To_Int8_DitherClip; */

    0, /* PaUtilConverter *Float32_To_UInt8; */
    0, /* PaUtilConverter *Float32_To_UInt8_Dither; */
    0, /* PaUtilConverter *Float32_To_UInt8_Clip; */
    0, /* PaUtilConverter *Float32_To_UInt8_DitherClip; */

    0, /* PaUtilConverter *Int32_To_Float32; */
    0, /* PaUtilConverter *Int32_To_Int24; */
    0, /* PaUtilConverter *Int32_To_Int24_Dither; */
    0, /* PaUtilConverter *Int32_To_Int16; */
    0, /* PaUtilConverter *Int32_To_Int16_Dither; */
    0, /* PaUtilConverter *Int32_To_Int8; */
    0, /* PaUtilConverter *Int32_To_Int8_Dither; */
    0, /* PaUtilConverter *Int32_To_UInt8; */
    0, /* PaUtilConverter *Int32_To_UInt8_Dither; */

    0, /* PaUtilConverter *Int24_To_Float32; */
    0, /* PaUtilConverter *Int24_To_Int32; */
    0, /* PaUtilConverter *Int24_To_Int16; */
    0, /* PaUtilConverter *Int24_To_Int16_Dither; */
    0, /* PaUtilConverter *Int24_To_Int8; */
    0, /* PaUtilConverter *Int24_To_Int8_Dither; */
    0, /* PaUtilConverter *Int24_To_UInt8; */
    0, /* PaUtilConverter *Int24_To_UInt8_Dither; */
    
    0, /* PaUtilConverter *Int16_To_Float32; */
    0, /* PaUtilConverter *Int16_To_Int32; */
    0, /* PaUtilConverter *Int16_To_Int24; */
    0, /* PaUtilConverter *Int16_To_Int8; */
    0, /* PaUtilConverter *Int16_To_Int8_Dither; */
    0, /* PaUtilConverter *Int16_To_UInt8; */
    0, /* PaUtilConverter *Int16_To_UInt8_Dither; */

    0, /* PaUtilConverter *Int8_To_Float32; */
    0, /* PaUtilConverter *Int8_To_Int32; */
    0, /* PaUtilConverter *Int8_To_Int24 */
    0, /* PaUtilConverter *Int8_To_Int16; */
    0, /* PaUtilConverter *Int8_To_UInt8; */

    0, /* PaUtilConverter *UInt8_To_Float32; */
    0, /* PaUtilConverter *UInt8_To_Int32; */
    0, /* PaUtilConverter *UInt8_To_Int24; */
    0, /* PaUtilConverter *UInt8_To_Int16; */
    0, /* PaUtilConverter *UInt8_To_Int8; */

    0, /* PaUtilConverter *Copy_8_To_8; */
    0, /* PaUtilConverter *Copy_16_To_16; */
    0, /* PaUtilConverter *Copy_24_To_24; */
    0  /* PaUtilConverter *Copy_32_To_32; */
};

/* -------------------------------------------------------------------------- */

#else /* PA_NO_STANDARD_CONVERTERS is not defined */

/* -------------------------------------------------------------------------- */

#define PA_CLIP_( val, min, max )\
    { val = ((val) < (min)) ? (min) : (((val) > (max)) ? (max) : (val)); }


static const float const_1_div_128_ = 1.0f / 128.0f;  /* 8 bit multiplier */

static const float const_1_div_32768_ = 1.0f / 32768.f; /* 16 bit multiplier */

static const double const_1_div_2147483648_ = 1.0 / 2147483648.0; /* 32 bit multiplier */

/* -------------------------------------------------------------------------- */

static void Float32_To_Int32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {
        /* REVIEW */
        double scaled = *src * 0x7FFFFFFF;
        *dest = (signed long) scaled;
        
        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_To_Int32_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;

    while( count-- )
    {
        /* REVIEW */
        double dither  = PaUtil_GenerateFloatTriangularDither( ditherGenerator );
        /* use smaller scaler to prevent overflow when we add the dither */
        double dithered = ((double)*src * (2147483646.0)) + dither;
        *dest = (signed long) dithered;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_To_Int32_Clip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;
    (void) ditherGenerator; /* unused parameter */
    
    while( count-- )
    {
        /* REVIEW */
        double scaled = *src * 0x7FFFFFFF;
        PA_CLIP_( scaled, -2147483648., 2147483647.  );
        *dest = (signed long) scaled;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_To_Int32_DitherClip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed long *dest =  (signed long*)destinationBuffer;

    while( count-- )
    {
        /* REVIEW */
        double dither  = PaUtil_GenerateFloatTriangularDither( ditherGenerator );
        /* use smaller scaler to prevent overflow when we add the dither */
        double dithered = ((double)*src * (2147483646.0)) + dither;
        PA_CLIP_( dithered, -2147483648., 2147483647.  );
        *dest = (signed long) dithered;


        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_To_Int24(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    unsigned char *dest = (unsigned char*)destinationBuffer;
    signed long temp;

    (void) ditherGenerator; /* unused parameter */
    
    while( count-- )
    {
        /* convert to 32 bit and drop the low 8 bits */
        double scaled = *src * 0x7FFFFFFF;
        temp = (signed long) scaled;

        /* REVIEW, FIXME : this is little endian byte order */
        dest[0] = (unsigned char)(temp >> 24);
        dest[1] = (unsigned char)(temp >> 16);
        dest[2] = (unsigned char)(temp >> 8);

        src += sourceStride;
        dest += destinationStride * 3;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_To_Int24_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    unsigned char *dest = (unsigned char*)destinationBuffer;
    signed long temp;

    while( count-- )
    {
        /* convert to 32 bit and drop the low 8 bits */

        double dither  = PaUtil_GenerateFloatTriangularDither( ditherGenerator );
        /* use smaller scaler to prevent overflow when we add the dither */
        double dithered = ((double)*src * (2147483646.0)) + dither;
        
        temp = (signed long) dithered;

        /* REVIEW, FIXME : this is little endian byte order */
        dest[0] = (unsigned char)(temp >> 24);
        dest[1] = (unsigned char)(temp >> 16);
        dest[2] = (unsigned char)(temp >> 8);

        src += sourceStride;
        dest += destinationStride * 3;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_To_Int24_Clip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    unsigned char *dest = (unsigned char*)destinationBuffer;
    signed long temp;

    (void) ditherGenerator; /* unused parameter */
    
    while( count-- )
    {
        /* convert to 32 bit and drop the low 8 bits */
        double scaled = *src * 0x7FFFFFFF;
        PA_CLIP_( scaled, -2147483648., 2147483647.  );
        temp = (signed long) scaled;

        /* REVIEW, FIXME : this is little endian byte order */
        dest[0] = (unsigned char)(temp >> 24);
        dest[1] = (unsigned char)(temp >> 16);
        dest[2] = (unsigned char)(temp >> 8);

        src += sourceStride;
        dest += destinationStride * 3;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_To_Int24_DitherClip(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    unsigned char *dest = (unsigned char*)destinationBuffer;
    signed long temp;
    
    while( count-- )
    {
        /* convert to 32 bit and drop the low 8 bits */
        
        double dither  = PaUtil_GenerateFloatTriangularDither( ditherGenerator );
        /* use smaller scaler to prevent overflow when we add the dither */
        double dithered = ((double)*src * (2147483646.0)) + dither;
        PA_CLIP_( dithered, -2147483648., 2147483647.  );
        
        temp = (signed long) dithered;

        /* REVIEW, FIXME : this is little endian byte order */
        dest[0] = (unsigned char)(temp >> 24);
        dest[1] = (unsigned char)(temp >> 16);
        dest[2] = (unsigned char)(temp >> 8);

        src += sourceStride;
        dest += destinationStride * 3;
    }
}

/* -------------------------------------------------------------------------- */

static void Float32_To_Int16(
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

static void Float32_To_Int16_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    float *src = (float*)sourceBuffer;
    signed short *dest = (signed short*)destinationBuffer;

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

static void Float32_To_Int16_Clip(
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

static void Float32_To_Int16_DitherClip(
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

static void Float32_To_Int8(
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

static void Float32_To_Int8_Dither(
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

static void Float32_To_Int8_Clip(
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

static void Float32_To_Int8_DitherClip(
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

static void Float32_To_UInt8(
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

static void Float32_To_UInt8_Dither(
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

static void Float32_To_UInt8_Clip(
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

static void Float32_To_UInt8_DitherClip(
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

static void Int32_To_Float32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    signed long *src = (signed long*)sourceBuffer;
    float *dest =  (float*)destinationBuffer;
    (void)ditherGenerator; /* unused parameter */

    while( count-- )
    {
        *dest = (double)*src * const_1_div_2147483648_;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int32_To_Int24(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int32_To_Int24_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int32_To_Int16(
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

static void Int32_To_Int16_Dither(
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

static void Int32_To_Int8(
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

static void Int32_To_Int8_Dither(
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

static void Int32_To_UInt8(
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

static void Int32_To_UInt8_Dither(
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

static void Int24_To_Float32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    float *dest = (float*)destinationBuffer;
    signed long temp;

    (void) ditherGenerator; /* unused parameter */
    
    while( count-- )
    {
        /* REVIEW, FIXME: this is little endian byte order */
        temp = src[0] << 24;
        temp = src[1] << 16;
        temp = src[2] << 8;

        *dest = (double)temp * const_1_div_2147483648_;

        src += sourceStride * 3;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Int24_To_Int32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int24_To_Int16(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int24_To_Int16_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int24_To_Int8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int24_To_Int8_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int24_To_UInt8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int24_To_UInt8_Dither(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int16_To_Float32(
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

static void Int16_To_Int32(
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

static void Int16_To_Int24(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void Int16_To_Int8(
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

static void Int16_To_Int8_Dither(
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

static void Int16_To_UInt8(
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

static void Int16_To_UInt8_Dither(
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

static void Int8_To_Float32(
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

static void Int8_To_Int32(
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

static void Int8_To_Int24(
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
        dest += destinationStride * 3;
    }
}

/* -------------------------------------------------------------------------- */

static void Int8_To_Int16(
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

static void Int8_To_UInt8(
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

static void UInt8_To_Float32(
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

static void UInt8_To_Int32(
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

static void UInt8_To_Int24(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    (void) destinationBuffer; /* unused parameters */
    (void) destinationStride; /* unused parameters */
    (void) sourceBuffer; /* unused parameters */
    (void) sourceStride; /* unused parameters */
    (void) count; /* unused parameters */
    (void) ditherGenerator; /* unused parameters */
    /* IMPLEMENT ME */
}

/* -------------------------------------------------------------------------- */

static void UInt8_To_Int16(
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

static void UInt8_To_Int8(
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

static void Copy_8_To_8(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    unsigned char *dest = (unsigned char*)destinationBuffer;
                                                      
    (void) ditherGenerator; /* unused parameter */

    while( count-- )
    {
        *dest = *src;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Copy_16_To_16(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned short *src = (unsigned short*)sourceBuffer;
    unsigned short *dest = (unsigned short*)destinationBuffer;
                                                        
    (void) ditherGenerator; /* unused parameter */
    
    while( count-- )
    {
        *dest = *src;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

static void Copy_24_To_24(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    unsigned char *dest = (unsigned char*)destinationBuffer;

    (void) ditherGenerator; /* unused parameter */
    
    while( count-- )
    {
        dest[0] = src[0];
        dest[1] = src[1];
        dest[2] = src[2];

        src += sourceStride * 3;
        dest += destinationStride * 3;
    }
}

/* -------------------------------------------------------------------------- */

static void Copy_32_To_32(
    void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count, PaUtilTriangularDitherGenerator *ditherGenerator )
{
    unsigned long *dest = (unsigned long*)destinationBuffer;
    unsigned long *src = (unsigned long*)sourceBuffer;

    (void) ditherGenerator; /* unused parameter */
    
    while( count-- )
    {
        *dest = *src;

        src += sourceStride;
        dest += destinationStride;
    }
}

/* -------------------------------------------------------------------------- */

PaUtilConverterTable paConverters = {
    Float32_To_Int32,              /* PaUtilConverter *Float32_To_Int32; */
    Float32_To_Int32_Dither,       /* PaUtilConverter *Float32_To_Int32_Dither; */
    Float32_To_Int32_Clip,         /* PaUtilConverter *Float32_To_Int32_Clip; */
    Float32_To_Int32_DitherClip,   /* PaUtilConverter *Float32_To_Int32_DitherClip; */

    Float32_To_Int24,              /* PaUtilConverter *Float32_To_Int24; */
    Float32_To_Int24_Dither,       /* PaUtilConverter *Float32_To_Int24_Dither; */
    Float32_To_Int24_Clip,         /* PaUtilConverter *Float32_To_Int24_Clip; */
    Float32_To_Int24_DitherClip,   /* PaUtilConverter *Float32_To_Int24_DitherClip; */
    
    Float32_To_Int16,              /* PaUtilConverter *Float32_To_Int16; */
    Float32_To_Int16_Dither,       /* PaUtilConverter *Float32_To_Int16_Dither; */
    Float32_To_Int16_Clip,         /* PaUtilConverter *Float32_To_Int16_Clip; */
    Float32_To_Int16_DitherClip,   /* PaUtilConverter *Float32_To_Int16_DitherClip; */

    Float32_To_Int8,               /* PaUtilConverter *Float32_To_Int8; */
    Float32_To_Int8_Dither,        /* PaUtilConverter *Float32_To_Int8_Dither; */
    Float32_To_Int8_Clip,          /* PaUtilConverter *Float32_To_Int8_Clip; */
    Float32_To_Int8_DitherClip,    /* PaUtilConverter *Float32_To_Int8_DitherClip; */

    Float32_To_UInt8,              /* PaUtilConverter *Float32_To_UInt8; */
    Float32_To_UInt8_Dither,       /* PaUtilConverter *Float32_To_UInt8_Dither; */
    Float32_To_UInt8_Clip,         /* PaUtilConverter *Float32_To_UInt8_Clip; */
    Float32_To_UInt8_DitherClip,   /* PaUtilConverter *Float32_To_UInt8_DitherClip; */

    Int32_To_Float32,              /* PaUtilConverter *Int32_To_Float32; */
    Int32_To_Int24,                /* PaUtilConverter *Int32_To_Int24; */
    Int32_To_Int24_Dither,         /* PaUtilConverter *Int32_To_Int24_Dither; */
    Int32_To_Int16,                /* PaUtilConverter *Int32_To_Int16; */
    Int32_To_Int16_Dither,         /* PaUtilConverter *Int32_To_Int16_Dither; */
    Int32_To_Int8,                 /* PaUtilConverter *Int32_To_Int8; */
    Int32_To_Int8_Dither,          /* PaUtilConverter *Int32_To_Int8_Dither; */
    Int32_To_UInt8,                /* PaUtilConverter *Int32_To_UInt8; */
    Int32_To_UInt8_Dither,         /* PaUtilConverter *Int32_To_UInt8_Dither; */

    Int24_To_Float32,              /* PaUtilConverter *Int24_To_Float32; */
    Int24_To_Int32,                /* PaUtilConverter *Int24_To_Int32; */
    Int24_To_Int16,                /* PaUtilConverter *Int24_To_Int16; */
    Int24_To_Int16_Dither,         /* PaUtilConverter *Int24_To_Int16_Dither; */
    Int24_To_Int8,                 /* PaUtilConverter *Int24_To_Int8; */
    Int24_To_Int8_Dither,          /* PaUtilConverter *Int24_To_Int8_Dither; */
    Int24_To_UInt8,                /* PaUtilConverter *Int24_To_UInt8; */
    Int24_To_UInt8_Dither,         /* PaUtilConverter *Int24_To_UInt8_Dither; */

    Int16_To_Float32,              /* PaUtilConverter *Int16_To_Float32; */
    Int16_To_Int32,                /* PaUtilConverter *Int16_To_Int32; */
    Int16_To_Int24,                /* PaUtilConverter *Int16_To_Int24; */
    Int16_To_Int8,                 /* PaUtilConverter *Int16_To_Int8; */
    Int16_To_Int8_Dither,          /* PaUtilConverter *Int16_To_Int8_Dither; */
    Int16_To_UInt8,                /* PaUtilConverter *Int16_To_UInt8; */
    Int16_To_UInt8_Dither,         /* PaUtilConverter *Int16_To_UInt8_Dither; */

    Int8_To_Float32,               /* PaUtilConverter *Int8_To_Float32; */
    Int8_To_Int32,                 /* PaUtilConverter *Int8_To_Int32; */
    Int8_To_Int24,                 /* PaUtilConverter *Int8_To_Int24 */
    Int8_To_Int16,                 /* PaUtilConverter *Int8_To_Int16; */
    Int8_To_UInt8,                 /* PaUtilConverter *Int8_To_UInt8; */

    UInt8_To_Float32,              /* PaUtilConverter *UInt8_To_Float32; */
    UInt8_To_Int32,                /* PaUtilConverter *UInt8_To_Int32; */
    UInt8_To_Int24,                /* PaUtilConverter *UInt8_To_Int24; */
    UInt8_To_Int16,                /* PaUtilConverter *UInt8_To_Int16; */
    UInt8_To_Int8,                 /* PaUtilConverter *UInt8_To_Int8; */

    Copy_8_To_8,                   /* PaUtilConverter *Copy_8_To_8; */
    Copy_16_To_16,                 /* PaUtilConverter *Copy_16_To_16; */
    Copy_24_To_24,                 /* PaUtilConverter *Copy_24_To_24; */
    Copy_32_To_32                  /* PaUtilConverter *Copy_32_To_32; */
};

/* -------------------------------------------------------------------------- */

#endif /* PA_NO_STANDARD_CONVERTERS */
