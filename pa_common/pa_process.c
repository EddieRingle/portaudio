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

#include "pa_process.h"
#include "pa_util.h"


PaError PaUtil_InitializeBufferProcessor( PaUtilBufferProcessor* bp,
        int numInputChannels, PaSampleFormat userInputSampleFormat,
        PaSampleFormat hostInputSampleFormat,
        int numOutputChannels, PaSampleFormat userOutputSampleFormat,
        PaSampleFormat hostOutputSampleFormat,
        double sampleRate,
        PaStreamFlags streamFlags,
        unsigned long framesPerUserBuffer, unsigned long framesPerHostBuffer,
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
            bp->bytesPerUserInputSample * numInputChannels * framesPerUserBuffer;
        bp->tempInputBuffer = PaUtil_AllocateMemory( tempInputBufferSize );
        if( bp->tempInputBuffer == 0 )
        {
            result = paInsufficientMemory;
            goto error;
        }

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
            bp->bytesPerUserOutputSample * numOutputChannels * framesPerUserBuffer;

        bp->tempOutputBuffer = PaUtil_AllocateMemory( tempOutputBufferSize );
        if( bp->tempOutputBuffer == 0 )
        {
            result = paInsufficientMemory;
            goto error;
        }


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
    }

    PaUtil_InitializeTriangularDitherState( &bp->ditherGenerator );

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


static void* ConfigureUserOutputBufferPtr( PaUtilBufferProcessor *bp, void *hostBuffer )
{
    unsigned int i;
    unsigned char *p;
    void *result;

    if( bp->numOutputChannels == 0 )
    {
        /* no output */
        result = 0;
    }
    else
    {
        if( bp->outputConverter )
        {
            if( bp->userOutputIsInterleaved )
            {

                /* user output is interleaved */
                result = bp->tempOutputBuffer;

            }
            else
            {

                /* user output is non-interleaved */
                p = bp->tempOutputBuffer;

                for( i=0; i < bp->numOutputChannels; ++i )
                {
                    bp->tempOutputBufferPtrs[i] = p;
                    p +=  bp->framesPerUserBuffer * bp->bytesPerUserOutputSample;
                }

                result = bp->tempOutputBufferPtrs;
            }
        }
        else
        {
            /* pass output buffer directly if no conversion is needed */
            result = hostBuffer;

            /* REVIEW: what if no conversion is necessary but the host is interleaved and the user isn't or vice versa ? */
        }
    }

    return result;
}


int PaUtil_ProcessInterleavedBuffers( PaUtilBufferProcessor* bp,
                                      void *hostInput, void *hostOutput, PaTimestamp outTime )
{
    unsigned int hostFramesRemaining = bp->framesPerHostBuffer;
    void *userInput, *userOutput;
    unsigned char *srcBytePtr, *destBytePtr;
    unsigned char **srcNonInterleavedPtr;
    unsigned int i;
    int result;

    while( hostFramesRemaining > 0 )
    {

        /* configure user input buffer and convert input data (host -> user) */
        if( bp->numInputChannels == 0 )
        {
            /* no input */
            userInput = 0;
        }
        else
        {
            if( bp->inputConverter )
            {

                if( bp->userInputIsInterleaved )
                {

                    /* host and user input are interleaved,
                        convert all channels at once... */

                    bp->inputConverter( bp->tempInputBuffer, 1, hostInput, 1,
                                        bp->framesPerUserBuffer * bp->numInputChannels, &bp->ditherGenerator );

                    userInput = bp->tempInputBuffer;

                }
                else
                {

                    /* host input is interleaved, user input is non-interleaved */

                    srcBytePtr = hostInput;
                    destBytePtr = bp->tempInputBuffer;
                    for( i=0; i<bp->numInputChannels; ++i )
                    {
                        bp->inputConverter( destBytePtr, 1,
                                            srcBytePtr, bp->numInputChannels,
                                            bp->framesPerUserBuffer, &bp->ditherGenerator );

                        srcBytePtr += bp->bytesPerHostInputSample; /* skip to next interleaved channel */
                        bp->tempInputBufferPtrs[i] = destBytePtr;
                        /* skip to next non-interleaved channel */
                        destBytePtr +=  bp->framesPerUserBuffer * bp->bytesPerUserInputSample;
                    }

                    userInput = bp->tempInputBufferPtrs;
                }
            }
            else
            {
                /* pass input buffer directly if no conversion is needed */
                userInput = hostInput;
            }
            /* advance input ptr for next iteration */
            hostInput = ((unsigned char*)hostInput) +
                        bp->framesPerUserBuffer * bp->numInputChannels * bp->bytesPerHostInputSample;

        }

        userOutput = ConfigureUserOutputBufferPtr( bp, hostOutput );

        result = bp->userCallback( userInput, userOutput,
                                   bp->framesPerUserBuffer, outTime, bp->userData );


        /* convert output (user -> host) */
        if( bp->numOutputChannels != 0 )
        {
            if( bp->outputConverter )
            {

                if( bp->userOutputIsInterleaved )
                {

                    /* host and user output are interleaved */

                    bp->outputConverter( hostOutput, 1, userOutput, 1,
                                         bp->framesPerUserBuffer * bp->numOutputChannels, &bp->ditherGenerator );

                }
                else
                {

                    /* host output is interleaved, user output is non-interleaved */
                    srcNonInterleavedPtr = (unsigned char**)userOutput;
                    destBytePtr = hostOutput;

                    for( i=0; i<bp->numOutputChannels; ++i )
                    {
                        bp->outputConverter( destBytePtr, bp->numOutputChannels, *srcNonInterleavedPtr, 1, bp->framesPerUserBuffer, &bp->ditherGenerator );
                        ++srcNonInterleavedPtr;
                        destBytePtr += bp->bytesPerHostOutputSample;
                    }
                }


            }
            else
            {
                /* output samples are already in host output buffer */
            }
            /* advance output ptr for next iteration */
            hostOutput = ((unsigned char*)hostOutput) +
                         bp->framesPerUserBuffer * bp->numOutputChannels * bp->bytesPerHostOutputSample;
        }

        outTime += bp->framesPerUserBuffer;
        hostFramesRemaining -= bp->framesPerUserBuffer;
    }

    return result;
}


