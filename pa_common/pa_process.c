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
#include <string.h> /* memset() */

#include "pa_process.h"
#include "pa_util.h"

/*
    The code in this file is not optimised yet. there may appear to be redundancies
    that could be factored into common functions, but the redundanceis are left
    intentionally as each appearance may have different optimisation possibilities.

    The optimisations which are planned involve only converting data in-place
    where possible, rather than copying to the temp buffer(s).

    Cache tilings for intereave<->deinterleave also need to be considered.
*/


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
        bp->framesInTempInputBuffer = 0;
        bp->framesInTempOutputBuffer = 0;

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

        if( hostBufferSizeMode == paUtilFixedHostBufferSize
                && framesPerHostBuffer % framesPerUserBuffer == 0 )
        {
            bp->useNonAdaptingProcess = 1;
            bp->framesInTempInputBuffer = 0;
            bp->framesInTempOutputBuffer = 0;
        }
        else
        {
            bp->useNonAdaptingProcess = 0;

            if( numInputChannels > 0 && numOutputChannels > 0 )
            {
                /* full duplex */
                if( hostBufferSizeMode == paUtilFixedHostBufferSize )
                {
                    if( framesPerUserBuffer > framesPerHostBuffer )
                    {
                        bp->framesInTempInputBuffer = framesPerUserBuffer; /* FIXME: possibly add less than one buffer's worth of latency as per stephane's pdf*/
                        bp->framesInTempOutputBuffer = 0;
                    }
                    else
                    {
                        bp->framesInTempInputBuffer = 0;
                        bp->framesInTempOutputBuffer = framesPerUserBuffer;   /* FIXME: possibly add less than one buffer's worth of latency as per stephane's pdf*/
                    } 
                }
                else /* variable host buffer size, add framesPerUserBuffer latency */
                {
                    bp->framesInTempInputBuffer = 0;
                    bp->framesInTempOutputBuffer = framesPerUserBuffer;
                }
            }
            else
            {
                /* half duplex */
                bp->framesInTempInputBuffer = 0;
                bp->framesInTempOutputBuffer = 0;
            }
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
        if( bp->framesInTempInputBuffer > 0 )
            memset( bp->tempInputBuffer, 0, tempInputBufferSize );


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

        if( bp->framesInTempOutputBuffer > 0 )
            memset( bp->tempOutputBuffer, 0, tempOutputBufferSize );

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
    /* the first callback will be called to generate samples which will be
        outputted after the frames currently in the output buffer have been
        outputted. */
    bp->hostOutTime = outTime + bp->framesInTempOutputBuffer * bp->samplePeriod;;

    bp->hostInputFrameCount[1] = 0;
    bp->hostOutputFrameCount[1] = 0;
}


static unsigned long NonAdaptingProcess( PaUtilBufferProcessor *bp,
        int *callbackResult,
        PaUtilChannelDescriptor *hostInputChannels,
        PaUtilChannelDescriptor *hostOutputChannels,
        unsigned long frameCount );

static unsigned long AdaptingInputOnlyProcess( PaUtilBufferProcessor *bp,
        int *callbackResult,
        PaUtilChannelDescriptor *hostInputChannels,
        unsigned long frameCount );

static unsigned long AdaptingOutputOnlyProcess( PaUtilBufferProcessor *bp,
        int *callbackResult,
        PaUtilChannelDescriptor *hostOutputChannels,
        unsigned long framesToProcess );

static unsigned long PartialFillProcess( PaUtilBufferProcessor *bp,
        int *callbackResult );

static unsigned long AdaptingProcess( PaUtilBufferProcessor *bp,
        int *callbackResult );


