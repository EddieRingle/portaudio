/*
 * $Id$
 * Portable Audio I/O Library
 * callback <-> host buffer processing adapter
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

#include <assert.h>

#include "pa_process.h"
#include "pa_util.h"



#define PA_FRAMES_PER_TEMP_BUFFER_WHEN_HOST_BUFFER_SIZE_IS_UNKNOWN_    1024



PaError PaUtil_InitializeBufferProcessor( PaUtilBufferProcessor* bp,
        int numInputChannels, PaSampleFormat userInputSampleFormat,
        PaSampleFormat hostInputSampleFormat,
        int numOutputChannels, PaSampleFormat userOutputSampleFormat,
        PaSampleFormat hostOutputSampleFormat,
        double sampleRate,
        PaStreamFlags streamFlags,
        unsigned long framesPerUserBuffer,
        unsigned long framesPerHostBuffer,
        PaUtilHostBufferSizeMode hostBufferSizeMode,
        PortAudioCallback *userCallback, void *userData )
{
    PaError result = paNoError;
    PaError bytesPerSample;
    unsigned long tempInputBufferSize, tempOutputBufferSize;

    /* initialize buffer ptrs to zero so they can be freed if necessary in error */
    bp->tempInputBuffer = 0;
    bp->tempInputBufferPtrs = 0;
    bp->tempOutputBuffer = 0;
    bp->tempOutputBufferPtrs = 0;

    bp->framesPerUserBuffer = framesPerUserBuffer;
    bp->framesPerHostBuffer = framesPerHostBuffer;

    bp->numInputChannels = numInputChannels;
    bp->numOutputChannels = numOutputChannels;

    bp->hostBufferSizeMode = hostBufferSizeMode;
    
    if( framesPerUserBuffer == 0 ) /* callback will accept any buffer size */
    {
        bp->useNonAdaptingProcess = 1;
        
        if( hostBufferSizeMode == paUtilFixedHostBufferSize
                || hostBufferSizeMode == paUtilBoundedHostBufferSize )
        {
            bp->framesPerTempBuffer = framesPerHostBuffer;
        }
        else /* unknown host buffer size */
        {
             bp->framesPerTempBuffer = PA_FRAMES_PER_TEMP_BUFFER_WHEN_HOST_BUFFER_SIZE_IS_UNKNOWN_;
        }
    }
    else
    {
        bp->framesPerTempBuffer = framesPerUserBuffer;

        if( framesPerHostBuffer % framesPerUserBuffer == 0 )
        {
            bp->useNonAdaptingProcess = 1;
        }
        else
        {
            bp->useNonAdaptingProcess = 0;
        }
    }



    if( numInputChannels > 0 )
    {
        bytesPerSample = Pa_GetSampleSize( hostInputSampleFormat );
        if( bytesPerSample > 0 )
        {
            bp->bytesPerHostInputSample = bytesPerSample;
        }
        else
        {
            result = bytesPerSample;
            goto error;
        }

        bytesPerSample = Pa_GetSampleSize( userInputSampleFormat );
        if( bytesPerSample > 0 )
        {
            bp->bytesPerUserInputSample = bytesPerSample;
        }
        else
        {
            result = bytesPerSample;
            goto error;
        }

        bp->inputConverter =
            PaUtil_SelectConverter( hostInputSampleFormat, userInputSampleFormat, streamFlags );

            
        bp->userInputIsInterleaved = (userInputSampleFormat & paNonInterleaved)?0:1;


        tempInputBufferSize =
            bp->framesPerTempBuffer * bp->bytesPerUserInputSample * numInputChannels;
         
        bp->tempInputBuffer = PaUtil_AllocateMemory( tempInputBufferSize );
        if( bp->tempInputBuffer == 0 )
        {
            result = paInsufficientMemory;
            goto error;
        }

        //if( userInputSampleFormat & paNonInterleaved )
        //{
        // allocate even if unused for now (code below depends on them being present
            bp->tempInputBufferPtrs =
                PaUtil_AllocateMemory( sizeof(void*)*numInputChannels );
            if( bp->tempInputBufferPtrs == 0 )
            {
                result = paInsufficientMemory;
                goto error;
            }
        //}
    }

    if( numOutputChannels > 0 )
    {
        bytesPerSample = Pa_GetSampleSize( hostOutputSampleFormat );
        if( bytesPerSample > 0 )
        {
            bp->bytesPerHostOutputSample = bytesPerSample;
        }
        else
        {
            result = bytesPerSample;
            goto error;
        }

        bytesPerSample = Pa_GetSampleSize( userOutputSampleFormat );
        if( bytesPerSample > 0 )
        {
            bp->bytesPerUserOutputSample = bytesPerSample;
        }
        else
        {
            result = bytesPerSample;
            goto error;
        }

        bp->outputConverter =
            PaUtil_SelectConverter( userOutputSampleFormat, hostOutputSampleFormat, streamFlags );


        bp->userOutputIsInterleaved = (userOutputSampleFormat & paNonInterleaved)?0:1;

        tempOutputBufferSize =
                bp->framesPerTempBuffer * bp->bytesPerUserOutputSample * numOutputChannels;
        
        bp->tempOutputBuffer = PaUtil_AllocateMemory( tempOutputBufferSize );
        if( bp->tempOutputBuffer == 0 )
        {
            result = paInsufficientMemory;
            goto error;
        }


        //if( userOutputSampleFormat & paNonInterleaved )
        //{
        // allocate even if unused for now (code below depends on them being present
            bp->tempOutputBufferPtrs =
                PaUtil_AllocateMemory( sizeof(void*)*numOutputChannels );
            if( bp->tempOutputBufferPtrs == 0 )
            {
                result = paInsufficientMemory;
                goto error;
            }
        //}
    }

    PaUtil_InitializeTriangularDitherState( &bp->ditherGenerator );

    bp->samplePeriod = 1. / sampleRate;

    bp->userCallback = userCallback;
    bp->userData = userData;

    return result;