int PaUtil_ProcessNonInterleavedBuffers( PaUtilBufferProcessor* bp,
        void **hostInput, void **hostOutput, PaTimestamp outTime )
{
    unsigned int hostFramesRemaining = bp->framesPerHostBuffer;
    void *userInput, *userOutput;
    unsigned char *srcBytePtr, *destBytePtr;
    unsigned char **srcNonInterleavedPtr, **destNonInterleavedPtr;
    unsigned int i;
    int result;

    while( hostFramesRemaining > 0 )
    {

        /* configure user input buffer and convert input data (host -> user) */
        if( bp->numInputChannels == 0 )
        {
            /* no input */
            userInput = 0;
        }
        else
        {
            if( bp->inputConverter )
            {
                if( bp->userInputIsInterleaved )
                {

                    /* host input is non-interleaved, user input is interleaved */
                    srcNonInterleavedPtr = (unsigned char**)hostInput;
                    destBytePtr = bp->tempInputBuffer;
                    for( i=0; i<bp->numInputChannels; ++i )
                    {
                        bp->inputConverter( destBytePtr, bp->numInputChannels,
                                            *srcNonInterleavedPtr, 1,
                                            bp->framesPerUserBuffer, &bp->ditherGenerator );

                        /* advance host ptr for next iteration */
                        *srcNonInterleavedPtr += bp->framesPerUserBuffer * bp->bytesPerHostInputSample;
                        ++srcNonInterleavedPtr;

                        /* skip to next interleaved channel */
                        destBytePtr +=  bp->bytesPerUserInputSample;
                    }

                    userInput = bp->tempInputBuffer;

                }
                else
                {

                    /* host and user input are non-interleaved */
                    srcNonInterleavedPtr = (unsigned char**)hostInput;
                    destBytePtr = bp->tempInputBuffer;
                    for( i=0; i<bp->numInputChannels; ++i )
                    {
                        bp->inputConverter( destBytePtr, 1, *srcNonInterleavedPtr, 1,
                                            bp->framesPerUserBuffer, &bp->ditherGenerator );

                        /* advance input ptr for next iteration */
                        *srcNonInterleavedPtr += bp->framesPerUserBuffer * bp->bytesPerHostInputSample;
                        ++srcNonInterleavedPtr;

                        bp->tempInputBufferPtrs[i] = destBytePtr;
                        /* skip to next non-interleaved channel */
                        destBytePtr +=  bp->framesPerUserBuffer * bp->bytesPerUserInputSample;
                    }

                    userInput = bp->tempInputBufferPtrs;
                }
            }
            else
            {
                /* pass input buffer directly if no conversion is needed */
                userInput = hostInput;
            }
        }

        userOutput = ConfigureUserOutputBufferPtr( bp, hostOutput );

        result = bp->userCallback( userInput, userOutput,
                                   bp->framesPerUserBuffer, outTime, bp->userData );


        /* convert output (user -> host) */
        if( bp->numOutputChannels != 0 )
        {
            if( bp->outputConverter )
            {
                if( bp->userOutputIsInterleaved )
                {
                    /* host output is non-interleaved, user output is interleaved */
                    srcBytePtr = userOutput;
                    destNonInterleavedPtr = (unsigned char**)hostOutput;
                    for( i=0; i<bp->numOutputChannels; ++i )
                    {
                        bp->outputConverter( *destNonInterleavedPtr, 1,
                                            srcBytePtr, bp->numOutputChannels,
                                            bp->framesPerUserBuffer, &bp->ditherGenerator );

                        srcBytePtr += bp->bytesPerUserOutputSample; /* skip to next interleaved channel */

                        /* advance output ptr for next iteration */
                        *destNonInterleavedPtr += bp->framesPerUserBuffer * bp->bytesPerHostOutputSample;
                        ++destNonInterleavedPtr;
                    }
                }
                else
                {
                    /* host and user output are non-interleaved */
                    srcNonInterleavedPtr = (unsigned char**)userOutput;
                    destNonInterleavedPtr = (unsigned char**)hostOutput;
                    for( i=0; i<bp->numOutputChannels; ++i )
                    {
                        bp->outputConverter( *destNonInterleavedPtr, 1, *srcNonInterleavedPtr, 1,
                                             bp->framesPerUserBuffer, &bp->ditherGenerator );

                        ++srcNonInterleavedPtr;

                        /* advance output ptr for next iteration */
                        *destNonInterleavedPtr += bp->framesPerUserBuffer * bp->bytesPerHostOutputSample;
                        ++destNonInterleavedPtr;
                    }
                }
            }
            else
            {
                /* output samples are already in host output buffer */
            }
        }

        outTime += bp->framesPerUserBuffer;
        hostFramesRemaining -= bp->framesPerUserBuffer;
    }

    /* rewind the non-interleaved pointers, so they have the same values as they did on entry */

    if( bp->numInputChannels != 0 )
    {
        srcNonInterleavedPtr = (unsigned char**)hostInput;
        for( i=0; i<bp->numInputChannels; ++i )
        {
            *srcNonInterleavedPtr -= bp->framesPerHostBuffer * bp->bytesPerHostInputSample;
            ++srcNonInterleavedPtr;
        }
    }

    if( bp->numOutputChannels != 0 )
    {
        destNonInterleavedPtr = (unsigned char**)hostOutput;
        for( i=0; i<bp->numOutputChannels; ++i )
        {
            *destNonInterleavedPtr -= bp->framesPerHostBuffer * bp->bytesPerHostOutputSample;
            ++destNonInterleavedPtr;
        }
    }

    return result;
}