unsigned long PaUtil_EndBufferProcessing( PaUtilBufferProcessor* bp, int *callbackResult )
{
    unsigned long framesToProcess;
    unsigned long framesProcessed = 0;
    
    if( bp->numInputChannels != 0 && bp->numOutputChannels != 0 )
    {
        assert( (bp->hostInputFrameCount[0] + bp->hostInputFrameCount[1]) ==
                (bp->hostOutputFrameCount[0] + bp->hostOutputFrameCount[1]) );
    }

    
    if( bp->useNonAdaptingProcess )
    {
        /* process first buffer */
        
        framesToProcess = (bp->numInputChannels != 0)
                        ? bp->hostInputFrameCount[0]
                        : bp->hostOutputFrameCount[0];

        framesProcessed = NonAdaptingProcess( bp, callbackResult,
                    bp->hostInputChannels[0], bp->hostOutputChannels[0],
                    framesToProcess );

        /* process second buffer if provided */
        
        framesToProcess = (bp->numInputChannels != 0)
                        ? bp->hostInputFrameCount[1]
                        : bp->hostOutputFrameCount[1];
        if( framesToProcess > 0 )
        {
            framesProcessed += NonAdaptingProcess( bp, callbackResult,
                bp->hostInputChannels[1], bp->hostOutputChannels[1],
                framesToProcess );      
        }
    }
    else /* block adaption necessary*/
    {

        if( bp->numInputChannels != 0 && bp->numOutputChannels != 0 )
        {
            /* full duplex */
            
            if( bp->hostBufferSizeMode == paUtilVariableHostBufferSizePartialUsageAllowed  )
            {
                framesProcessed = PartialFillProcess( bp, callbackResult );
            }
            else
            {
                framesProcessed = AdaptingProcess( bp, callbackResult );
            }
        }
        else if( bp->numInputChannels != 0 )
        {
            /* input only */
            framesToProcess = bp->hostInputFrameCount[0];

            framesProcessed = AdaptingInputOnlyProcess( bp, callbackResult,
                        bp->hostInputChannels[0], framesToProcess );

            framesToProcess = bp->hostInputFrameCount[1];
            if( framesToProcess > 0 )
            {
                framesProcessed += AdaptingInputOnlyProcess( bp, callbackResult,
                        bp->hostInputChannels[1], framesToProcess );
            }
        }
        else
        {
            /* output only */
            framesToProcess = bp->hostOutputFrameCount[0];

            framesProcessed = AdaptingOutputOnlyProcess( bp, callbackResult,
                        bp->hostOutputChannels[0], framesToProcess );

            framesToProcess = bp->hostOutputFrameCount[1];
            if( framesToProcess > 0 )
            {
                framesProcessed += AdaptingOutputOnlyProcess( bp, callbackResult,
                        bp->hostOutputChannels[1], framesToProcess );
            }
        }
    }

    return framesProcessed;
}


void PaUtil_SetInputFrameCount( PaUtilBufferProcessor* bp,
        unsigned long frameCount )
{
    if( frameCount == 0 )
        bp->hostInputFrameCount[0] = bp->framesPerHostBuffer;
    else
        bp->hostInputFrameCount[0] = frameCount;
}
        

void PaUtil_SetInputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data, unsigned int stride )
{
    bp->hostInputChannels[0][channel].data = data;
    bp->hostInputChannels[0][channel].stride = stride;
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
        bp->hostInputChannels[0][channel+i].data = p;
        p += bp->bytesPerHostInputSample;
        bp->hostInputChannels[0][channel+i].stride = channelCount;
    }
}


void PaUtil_SetNonInterleavedInputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data )
{
    bp->hostInputChannels[0][channel].data = data;
    bp->hostInputChannels[0][channel].stride = 1;
}


void PaUtil_Set2ndInputFrameCount( PaUtilBufferProcessor* bp,
        unsigned long frameCount )
{
    bp->hostInputFrameCount[1] = frameCount;
}


void PaUtil_Set2ndInputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data, unsigned int stride )
{
    bp->hostInputChannels[1][channel].data = data;
    bp->hostInputChannels[1][channel].stride = stride;
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
        bp->hostInputChannels[1][channel+i].data = p;
        p += bp->bytesPerHostInputSample;
        bp->hostInputChannels[1][channel+i].stride = channelCount;
    }
}

        
void PaUtil_Set2ndNonInterleavedInputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data )
{
    bp->hostInputChannels[1][channel].data = data;
    bp->hostInputChannels[1][channel].stride = 1;
}


void PaUtil_SetOutputFrameCount( PaUtilBufferProcessor* bp,
        unsigned long frameCount )
{
    if( frameCount == 0 )
        bp->hostOutputFrameCount[0] = bp->framesPerHostBuffer;
    else
        bp->hostOutputFrameCount[0] = frameCount;
}


void PaUtil_SetOutputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data, unsigned int stride )
{
    bp->hostOutputChannels[0][channel].data = data;
    bp->hostOutputChannels[0][channel].stride = stride;
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
        bp->hostOutputChannels[0][channel+i].data = p;
        p += bp->bytesPerHostOutputSample;
        bp->hostOutputChannels[0][channel+i].stride = channelCount;
    }
}