error:
    if( bp->tempInputBuffer )
        PaUtil_FreeMemory( bp->tempInputBuffer );

    if( bp->tempInputBufferPtrs )
        PaUtil_FreeMemory( bp->tempInputBufferPtrs );

    if( bp->tempOutputBuffer )
        PaUtil_FreeMemory( bp->tempOutputBuffer );

    if( bp->tempOutputBufferPtrs )
        PaUtil_FreeMemory( bp->tempOutputBufferPtrs );

    return result;
}


void PaUtil_TerminateBufferProcessor( PaUtilBufferProcessor* bp )
{
    if( bp->tempInputBuffer )
        PaUtil_FreeMemory( bp->tempInputBuffer );

    if( bp->tempInputBufferPtrs )
        PaUtil_FreeMemory( bp->tempInputBufferPtrs );

    if( bp->tempOutputBuffer )
        PaUtil_FreeMemory( bp->tempOutputBuffer );

    if( bp->tempOutputBufferPtrs )
        PaUtil_FreeMemory( bp->tempOutputBufferPtrs );
}


void PaUtil_BeginBufferProcessing( PaUtilBufferProcessor* bp, PaTimestamp outTime )
{
    bp->hostOutTime = outTime;

    bp->hostInputFrameCount2 = 0;
    bp->hostOutputFrameCount2 = 0;
}


static void NonAdaptingProcess( PaUtilBufferProcessor *bp,
        unsigned long *framesProcessed, int *callbackResult,
        PaUtilChannelDescriptor *hostInputChannels,
        PaUtilChannelDescriptor *hostOutputChannels,
        unsigned long frameCount );
        




unsigned long PaUtil_EndBufferProcessing( PaUtilBufferProcessor* bp, int *callbackResult )
{
    void *userInput, *userOutput;
    unsigned char *srcBytePtr, *destBytePtr;
    unsigned int i;
    unsigned long frameCount, framesToGo;
    unsigned int destStride, destBytePtrStride;
    unsigned int srcStride, srcBytePtrStride;
    unsigned long framesProcessed = 0;
    
    if( bp->numInputChannels != 0 && bp->numOutputChannels != 0 )
    {
        assert( (bp->hostInputFrameCount + bp->hostInputFrameCount2) ==
                (bp->hostOutputFrameCount + bp->hostOutputFrameCount2) );
    }

    
    if( bp->useNonAdaptingProcess )
    {
        /* process first buffer */
        framesToGo = (bp->numInputChannels != 0)
                        ? bp->hostInputFrameCount
                        : bp->hostOutputFrameCount;
        do
        {
            frameCount = ( bp->framesPerTempBuffer < framesToGo )
                        ? bp->framesPerTempBuffer
                        : framesToGo;

            NonAdaptingProcess( bp, &framesProcessed, callbackResult,
                    bp->hostInputChannels, bp->hostOutputChannels,
                    frameCount );

            framesToGo -= frameCount;
        }
        while( framesToGo > 0 );


        /* process second buffer if provided */
        framesToGo = (bp->numInputChannels != 0)
                        ? bp->hostInputFrameCount2
                        : bp->hostOutputFrameCount2;

        while( framesToGo > 0 )
        {
            frameCount = ( bp->framesPerTempBuffer < framesToGo )
                        ? bp->framesPerTempBuffer
                        : framesToGo;

            NonAdaptingProcess( bp, &framesProcessed, callbackResult,
                bp->hostInputChannels2, bp->hostOutputChannels2,
                frameCount );      

            framesToGo -= frameCount;
        }
    }
    else /* block adaption necessary*/
    {



    }

    return framesProcessed;
}