int PaUtil_ProcessBuffers( PaUtilBufferProcessor* bp, /* bp => buffer processor */
                           PaUtilChannelDescriptor *hostInput, PaUtilChannelDescriptor *hostOutput,
                           PaTimestamp outTime )
{

    /* simplest implementation, assumes framesPerHostBuffer mod framesPerUserBuffer == 0 */
    /*
        TODO: handle NULL hostInput and hostOutput buffers

    */

    unsigned int hostFramesRemaining = bp->framesPerHostBuffer;
    void *userInput, *userOutput;
    unsigned char *srcBytePtr, *destBytePtr;
    unsigned char **srcNonInterleavedPtr;
    unsigned int i;
    int result;

    while( hostFramesRemaining > 0 )
    {

        /* configure user input buffer and convert input data (host -> user) */
        if( bp->numInputChannels == 0 )
        {
            /* no input */
            userInput = 0;
        }
        else
        {
            if( bp->inputConverter )
            {
                if( bp->userInputIsInterleaved )
                {

                    destBytePtr = bp->tempInputBuffer;

                    for( i=0; i<bp->numInputChannels; ++i )
                    {
                        bp->inputConverter( destBytePtr, bp->numInputChannels,
                                            hostInput[i].data, hostInput[i].stride,
                                            bp->framesPerUserBuffer, &bp->ditherGenerator );

                        destBytePtr += bp->bytesPerUserInputSample;

                        /* advance src ptr for next iteration */
                        hostInput[i].data += hostInput[i].stride * bp->framesPerUserBuffer;
                    }

                    userInput = bp->tempInputBuffer;

                }
                else
                {

                    destBytePtr = bp->tempInputBuffer;

                    for( i=0; i<bp->numInputChannels; ++i )
                    {
                        bp->inputConverter( destBytePtr, 1,
                                            hostInput[i].data, hostInput[i].stride,
                                            bp->framesPerUserBuffer, &bp->ditherGenerator );

                        bp->tempInputBufferPtrs[i] = destBytePtr;
                        /* skip to next non-interleaved channel */
                        destBytePtr +=  bp->framesPerUserBuffer * bp->bytesPerUserInputSample;

                        /* advance src ptr for next iteration */
                        hostInput[i].data += hostInput[i].stride * bp->framesPerUserBuffer;
                    }

                    userInput = bp->tempInputBufferPtrs;
                }
            }
            else
            {
                /* pass input buffer directly if no conversion is needed */
                userInput = hostInput;
            }
        }

        userOutput = ConfigureUserOutputBufferPtr( bp, hostOutput );

        result = bp->userCallback( userInput, userOutput,
                                   bp->framesPerUserBuffer, outTime, bp->userData );


        /* convert output (user -> host) */
        if( bp->numOutputChannels != 0 )
        {
            if( bp->outputConverter )
            {
                if( bp->userOutputIsInterleaved )
                {
                    srcBytePtr = userOutput;

                    for( i=0; i<bp->numOutputChannels; ++i )
                    {
                        bp->inputConverter( hostOutput[i].data, hostInput[i].stride,
                                            srcBytePtr, bp->numOutputChannels,
                                            bp->framesPerUserBuffer, &bp->ditherGenerator );

                        srcBytePtr += bp->bytesPerHostOutputSample; /* skip to next interleaved channel */

                        /* advance src ptr for next iteration */
                        hostOutput[i].data += hostInput[i].stride * bp->framesPerUserBuffer;
                    }

                }
                else
                {

                    /* host and user output are non-interleaved */
                    srcNonInterleavedPtr = (unsigned char**)userOutput;
                    for( i=0; i<bp->numOutputChannels; ++i )
                    {
                        bp->outputConverter( hostOutput[i].data, hostInput[i].stride,
                                             *srcNonInterleavedPtr, 1,
                                             bp->framesPerUserBuffer, &bp->ditherGenerator );

                        ++srcNonInterleavedPtr;

                        /* advance src ptr for next iteration */
                        hostOutput[i].data += hostInput[i].stride * bp->framesPerUserBuffer;
                    }
                }
            }
            else
            {
                /* output samples are already in host output buffer */
            }
        }

        outTime += bp->framesPerUserBuffer;
        hostFramesRemaining -= bp->framesPerUserBuffer;
    }

    //TODO: rewind the non-interleaved pointers if necessary.

    return result;
}