void PaUtil_SetNonInterleavedOutputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data )
{
    bp->hostOutputChannels[0][channel].data = data;
    bp->hostOutputChannels[0][channel].stride = 1;
}


void PaUtil_Set2ndOutputFrameCount( PaUtilBufferProcessor* bp,
        unsigned long frameCount )
{
    bp->hostOutputFrameCount[1] = frameCount;
}


void PaUtil_Set2ndOutputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data, unsigned int stride )
{
    bp->hostOutputChannels[1][channel].data = data;
    bp->hostOutputChannels[1][channel].stride = stride;
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
        bp->hostOutputChannels[1][channel+i].data = p;
        p += bp->bytesPerHostOutputSample;
        bp->hostOutputChannels[1][channel+i].stride = channelCount;
    }
}

        
void PaUtil_Set2ndNonInterleavedOutputChannel( PaUtilBufferProcessor* bp,
        unsigned int channel, void *data )
{
    bp->hostOutputChannels[1][channel].data = data;
    bp->hostOutputChannels[1][channel].stride = 1;
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


static void Copy( void *destinationBuffer, signed int destinationStride,
    void *sourceBuffer, signed int sourceStride,
    unsigned int frameCount, unsigned int bytesPerSample )
{
    switch( bytesPerSample )
    {
        case 1:
            Copy1( destinationBuffer, destinationStride,
                    sourceBuffer, sourceStride, frameCount );
            break;
        case 2:
            Copy2( destinationBuffer, destinationStride,
                    sourceBuffer, sourceStride, frameCount );
            break;
        case 3:
            Copy3( destinationBuffer, destinationStride,
                    sourceBuffer, sourceStride, frameCount );
            break;
        case 4:
            Copy4( destinationBuffer, destinationStride,
                    sourceBuffer, sourceStride, frameCount );
            break;
    }
}


static unsigned long NonAdaptingProcess( PaUtilBufferProcessor *bp,
        int *callbackResult,
        PaUtilChannelDescriptor *hostInputChannels,
        PaUtilChannelDescriptor *hostOutputChannels,
        unsigned long framesToProcess )
{
    void *userInput, *userOutput;
    unsigned char *srcBytePtr, *destBytePtr;
    unsigned int srcStride, srcBytePtrStride;
    unsigned int destStride, destBytePtrStride;
    unsigned int i;
    unsigned long frameCount;
    unsigned long framesToGo = framesToProcess;
    unsigned long framesProcessed = 0;
    
    do
    {
        frameCount = ( bp->framesPerTempBuffer < framesToGo )
                    ? bp->framesPerTempBuffer
                    : framesToGo;

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
                    Copy( destBytePtr, destStride,
                            hostInputChannels[i].data,
                            hostInputChannels[i].stride,
                            frameCount, bp->bytesPerHostInputSample );

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
                    Copy( hostOutputChannels[i].data,
                            hostOutputChannels[i].stride,
                            srcBytePtr, srcStride,
                            frameCount, bp->bytesPerHostInputSample );

                    srcBytePtr += srcBytePtrStride;  /* skip to next source channel */

                    /* advance dest ptr for next iteration */
                    hostOutputChannels[i].data = ((unsigned char*)hostOutputChannels[i].data) +
                            frameCount * hostOutputChannels[i].stride * bp->bytesPerHostOutputSample;
                }
            }
        }

        framesProcessed += frameCount;

        framesToGo -= frameCount;
    }
    while( framesToGo > 0 );

    return framesProcessed;
}