void PaUtil_SetInputFrameCount( PaUtilBufferProcessor* bp,
        unsigned long frameCount )
{
    if( frameCount == 0 )
        bp->hostInputFrameCount = bp->framesPerHostBuffer;
    else
        bp->hostInputFrameCount = frameCount;
}
        

void PaUtil_SetInputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data, unsigned int stride )
{
    bp->hostInputChannels[channel].data = data;
    bp->hostInputChannels[channel].stride = stride;
}


void PaUtil_SetInterleavedInputChannels( PaUtilBufferProcessor* bp,
        unsigned int firstChannel, void *data, unsigned int channelCount )
{
    unsigned int i;
    unsigned int channel = firstChannel;
    unsigned char *p = (unsigned char*)data;

    if( channelCount == 0 )
        channelCount = bp->numInputChannels;

    for( i=0; i< channelCount; ++i )
    {
        bp->hostInputChannels[channel+i].data = p;
        p += bp->bytesPerHostInputSample;
        bp->hostInputChannels[channel+i].stride = channelCount;
    }
}


void PaUtil_SetNonInterleavedInputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data )
{
    bp->hostInputChannels[channel].data = data;
    bp->hostInputChannels[channel].stride = 1;
}


void PaUtil_Set2ndInputFrameCount( PaUtilBufferProcessor* bp,
        unsigned long frameCount )
{
    bp->hostInputFrameCount2 = frameCount;
}


void PaUtil_Set2ndInputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data, unsigned int stride )
{
    bp->hostInputChannels2[channel].data = data;
    bp->hostInputChannels2[channel].stride = stride;
}


void PaUtil_Set2ndInterleavedInputChannels( PaUtilBufferProcessor* bp,
        unsigned int firstChannel, void *data, unsigned int channelCount )
{
    unsigned int i;
    unsigned int channel = firstChannel;
    unsigned char *p = (unsigned char*)data;

    if( channelCount == 0 )
        channelCount = bp->numInputChannels;

    for( i=0; i< channelCount; ++i )
    {
        bp->hostInputChannels2[channel+i].data = p;
        p += bp->bytesPerHostInputSample;
        bp->hostInputChannels2[channel+i].stride = channelCount;
    }
}

        
void PaUtil_Set2ndNonInterleavedInputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data )
{
    bp->hostInputChannels2[channel].data = data;
    bp->hostInputChannels2[channel].stride = 1;
}


void PaUtil_SetOutputFrameCount( PaUtilBufferProcessor* bp,
        unsigned long frameCount )
{
    if( frameCount == 0 )
        bp->hostOutputFrameCount = bp->framesPerHostBuffer;
    else
        bp->hostOutputFrameCount = frameCount;
}


void PaUtil_SetOutputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data, unsigned int stride )
{
    bp->hostOutputChannels[channel].data = data;
    bp->hostOutputChannels[channel].stride = stride;
}


void PaUtil_SetInterleavedOutputChannels( PaUtilBufferProcessor* bp,
        unsigned int firstChannel, void *data, unsigned int channelCount )
{
    unsigned int i;
    unsigned int channel = firstChannel;
    unsigned char *p = (unsigned char*)data;

    if( channelCount == 0 )
        channelCount = bp->numOutputChannels;

    for( i=0; i< channelCount; ++i )
    {
        bp->hostOutputChannels[channel+i].data = p;
        p += bp->bytesPerHostOutputSample;
        bp->hostOutputChannels[channel+i].stride = channelCount;
    }
}


void PaUtil_SetNonInterleavedOutputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data )
{
    bp->hostOutputChannels[channel].data = data;
    bp->hostOutputChannels[channel].stride = 1;
}


void PaUtil_Set2ndOutputFrameCount( PaUtilBufferProcessor* bp,
        unsigned long frameCount )
{
    bp->hostOutputFrameCount2 = frameCount;
}


void PaUtil_Set2ndOutputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data, unsigned int stride )
{
    bp->hostOutputChannels2[channel].data = data;
    bp->hostOutputChannels2[channel].stride = stride; 
}