/*
 while availableFrames > 0
     if availableFrames < userFrames
         do something
     else
         choose input dest buffer(s) (could be in-place)
         if needed convert input src to dest
         or, zero input dest if src is null

         choose output src buffer(s) (could be in-place)

         calculate timestamp

         call callback

         if needed convert output src to dest
 */



/*
ROSS' NOTES:
 
don't know how to deal with interleave/deinterleave.. the converter will need to know:
 
for each of input and output,
    the converter will need to know if the src and dest are interleaved or deinterleaved
 
    it will need to make a decision about whether to use a temp buffer or in-place conversion
    based on various factors
 
    if non-interleaved it may need to generate the channel ptrs
 
    if passed a NULL input or output buffer it will know to use a temp buffer
    (and to zero the temp buffer for input)
 
    it will need to be passed the buffer slip flags.
 
    it will need to generate timestamps based on userBuffers start offset into the hostBuffer
    if buffer adaption is being used the timestamp will be generated from the last
    host buffer used to fill the user buffer.
 
 
    stuff that can be known up front (for input and output):
    - channel count
    - source and destination interleave
    - source and destination sample format
 
    * the same conversion function will be used irrespective of interleave or channel count
    * the conversion source and destination stride will be known up-front,
        but they can be derived from the channel count and interleave type
 
    - the conversion buffer pointers will not be known up front.
 
    if the source is interleaved, the source ptr will just step through the
    first channelCount samples of source.
 
    if the source is non-interleaved, the source ptr will step through the
    src ptrs array
 
    if the destination is interleaved, the destination ptr will just step through
    the first channelCount samples of destination.
 
    if the dest is non-interleaved the dest ptr will step through the dest ptrs array.
 
    if either input or output host buffers are non-interleaved their ptr arrays
    will be pased to our processing function
 
    therefore... the only de-interleaved ptr arrays we have to self-generate are
    those for userBufferPtrs
 
    essentially there are 4 possible conversion loops (see further down for implementations):
 
    interleave -> interleave ( src, srcSampleSize, dest, destSampleSize, channels, conversionFunc )
    interleave -> deinterleave ( src, srcSampleSize, destPtrs, destSampleSize, channels, conversionFunc )
    deinterleave -> deinterleave ( srcPtrs, srcSampleSize, destPtrs, destSampleSize, channels, conversionFunc )
    deinterleave -> interleave ( srcPtrs, srcSampleSize, dest, destSampleSize, channels, conversionFunc )
 
    the conversion loops are used for both input and output, producing a total
    of 8 potential conversion functions.
 
    the general conversion function structure is:
 
        input src and output dest are recieved as parameters
 
        choose input dest buffer(s) (could be in-place)
        if needed convert input src to dest
        or, zero input dest if src is null
 
        choose output src buffer(s) (could be in-place)
 
        calculate timestamp
 
        call callback
 
        if needed convert output src to dest
 
 
    this should sit in an outer loop something like:
 
    while availableFrames > 0
        if availableFrames < userFrames
            do something
        else
            just process the frames and call the callback, as above
 
 
 
 
 
TODAY'S BIG QUESTION: how do you block-adapt a full duplex stream?
The only way I can think of doing it is to always delay the input by userBufferFrames,
that way there is always enough data to call the callback and generate output,
even though a full frame's worth of input is not available.
 
----
possible callbacks
 
input only, no block adapt
{
    do{
        consider using in-place conversion
        convertBuffer
        call callback( destBuffer );
        framesAvailable -= framesPerUserBuffer;
 
    }while( framesAvailable > 0 );
    assert( framesAvailable == 0 ); // no partial buffers
}
 
input only, block adapt
{
    do{
        if( framesInTempBuffer == 0 && framesAvailable >= framesPerUserBuffer ){
            // convert a full buffer, as in non block-adapt mode
            consider using in-place conversion
            convertBuffer
            call callback( destBuffer );
            framesAvailable -= framesPerUserBuffer;
        }else{
            // convert a partial buffer, we are at the boundary of host buffer
            int framesLeftToFillInTempBuffer = framesPerUserBuffer - framesInTempBuffer;
            int framesToCopy = min( framesAvailable, framesLeftToFillInTempBuffer );
            convertBuffer( count= userBuffer + framesInTempBuffer, framesToCopy )
            framesInTempBuffer += framesToCopy;
            framesAvailable -= framesToCopy;
            assert( framesInTempBuffer <= framesPerUserBuffer );
            if( framesInTempBuffer == framesPerUserBuffer ){
                call callback( userBuffer );
                framesInTempBuffer = 0
            }
        }
    }while( framesAvailable > 0 );
}
 
 
output only, no block adapt
output only, block adapt
full-duplex, no block adapt
full-duplex, block adapt
 
----
 
    interleave -> interleave ( src, srcSampleSize, dest, destSampleSize, channels, conversionFunc, count )
        srcStride = channels
        destSride = channels
        for( i = 0; i < channels; ++i )
            conversionFunc( src, srcStride, dest, destStride, count )
            src += srcSampleSize
            dest += destSampleSize
 
    interleave -> deinterleave ( src, srcSampleSize, destPtrs, channels, conversionFunc, count )
        srcStride = channels
        destSride = 1
        for( i = 0; i < channels; ++i )
            conversionFunc( src, srcStride, destPtrs[i], destStride, count )
            src += srcSampleSize
 
    deinterleave -> deinterleave ( srcPtrs, destPtrs, channels, conversionFunc, count )
        srcStride = 1
        destStride = 1
        for( i = 0; i < channels; ++i )
            conversionFunc( srcPtrs[i], srcStride, destPtrs[i], destStride, count )
            src += srcSampleSize
 
    deinterleave -> interleave ( srcPtrs, dest, destSampleSize, channels, conversionFunc, count )
        srcStride = 1
        destSride = channels
        conversionFunc( srcPtrs[i], srcStride, dest, destStride, count )
            dest += destSampleSize
 
*/