static unsigned long AdaptingInputOnlyProcess( PaUtilBufferProcessor *bp,
        int *callbackResult,
        PaUtilChannelDescriptor *hostInputChannels,
        unsigned long framesToProcess )
{
    void *userInput, *userOutput;
    unsigned char *destBytePtr;
    unsigned int destStride, destBytePtrStride;
    unsigned int i;
    unsigned long frameCount;
    unsigned long framesToGo = framesToProcess;
    unsigned long framesProcessed = 0;
    
    userOutput = 0;

    do
    {
        frameCount = ( bp->framesInTempInputBuffer + framesToGo > bp->framesPerUserBuffer )
                ? ( bp->framesPerUserBuffer - bp->framesInTempInputBuffer )
                : framesToGo;

        /* convert frameCount samples into temp buffer */

        if( bp->userInputIsInterleaved )
        {
            destBytePtr = ((unsigned char*)bp->tempInputBuffer) +
                    bp->bytesPerUserInputSample * bp->numInputChannels *
                    (bp->framesPerUserBuffer - bp->framesInTempInputBuffer );

            destStride = bp->numInputChannels;
            destBytePtrStride = bp->bytesPerUserInputSample;

            userInput = bp->tempInputBuffer;
        }
        else /* user input is not interleaved */
        {
            destBytePtr = ((unsigned char*)bp->tempInputBuffer) +
                    bp->bytesPerUserInputSample *
                    (bp->framesPerUserBuffer - bp->framesInTempInputBuffer );
                            
            destStride = 1;
            destBytePtrStride = bp->framesPerUserBuffer * bp->bytesPerUserInputSample;

            /* setup non-interleaved ptrs */
            for( i=0; i<bp->numInputChannels; ++i )
            {
                bp->tempInputBufferPtrs[i] = ((unsigned char*)bp->tempInputBuffer) +
                    i * bp->bytesPerUserInputSample * bp->framesPerUserBuffer;
            }
                    
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
                Copy( destBytePtr, destStride,
                        hostInputChannels[i].data,
                        hostInputChannels[i].stride,
                        frameCount, bp->bytesPerHostInputSample );

                destBytePtr += destBytePtrStride;  /* skip to next destination channel */

                /* advance src ptr for next iteration */
                hostInputChannels[i].data = ((unsigned char*)hostInputChannels[i].data) +
                        frameCount * hostInputChannels[i].stride * bp->bytesPerHostInputSample;
            }
        }

        bp->framesInTempInputBuffer += frameCount;

        if( bp->framesInTempInputBuffer == bp->framesPerUserBuffer )
        {
            *callbackResult = bp->userCallback( userInput, userOutput,
                                bp->framesPerUserBuffer, bp->hostOutTime, bp->userData );

            bp->hostOutTime += frameCount * bp->samplePeriod;  //FIXME, this is completely wrong for input only

            // FIXME: if callback result is abort, then abort!

            bp->framesInTempInputBuffer = 0;
        }

        framesProcessed += frameCount;

        framesToGo -= frameCount;
    }while( framesToGo > 0 );

    return framesProcessed;
}


static unsigned long AdaptingOutputOnlyProcess( PaUtilBufferProcessor *bp,
        int *callbackResult,
        PaUtilChannelDescriptor *hostOutputChannels,
        unsigned long framesToProcess )
{
    void *userInput, *userOutput;
    unsigned char *srcBytePtr;
    unsigned int srcStride, srcBytePtrStride;
    unsigned int i;
    unsigned long frameCount;
    unsigned long framesToGo = framesToProcess;
    unsigned long framesProcessed = 0;

    do
    {
        if( bp->framesInTempOutputBuffer == 0 )
        {
            userInput = 0;

            /* setup userOutput */
            if( bp->userOutputIsInterleaved )
            {
                userOutput = bp->tempOutputBuffer;
            }
            else /* user output is not interleaved */
            {
                for( i = 0; i < bp->numOutputChannels; ++i )
                {
                    bp->tempOutputBufferPtrs[i] = ((unsigned char*)bp->tempOutputBuffer) +
                            i * bp->framesPerUserBuffer * bp->bytesPerUserOutputSample;
                }

                userOutput = bp->tempOutputBufferPtrs;
            }

            *callbackResult = bp->userCallback( userInput, userOutput,
                    bp->framesPerUserBuffer, bp->hostOutTime, bp->userData );

            bp->hostOutTime += bp->framesPerUserBuffer * bp->samplePeriod;

            // FIXME: if callback result is abort, then abort!

            bp->framesInTempOutputBuffer = bp->framesPerUserBuffer;
        }

        frameCount = ( bp->framesInTempOutputBuffer > framesToGo )
                     ? framesToGo
                     : bp->framesInTempOutputBuffer;

        /* convert frameCount frames from user buffer to host buffer */

        if( bp->userOutputIsInterleaved )
        {
            srcBytePtr = ((unsigned char*)bp->tempOutputBuffer) +
                    bp->bytesPerUserOutputSample * bp->numOutputChannels *
                    (bp->framesPerUserBuffer - bp->framesInTempOutputBuffer);
                            
            srcStride = bp->numOutputChannels;
            srcBytePtrStride = bp->bytesPerUserOutputSample;
        }
        else /* user output is not interleaved */
        {
            srcBytePtr = ((unsigned char*)bp->tempOutputBuffer) +
                    bp->bytesPerUserOutputSample *
                    (bp->framesPerUserBuffer - bp->framesInTempOutputBuffer);
                            
            srcStride = 1;
            srcBytePtrStride = bp->framesPerUserBuffer * bp->bytesPerUserOutputSample;
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
                Copy( hostOutputChannels[i].data,
                        hostOutputChannels[i].stride,
                        srcBytePtr, srcStride,
                        frameCount, bp->bytesPerHostInputSample );

                srcBytePtr += srcBytePtrStride;  /* skip to next source channel */

                /* advance dest ptr for next iteration */
                hostOutputChannels[i].data = ((unsigned char*)hostOutputChannels[i].data) +
                        frameCount * hostOutputChannels[i].stride * bp->bytesPerHostOutputSample;
            }
        }

        bp->framesInTempOutputBuffer -= frameCount;

        framesProcessed += frameCount;
        
        framesToGo -= frameCount;

    }while( framesToGo > 0 );

    return framesProcessed;
}