void PaUtil_Set2ndInterleavedOutputChannels( PaUtilBufferProcessor* bp,
        unsigned int firstChannel, void *data, unsigned int channelCount )
{
    unsigned int i;
    unsigned int channel = firstChannel;
    unsigned char *p = (unsigned char*)data;

    if( channelCount == 0 )
        channelCount = bp->numOutputChannels;
        
    for( i=0; i< channelCount; ++i )
    {
        bp->hostOutputChannels2[channel+i].data = p;
        p += bp->bytesPerHostOutputSample;
        bp->hostOutputChannels2[channel+i].stride = channelCount;
    }
}

        
void PaUtil_Set2ndNonInterleavedOutputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data )
{
    bp->hostOutputChannels2[channel].data = data;
    bp->hostOutputChannels2[channel].stride = 1;
}


static void Copy1( void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    unsigned char *dest = (unsigned char*)destinationBuffer;

    while( count-- )
    {
        *dest = *src;
        src += sourceStride;
        dest += destinationStride;
    }
}


static void Copy2( void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count )
{
    unsigned short *src = (unsigned short*)sourceBuffer;
    unsigned short *dest = (unsigned short*)destinationBuffer;

    while( count-- )
    {
        *dest = *src;
        src += sourceStride;
        dest += destinationStride;
    }
}


static void Copy3( void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count )
{
    unsigned char *src = (unsigned char*)sourceBuffer;
    unsigned char *dest = (unsigned char*)destinationBuffer;

    while( count-- )
    {
        dest[0] = src[0];
        dest[1] = src[1];
        dest[2] = src[2];

        src += sourceStride * 3;
        dest += destinationStride * 3;
    }
}


static void Copy4( void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int count )
{
    unsigned long *src = (unsigned long*)sourceBuffer;
    unsigned long *dest = (unsigned long*)destinationBuffer;

    while( count-- )
    {
        *dest = *src;
        src += sourceStride;
        dest += destinationStride;
    }
}