/* Phil's old process code:
 
PaError PaConvert_SetupInput( internalPortAudioStream   *past,
    PaSampleFormat   nativeInputSampleFormat )
{
    past->past_NativeInputSampleFormat = nativeInputSampleFormat;
    past->past_InputConversionSourceStride = 1;
    past->past_InputConversionTargetStride = 1;
 
    if( nativeInputSampleFormat != past->past_InputSampleFormat )
    {
        int ifDither = (past->past_Flags & paDitherOff) == 0;
        past->past_InputConversionProc = PaConvert_SelectProc( nativeInputSampleFormat,
             past->past_InputSampleFormat, 0, ifDither );
        if( past->past_InputConversionProc == NULL ) return paSampleFormatNotSupported;
    }
    else
    {
        past->past_InputConversionProc = NULL; // no conversion necessary
    }
 
    return paNoError;
}
 
PaError PaConvert_SetupOutput( internalPortAudioStream   *past,
    PaSampleFormat   nativeOutputSampleFormat )
{
 
    past->past_NativeOutputSampleFormat = nativeOutputSampleFormat;
    past->past_OutputConversionSourceStride = 1;
    past->past_OutputConversionTargetStride = 1;
    
    if( nativeOutputSampleFormat != past->past_OutputSampleFormat )
    {
        int ifDither = (past->past_Flags & paDitherOff) == 0;
        int ifClip = (past->past_Flags & paClipOff) == 0;
 
        past->past_OutputConversionProc = PaConvert_SelectProc( past->past_OutputSampleFormat,
            nativeOutputSampleFormat, ifClip, ifDither );
        if( past->past_OutputConversionProc == NULL ) return paSampleFormatNotSupported;
    }
    else
    {
        past->past_OutputConversionProc = NULL; // no conversion necessary
    }
 
    return paNoError;
}
 
 
 
//Called by host code.
// Convert input from native format to user format,
// call user code,
// then convert output to native format.
// Returns result from user callback.
long PaConvert_Process( internalPortAudioStream   *past,
                            void *nativeInputBuffer,
                            void *nativeOutputBuffer )
{
    int               userResult;
    void             *inputBuffer = NULL;
    void             *outputBuffer = NULL;
 
    /// Get native input data.
    if( (past->past_NumInputChannels > 0) && (nativeInputBuffer != NULL) )
    {
        if( past->past_InputSampleFormat == past->past_NativeInputSampleFormat )
        {
        //  Already in native format so just read directly from native buffer.
            inputBuffer =  nativeInputBuffer;
        }
        else
        {
            inputBuffer = past->past_InputBuffer;
        // Convert input data to user format.
            (*past->past_InputConversionProc)(nativeInputBuffer, past->past_InputConversionSourceStride,
                inputBuffer, past->past_InputConversionTargetStride,
                past->past_FramesPerUserBuffer * past->past_NumInputChannels );
        }
    }
 
    // Are we doing output?
    if( (past->past_NumOutputChannels > 0) && (nativeOutputBuffer != NULL) )
    {
        outputBuffer = (past->past_OutputConversionProc == NULL) ?
                       nativeOutputBuffer : past->past_OutputBuffer;
    }
 
     // AddTraceMessage("Pa_CallConvertInt16: inputBuffer = ", (int) inputBuffer );
     // AddTraceMessage("Pa_CallConvertInt16: outputBuffer = ", (int) outputBuffer );
 
    // Call user callback routine.
    userResult = past->past_Callback(
                     inputBuffer,
                     outputBuffer,
                     past->past_FramesPerUserBuffer,
                     past->past_FrameCount,
                     past->past_UserData );
 
    // Advance frame counter for timestamp.
    past->past_FrameCount += past->past_FramesPerUserBuffer; // FIXME - should this be in here?
 
    // Convert to native format if necessary. 
    if( (past->past_OutputConversionProc != NULL ) && (outputBuffer != NULL) )
    {
        (*past->past_OutputConversionProc)( outputBuffer, past->past_OutputConversionSourceStride,
            nativeOutputBuffer, past->past_OutputConversionTargetStride,
            past->past_FramesPerUserBuffer * past->past_NumOutputChannels );
    }
 
    return userResult;
}
*/