static unsigned long PartialFillProcess( PaUtilBufferProcessor *bp,
        int *callbackResult )
{
    void *userInput, *userOutput;
    unsigned long framesProcessed = 0;
    unsigned long framesAvailable;
    unsigned long maxFramesToCopy;
    unsigned long framesInTempInputBuffer, framesInTempOutputBuffer;
    PaUtilChannelDescriptor *hostInputChannels, *hostOutputChannels;
    unsigned int frameCount;
    unsigned char *srcBytePtr, *destBytePtr;
    unsigned int srcStride, srcBytePtrStride, destStride, destBytePtrStride;
    unsigned int i;

    framesAvailable = bp->hostInputFrameCount[0] + bp->hostInputFrameCount[1];/* this is assumed to be the same as the output buffers frame count */
    
    while( framesAvailable >= bp->framesPerUserBuffer )
    {
        framesInTempInputBuffer = 0;
        do{ /* copy frames from input buffers */
            maxFramesToCopy = bp->framesPerUserBuffer - framesInTempInputBuffer;

            /* select the input buffer set (1st or 2nd) */
            if( bp->hostInputFrameCount[0] > 0 )
            {
                hostInputChannels = bp->hostInputChannels[0];
                frameCount = (bp->hostInputFrameCount[0] < maxFramesToCopy)
                            ? bp->hostInputFrameCount[0]
                            : maxFramesToCopy;
            }
            else
            {
                hostInputChannels = bp->hostInputChannels[1];
                frameCount = (bp->hostInputFrameCount[1] < maxFramesToCopy)
                            ? bp->hostInputFrameCount[1]
                            : maxFramesToCopy;
            }

            /* configure conversion destination pointers */
            if( bp->userInputIsInterleaved )
            {
                destBytePtr = ((unsigned char*)bp->tempInputBuffer) +
                        bp->bytesPerUserInputSample * bp->numInputChannels *
                        (bp->framesPerUserBuffer - framesInTempInputBuffer );

                destStride = bp->numInputChannels;
                destBytePtrStride = bp->bytesPerUserInputSample;
            }
            else /* user input is not interleaved */
            {
                destBytePtr = ((unsigned char*)bp->tempInputBuffer) +
                        bp->bytesPerUserInputSample *
                        (bp->framesPerUserBuffer - bp->framesInTempInputBuffer );
                            
                destStride = 1;
                destBytePtrStride = bp->framesPerUserBuffer * bp->bytesPerUserInputSample;
            }

            if( bp->inputConverter )
            {
                for( i=0; i<bp->numInputChannels; ++i )
                {
                    bp->inputConverter( destBytePtr, destStride,
                                            hostInputChannels[i].data,
                                            hostInputChannels[i].stride,
                                            frameCount, &bp->ditherGenerator );

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
                    Copy( destBytePtr, destStride,
                            hostInputChannels[i].data,
                            hostInputChannels[i].stride,
                            frameCount, bp->bytesPerHostInputSample );

                    destBytePtr += destBytePtrStride;  /* skip to next destination channel */

                    /* advance src ptr for next iteration */
                    hostInputChannels[i].data = ((unsigned char*)hostInputChannels[i].data) +
                            frameCount * hostInputChannels[i].stride * bp->bytesPerHostInputSample;
                }
            }

            if( bp->hostInputFrameCount[0] > 0 )
                bp->hostInputFrameCount[0] -= frameCount;
            else
                bp->hostInputFrameCount[1] -= frameCount;
                
            bp->framesInTempInputBuffer += frameCount;

        }while( framesInTempInputBuffer < bp->framesPerUserBuffer );

        /* setup userInput */
        if( bp->userInputIsInterleaved )
        {
            userInput = bp->tempInputBuffer;
        }
        else
        {
            for( i = 0; i < bp->numInputChannels; ++i )
            {
                bp->tempInputBufferPtrs[i] = ((unsigned char*)bp->tempInputBuffer) +
                        i * bp->framesPerUserBuffer * bp->bytesPerUserInputSample;
            }

            userInput = bp->tempInputBufferPtrs;
        }

        /* setup userOutput */
        if( bp->userOutputIsInterleaved )
        {
            userOutput = bp->tempOutputBuffer;
        }
        else /* user output is not interleaved */
        {
            for( i = 0; i < bp->numOutputChannels; ++i )
            {
                bp->tempOutputBufferPtrs[i] = ((unsigned char*)bp->tempOutputBuffer) +
                        i * bp->framesPerUserBuffer * bp->bytesPerUserOutputSample;
            }

            userOutput = bp->tempOutputBufferPtrs;
        }

        /* call callback */

        *callbackResult = bp->userCallback( userInput, userOutput,
                                            frameCount, bp->hostOutTime, bp->userData );

        bp->hostOutTime += frameCount * bp->samplePeriod;

        // FIXME: if callback result is abort, then abort!
        

        framesInTempOutputBuffer = bp->framesPerUserBuffer;
        
        do /* copy frames to output buffers */
        {
            maxFramesToCopy = bp->framesPerUserBuffer - framesInTempInputBuffer;

            /* select the output buffer set (1st or 2nd) */
            if( bp->hostOutputFrameCount[0] > 0 )
            {
                hostOutputChannels = bp->hostOutputChannels[0];
                frameCount = (bp->hostOutputFrameCount[0] < maxFramesToCopy)
                            ? bp->hostOutputFrameCount[0]
                            : maxFramesToCopy;
            }
            else
            {
                hostOutputChannels = bp->hostOutputChannels[1];
                frameCount = (bp->hostOutputFrameCount[1] < maxFramesToCopy)
                            ? bp->hostOutputFrameCount[1]
                            : maxFramesToCopy;
            }

            if( bp->userOutputIsInterleaved )
            {
                srcBytePtr = ((unsigned char*)bp->tempOutputBuffer) +
                        bp->bytesPerUserOutputSample * bp->numOutputChannels *
                        (bp->framesPerUserBuffer - framesInTempOutputBuffer);
                            
                srcStride = bp->numOutputChannels;
                srcBytePtrStride = bp->bytesPerUserOutputSample;
            }
            else /* user output is not interleaved */
            {
                srcBytePtr = ((unsigned char*)bp->tempOutputBuffer) +
                        bp->bytesPerUserOutputSample *
                        (bp->framesPerUserBuffer - framesInTempOutputBuffer);
                            
                srcStride = 1;
                srcBytePtrStride = bp->framesPerUserBuffer * bp->bytesPerUserOutputSample;
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
                    Copy( hostOutputChannels[i].data,
                            hostOutputChannels[i].stride,
                            srcBytePtr, srcStride,
                            frameCount, bp->bytesPerHostInputSample );

                    srcBytePtr += srcBytePtrStride;  /* skip to next source channel */

                    /* advance dest ptr for next iteration */
                    hostOutputChannels[i].data = ((unsigned char*)hostOutputChannels[i].data) +
                            frameCount * hostOutputChannels[i].stride * bp->bytesPerHostOutputSample;
                }
            }

            framesInTempOutputBuffer -= frameCount;

        }while( framesInTempOutputBuffer > 0 );

        framesAvailable -= bp->framesPerUserBuffer;
        framesProcessed += bp->framesPerUserBuffer;
    }
    
    return framesProcessed;
}


static unsigned long AdaptingProcess( PaUtilBufferProcessor *bp,
        int *callbackResult )
{
    unsigned long framesProcessed = 0;

    // fill host output buffer
    // if there are samples in our ouput buffer, convert them to the real output buffer
    // until we have no more output samples, or the output buffer is full, whichever
    // comes first

    // fill user (temp) input buffer
    // while our input buffer is not full, fill it with samples from the host
    // input buffer.

    //if the host output buffer is empty, and the host input buffer is full
    // call callback

    // repeat until all input and output samples are used

    return framesProcessed;
}
