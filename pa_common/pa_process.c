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

    Note that in the extreme case of being able to convert in-place, and there
    being no conversion necessary there should be some code which short-circuits
    the operation.

    Cache tilings for intereave<->deinterleave also need to be considered.

    The abort flag from the callback is currently not honoured properly
    in this file, see fixmes.
*/


#define PA_FRAMES_PER_TEMP_BUFFER_WHEN_HOST_BUFFER_SIZE_IS_UNKNOWN_    1024


/* greatest common divisor - PGCD in french */
static unsigned long GCD( unsigned long a, unsigned long b )
{
    return (b==0) ? a : GCD( b, a%b);
}

/* least common multiple - PPCM in french */
static unsigned long LCM( unsigned long a, unsigned long b )
{
    return (a*b) / GCD(a,b);
}

#define PA_MAX_( a, b ) (((a) > (b)) ? (a) : (b))

static unsigned long CalculateFrameShift( unsigned long M, unsigned long N )
{
    unsigned long result = 0;
    unsigned long i;
    unsigned long lcm;
    lcm = LCM( M, N );
    for( i = M; i < lcm; i += M )
        result = PA_MAX_( result, i % N );

    return result;
}


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

    bp->hostInputChannels[0] = 0;
    bp->hostOutputChannels[0] = 0;
    
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
                    unsigned long frameShift =
                        CalculateFrameShift( framesPerHostBuffer, framesPerUserBuffer );

                    if( framesPerUserBuffer > framesPerHostBuffer )
                    {
                        bp->framesInTempInputBuffer = frameShift;
                        bp->framesInTempOutputBuffer = 0;
                    }
                    else
                    {
                        bp->framesInTempInputBuffer = 0;
                        bp->framesInTempOutputBuffer = frameShift;
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

        if( userInputSampleFormat & paNonInterleaved )
        {
            bp->tempInputBufferPtrs =
                PaUtil_AllocateMemory( sizeof(void*)*numInputChannels );
            if( bp->tempInputBufferPtrs == 0 )
            {
                result = paInsufficientMemory;
                goto error;
            }
        }

        bp->hostInputChannels[0] = (PaUtilChannelDescriptor*)
                PaUtil_AllocateMemory( sizeof(PaUtilChannelDescriptor) * numInputChannels * 2);
        if( bp->hostInputChannels[0] == 0 )
        {
            result = paInsufficientMemory;
            goto error;
        }

        bp->hostInputChannels[1] = &bp->hostInputChannels[0][numInputChannels];
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
        
        if( userOutputSampleFormat & paNonInterleaved )
        {
            bp->tempOutputBufferPtrs =
                PaUtil_AllocateMemory( sizeof(void*)*numOutputChannels );
            if( bp->tempOutputBufferPtrs == 0 )
            {
                result = paInsufficientMemory;
                goto error;
            }
        }

        bp->hostOutputChannels[0] = (PaUtilChannelDescriptor*)
                PaUtil_AllocateMemory( sizeof(PaUtilChannelDescriptor)*numOutputChannels * 2 );
        if( bp->hostOutputChannels[0] == 0 )
        {                                                                     
            result = paInsufficientMemory;
            goto error;
        }

        bp->hostOutputChannels[1] = &bp->hostOutputChannels[0][numOutputChannels];
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

    if( bp->hostInputChannels[0] )
        PaUtil_FreeMemory( bp->hostInputChannels[0] );

    if( bp->tempOutputBuffer )
        PaUtil_FreeMemory( bp->tempOutputBuffer );

    if( bp->tempOutputBufferPtrs )
        PaUtil_FreeMemory( bp->tempOutputBufferPtrs );

    if( bp->hostOutputChannels[0] )
        PaUtil_FreeMemory( bp->hostOutputChannels[0] );

    return result;
}