static void NonAdaptingProcess( PaUtilBufferProcessor *bp,
        unsigned long *framesProcessed, int *callbackResult,
        PaUtilChannelDescriptor *hostInputChannels,
        PaUtilChannelDescriptor *hostOutputChannels,
        unsigned long frameCount )
{
    void *userInput, *userOutput;
    unsigned char *srcBytePtr, *destBytePtr;
    unsigned int srcStride, srcBytePtrStride;
    unsigned int destStride, destBytePtrStride;
    unsigned int i;

    
    /* configure user input buffer and convert input data (host -> user) */
    if( bp->numInputChannels == 0 )
    {
        /* no input */
        userInput = 0;
    }
    else /* there are input channels */
    {
        /*
            could use more elaborate logic here and sometimes process
            buffers in-place.
        */
            
        destBytePtr = bp->tempInputBuffer;

        if( bp->userInputIsInterleaved )
        {
            destStride = bp->numInputChannels;
            destBytePtrStride = bp->bytesPerUserInputSample;
            userInput = bp->tempInputBuffer;
        }
        else /* user input is not interleaved */
        {
            destStride = 1;
            destBytePtrStride = frameCount * bp->bytesPerUserInputSample;
            userInput = bp->tempInputBufferPtrs;
        }

        if( bp->inputConverter )
        {
            for( i=0; i<bp->numInputChannels; ++i )
            {
                bp->inputConverter( destBytePtr, destStride,
                                        hostInputChannels[i].data,
                                        hostInputChannels[i].stride,
                                        frameCount, &bp->ditherGenerator );

                bp->tempInputBufferPtrs[i] = destBytePtr; /* setup non-interleaved ptr (even if this is the interleaved case) */

                destBytePtr += destBytePtrStride;  /* skip to next destination channel */

                /* advance src ptr for next iteration */
                hostInputChannels[i].data = ((unsigned char*)hostInputChannels[i].data) +
                        frameCount * hostInputChannels[i].stride * bp->bytesPerHostInputSample;
            }
        }
        else /* no input converter, host and user format are the same */
        {
            /* we can optimize this in cases where the user input and host input
                have the same interleave. But for now we assume the worst and
                copy to the temp buffer */

            for( i=0; i<bp->numInputChannels; ++i )
            {
                switch( bp->bytesPerHostInputSample )
                {
                    case 1:
                        Copy1( destBytePtr, destStride,
                                    hostInputChannels[i].data,
                                    hostInputChannels[i].stride,
                                    frameCount );
                        break;
                    case 2:
                        Copy2( destBytePtr, destStride,
                                    hostInputChannels[i].data,
                                    hostInputChannels[i].stride,
                                    frameCount );
                        break;
                    case 3:
                        Copy3( destBytePtr, destStride,
                                    hostInputChannels[i].data,
                                    hostInputChannels[i].stride,
                                    frameCount );
                        break;
                    case 4:
                        Copy4( destBytePtr, destStride,
                                    hostInputChannels[i].data,
                                    hostInputChannels[i].stride,
                                    frameCount );
                        break;
                }


                bp->tempInputBufferPtrs[i] = destBytePtr; /* setup non-interleaved ptr (even if this is the interleaved case) */

                destBytePtr += destBytePtrStride;  /* skip to next destination channel */

                /* advance src ptr for next iteration */
                hostInputChannels[i].data = ((unsigned char*)hostInputChannels[i].data) +
                        frameCount * hostInputChannels[i].stride * bp->bytesPerHostInputSample;
            }
        }
    }

    /* configure user output buffer */
    if( bp->numOutputChannels == 0 )
    {
        /* no output */
        userOutput = 0;
    }
    else /* there are output channels */
    {
        if( bp->userOutputIsInterleaved )
        {
            userOutput = bp->tempOutputBuffer;
        }
        else /* user output is not interleaved */
        {
            srcBytePtr = bp->tempOutputBuffer;
            srcBytePtrStride = frameCount * bp->bytesPerUserOutputSample;

            for( i = 0; i < bp->numOutputChannels; ++i )
            {
                bp->tempOutputBufferPtrs[i] = srcBytePtr;
                srcBytePtr += srcBytePtrStride;
            }
                
            userOutput = bp->tempOutputBufferPtrs;
        }
    }

    *callbackResult = bp->userCallback( userInput, userOutput,
                                        frameCount, bp->hostOutTime, bp->userData );

    bp->hostOutTime += frameCount * bp->samplePeriod;

    // FIXME: if callback result is abort, then abort!

    /* convert output data (user -> host) */
    if( bp->numOutputChannels != 0 )
    {
        /*
            could use more elaborate logic here and sometimes process
            buffers in-place.
        */
            
        srcBytePtr = bp->tempOutputBuffer;

        if( bp->userOutputIsInterleaved )
        {
            srcStride = bp->numOutputChannels;
            srcBytePtrStride = bp->bytesPerUserOutputSample;
        }
        else /* user output is not interleaved */
        {
            srcStride = 1;
            srcBytePtrStride = frameCount * bp->bytesPerUserOutputSample;
        }

        if( bp->outputConverter )
        {
            for( i=0; i<bp->numOutputChannels; ++i )
            {
                bp->outputConverter(    hostOutputChannels[i].data,
                                        hostOutputChannels[i].stride,
                                        srcBytePtr, srcStride,
                                        frameCount, &bp->ditherGenerator );

                srcBytePtr += srcBytePtrStride;  /* skip to next source channel */

                /* advance dest ptr for next iteration */
                hostOutputChannels[i].data = ((unsigned char*)hostOutputChannels[i].data) +
                        frameCount * hostOutputChannels[i].stride * bp->bytesPerHostOutputSample;
            }
        }
        else /* no input converter, host and user format are the same */
        {
            /* we can optimize this in cases where the user input and host input
                have the same interleave. But for now we assume the worst and
                copy to the temp buffer */

            for( i=0; i<bp->numInputChannels; ++i )
            {
                switch( bp->bytesPerHostInputSample )
                {
                    case 1:
                        Copy1(      hostOutputChannels[i].data,
                                    hostOutputChannels[i].stride,
                                    srcBytePtr, srcStride,
                                    frameCount );
                        break;
                    case 2:
                        Copy2(      hostOutputChannels[i].data,
                                    hostOutputChannels[i].stride,
                                    srcBytePtr, srcStride,
                                    frameCount );
                        break;
                    case 3:
                        Copy3(      hostOutputChannels[i].data,
                                    hostOutputChannels[i].stride,
                                    srcBytePtr, srcStride,
                                    frameCount );
                        break;
                    case 4:
                        Copy4(      hostOutputChannels[i].data,
                                    hostOutputChannels[i].stride,
                                    srcBytePtr, srcStride,
                                    frameCount );
                        break;
                }

                srcBytePtr += srcBytePtrStride;  /* skip to next source channel */

                /* advance dest ptr for next iteration */
                hostOutputChannels[i].data = ((unsigned char*)hostOutputChannels[i].data) +
                        frameCount * hostOutputChannels[i].stride * bp->bytesPerHostOutputSample;
            }
        }
    }

    *framesProcessed += frameCount;
}
