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