void PaUtil_TerminateBufferProcessor( PaUtilBufferProcessor* bp )
{
    if( bp->tempInputBuffer )
        PaUtil_FreeMemory( bp->tempInputBuffer );

    if( bp->tempInputBufferPtrs )
        PaUtil_FreeMemory( bp->tempInputBufferPtrs );

    if( bp->hostInputChannels[0] )
        PaUtil_FreeMemory( bp->hostInputChannels[0] );
        
    if( bp->tempOutputBuffer )
        PaUtil_FreeMemory( bp->tempOutputBuffer );

    if( bp->tempOutputBufferPtrs )
        PaUtil_FreeMemory( bp->tempOutputBufferPtrs );

    if( bp->hostOutputChannels[0] )
        PaUtil_FreeMemory( bp->hostOutputChannels[0] );
}


void PaUtil_BeginBufferProcessing( PaUtilBufferProcessor* bp, PaTime outTime )
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

static unsigned long AdaptingProcess( PaUtilBufferProcessor *bp,
        int *callbackResult, int processPartialUserBuffers );

#define PA_MIN_( a, b ) ( ((a)<(b)) ? (a) : (b) )

unsigned long PaUtil_EndBufferProcessing( PaUtilBufferProcessor* bp, int *callbackResult )
{
    unsigned long framesToProcess, framesToGo;
    unsigned long framesProcessed = 0;
    
    if( bp->numInputChannels != 0 && bp->numOutputChannels != 0 )
    {
        assert( (bp->hostInputFrameCount[0] + bp->hostInputFrameCount[1]) ==
                (bp->hostOutputFrameCount[0] + bp->hostOutputFrameCount[1]) );
    }

    
    if( bp->useNonAdaptingProcess )
    {
        if( bp->numInputChannels != 0 && bp->numOutputChannels != 0 )
        {
            /* full duplex non-adapting process, splice buffers if they are
                different lengths */

            framesToGo = bp->hostInputFrameCount[0] + bp->hostInputFrameCount[1]; /* relies on assert above for input/output equivalence */

            do{
                unsigned long *hostInputFrameCount;
                PaUtilChannelDescriptor *hostInputChannels;
                unsigned long *hostOutputFrameCount;
                PaUtilChannelDescriptor *hostOutputChannels;
                unsigned long framesProcessedThisIteration;
                
                if( bp->hostInputFrameCount[0] != 0 )
                {
                    hostInputFrameCount = &bp->hostInputFrameCount[0];
                    hostInputChannels = bp->hostInputChannels[0];
                }
                else
                {
                    hostInputFrameCount = &bp->hostInputFrameCount[1];
                    hostInputChannels = bp->hostInputChannels[1];
                }

                if( bp->hostOutputFrameCount[0] != 0 )
                {
                    hostOutputFrameCount = &bp->hostOutputFrameCount[0];
                    hostOutputChannels = bp->hostOutputChannels[0];
                }
                else
                {
                    hostOutputFrameCount = &bp->hostOutputFrameCount[1];
                    hostOutputChannels = bp->hostOutputChannels[1];
                }

                framesToProcess = PA_MIN_( *hostInputFrameCount,
                                       *hostOutputFrameCount );

                assert( framesToProcess != 0 );
                
                framesProcessedThisIteration = NonAdaptingProcess( bp, callbackResult,
                        hostInputChannels, hostOutputChannels,
                        framesToProcess );                                       

                *hostInputFrameCount -= framesProcessedThisIteration;
                *hostOutputFrameCount -= framesProcessedThisIteration;

                framesProcessed += framesProcessedThisIteration;
                framesToGo -= framesProcessedThisIteration;
                
            }while( framesToGo > 0 );
        }
        else
        {
            /* half duplex non-adapting process, just process 1st and 2nd buffer */
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
    }
    else /* block adaption necessary*/
    {

        if( bp->numInputChannels != 0 && bp->numOutputChannels != 0 )
        {
            /* full duplex */
            
            if( bp->hostBufferSizeMode == paUtilVariableHostBufferSizePartialUsageAllowed  )
            {
                framesProcessed = AdaptingProcess( bp, callbackResult,
                        0 /* dont process partial user buffers */ );
            }
            else
            {
                framesProcessed = AdaptingProcess( bp, callbackResult,
                        1 /* process partial user buffers */ );
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
    assert( channel < bp->numInputChannels );
    
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

    assert( firstChannel < bp->numInputChannels );
    assert( firstChannel + channelCount <= bp->numInputChannels );

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
    assert( channel < bp->numInputChannels );
    
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
    assert( channel < bp->numInputChannels );

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

    assert( firstChannel < bp->numInputChannels );
    assert( firstChannel + channelCount <= bp->numInputChannels );
    
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
    assert( channel < bp->numInputChannels );
    
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
    assert( channel < bp->numOutputChannels );
    
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

    assert( firstChannel < bp->numOutputChannels );
    assert( firstChannel + channelCount <= bp->numOutputChannels );
    
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
    assert( channel < bp->numOutputChannels );

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
    assert( channel < bp->numOutputChannels );

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

    assert( firstChannel < bp->numOutputChannels );
    assert( firstChannel + channelCount <= bp->numOutputChannels );
    
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
    assert( channel < bp->numOutputChannels );
    
    bp->hostOutputChannels[1][channel].data = data;
    bp->hostOutputChannels[1][channel].stride = 1;
}


/*
    NonAdaptingProcess() is a simple buffer copying adaptor that can handle
    both full and half duplex copies. It processes framesToProcess frames,
    broken into blocks bp->framesPerTempBuffer long.
    This routine can be used when the callback doesn't care what length the
    buffers are, or when framesToProcess is an integer multiple of
    bp->framesPerTempBuffer, in which case callback will always be called
    with bp->framesPerTempBuffer samples.
*/
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

                /* setup non-interleaved ptrs */
                for( i=0; i<bp->numInputChannels; ++i )
                {
                    bp->tempInputBufferPtrs[i] = ((unsigned char*)bp->tempInputBuffer) +
                        i * bp->bytesPerUserInputSample * frameCount;
                }
                
                userInput = bp->tempInputBufferPtrs;
            }

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
                for( i = 0; i < bp->numOutputChannels; ++i )
                {
                    bp->tempOutputBufferPtrs[i] = ((unsigned char*)bp->tempOutputBuffer) +
                        i * bp->bytesPerUserOutputSample * frameCount;
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

        framesProcessed += frameCount;

        framesToGo -= frameCount;
    }
    while( framesToGo > 0 );

    return framesProcessed;
}


/*
    AdaptingInputOnlyProcess() is a half duplex input buffer processor. It
    converts data from the input buffers into the temporary input buffer,
    when the temporary input buffer is full, it calls the callback.
*/
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
                    bp->framesInTempInputBuffer;
                      
            destStride = bp->numInputChannels;
            destBytePtrStride = bp->bytesPerUserInputSample;

            userInput = bp->tempInputBuffer;
        }
        else /* user input is not interleaved */
        {
            destBytePtr = ((unsigned char*)bp->tempInputBuffer) +
                    bp->bytesPerUserInputSample * bp->framesInTempInputBuffer;

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

        bp->framesInTempInputBuffer += frameCount;

        if( bp->framesInTempInputBuffer == bp->framesPerUserBuffer )
        {
            *callbackResult = bp->userCallback( userInput, userOutput,
                                bp->framesPerUserBuffer, bp->hostOutTime, bp->userData );

            bp->hostOutTime += bp->framesPerUserBuffer * bp->samplePeriod;  //FIXME, this is completely wrong for input only

            // FIXME: if callback result is abort, then abort!

            bp->framesInTempInputBuffer = 0;
        }

        framesProcessed += frameCount;

        framesToGo -= frameCount;
    }while( framesToGo > 0 );

    return framesProcessed;
}


/*
    AdaptingOutputOnlyProcess() is a half duplex output buffer processor.
    It converts data from the temporary output buffer, to the output buffers,
    when the temporary output buffer is empty, it calls the callback.
*/
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

        bp->framesInTempOutputBuffer -= frameCount;

        framesProcessed += frameCount;
        
        framesToGo -= frameCount;

    }while( framesToGo > 0 );

    return framesProcessed;
}


/*
    AdaptingProcess is a full duplex adapting buffer processor. It converts
    data from the temporary output buffer into the host output buffers, then
    from the host input buffers into the temporary input buffers. Calling the
    callback when necessary.
    When processPartialUserBuffers is 0, all available input data will be
    consumed and all available output space will be filled. When
    processPartialUserBuffers is non-zero, as many full user buffers
    as possible will be processed, but partial buffers will not be consumed.
*/
static unsigned long AdaptingProcess( PaUtilBufferProcessor *bp,
        int *callbackResult, int processPartialUserBuffers )
{
    void *userInput, *userOutput;
    unsigned long framesProcessed = 0;
    unsigned long framesAvailable;
    unsigned long endProcessingMinFrameCount;
    unsigned long maxFramesToCopy;
    PaUtilChannelDescriptor *hostInputChannels, *hostOutputChannels;
    unsigned int frameCount;
    unsigned char *srcBytePtr, *destBytePtr;
    unsigned int srcStride, srcBytePtrStride, destStride, destBytePtrStride;
    unsigned int i;

    framesAvailable = bp->hostInputFrameCount[0] + bp->hostInputFrameCount[1];/* this is assumed to be the same as the output buffers frame count */

    if( processPartialUserBuffers )
        endProcessingMinFrameCount = 0;
    else
        endProcessingMinFrameCount = (bp->framesPerUserBuffer - 1);

    while( framesAvailable > endProcessingMinFrameCount )
    {
        /* copy frames from user to host output buffers */
        while( bp->framesInTempOutputBuffer > 0 &&
                ((bp->hostOutputFrameCount[0] + bp->hostOutputFrameCount[1]) > 0) )
        {
            maxFramesToCopy = bp->framesInTempOutputBuffer;

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

            if( bp->hostOutputFrameCount[0] > 0 )
                bp->hostOutputFrameCount[0] -= frameCount;
            else
                bp->hostOutputFrameCount[1] -= frameCount;

            bp->framesInTempOutputBuffer -= frameCount;
        }


        /* copy frames from host to user input buffers */
        while( bp->framesInTempInputBuffer < bp->framesPerUserBuffer &&
                ((bp->hostInputFrameCount[0] + bp->hostInputFrameCount[1]) > 0) )
        {
            maxFramesToCopy = bp->framesPerUserBuffer - bp->framesInTempInputBuffer;

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
                        bp->framesInTempInputBuffer;

                destStride = bp->numInputChannels;
                destBytePtrStride = bp->bytesPerUserInputSample;
            }
            else /* user input is not interleaved */
            {
                destBytePtr = ((unsigned char*)bp->tempInputBuffer) +
                        bp->bytesPerUserInputSample * bp->framesInTempInputBuffer;

                destStride = 1;
                destBytePtrStride = bp->framesPerUserBuffer * bp->bytesPerUserInputSample;
            }

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

            if( bp->hostInputFrameCount[0] > 0 )
                bp->hostInputFrameCount[0] -= frameCount;
            else
                bp->hostInputFrameCount[1] -= frameCount;
                
            bp->framesInTempInputBuffer += frameCount;

            /* update framesAvailable and framesProcessed based on input consumed
                unless something is very wrong this will also correspond to the
                amount of output generated */
            framesAvailable -= frameCount;
            framesProcessed += frameCount;
        }

        /* call callback */
        if( bp->framesInTempInputBuffer == bp->framesPerUserBuffer &&
            bp->framesInTempOutputBuffer == 0 )
        {
            /* setup userInput */
            if( bp->userInputIsInterleaved )
            {
                userInput = bp->tempInputBuffer;
            }
            else /* user input is not interleaved */
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
                    bp->framesPerUserBuffer, bp->hostOutTime, bp->userData );

            bp->hostOutTime += bp->framesPerUserBuffer * bp->samplePeriod;

            // FIXME: if callback result is abort, then abort!
        

            bp->framesInTempInputBuffer = 0;
            bp->framesInTempOutputBuffer = bp->framesPerUserBuffer;
        }
    }
    
    return framesProcessed;
}
