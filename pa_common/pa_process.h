#ifndef PA_PROCESS_H
#define PA_PROCESS_H
/*
 * $Id$
 * Portable Audio I/O Library callback buffer processing adapters
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

#include "portaudio.h"
#include "pa_converters.h"
#include "pa_dither.h"

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/** @file
    Support for adapting between host API input and output audio buffers, and
    the buffers passed to the client supplied stream callback function. Adaption
    is performed by the PaUtilBufferProcessor which supports a range of adaption
    modes.

    There are two aspects to the adaption supported by PaUtilBufferProcessor:
    Firstly, buffer length adaption - this is used when the client requests
    a buffer size which cannot be directly supported by the host API. In this
    case host API sized buffers are passed to the buffer processor, and the
    buffer processor takes care of slicing or chunking the data into the correct
    size for the client callback. Secondly, buffer format and interleaving
    adaption is performed. The buffer processor makes use of the converters
    devined in pa_converters.c to convert sample data, and also handles
    interleaving, deinterleaving, or re-interleaving data depending on the
    form the host API supplies the data in.

    PaUtilBufferProcessor maintains temporary buffers which are used when
    converting between incompatible sample formats.

    @todo finish documentation for the buffer processor
*/

typedef enum {
    paUtilFixedHostBufferSize,
    paUtilBoundedHostBufferSize,
    paUtilUnknownHostBufferSize,
    paUtilVariableHostBufferSizePartialUsageAllowed,   /**< the only mode where PaUtil_EndBufferProcessing() may not consume the whole buffer */
}PaUtilHostBufferSizeMode;


typedef struct PaUtilChannelDescriptor{
    void *data;
    unsigned int stride;
}PaUtilChannelDescriptor;


typedef struct {
    unsigned long framesPerUserBuffer;
    unsigned long framesPerHostBuffer;

    PaUtilHostBufferSizeMode hostBufferSizeMode;
    int useNonAdaptingProcess;
    unsigned long framesPerTempBuffer;

    unsigned int inputChannelCount;
    unsigned int bytesPerHostInputSample;
    unsigned int bytesPerUserInputSample;
    int userInputIsInterleaved;
    PaUtilConverter *inputConverter;
    PaUtilZeroer *inputZeroer;
    
    unsigned int outputChannelCount;
    unsigned int bytesPerHostOutputSample;
    unsigned int bytesPerUserOutputSample;
    int userOutputIsInterleaved;
    PaUtilConverter *outputConverter;
    PaUtilZeroer *outputZeroer;

    unsigned long initialFramesInTempInputBuffer;
    unsigned long initialFramesInTempOutputBuffer;

    void *tempInputBuffer;          /**< used for slips, block adaption, and conversion. */
    void **tempInputBufferPtrs;     /**< storage for non-interleaved buffer pointers, NULL for interleaved user input */
    unsigned long framesInTempInputBuffer; /**< frames remaining in input buffer from previous adaption iteration */

    void *tempOutputBuffer;         /**< used for slips, block adaption, and conversion. */
    void **tempOutputBufferPtrs;    /**< storage for non-interleaved buffer pointers, NULL for interleaved user output */
    unsigned long framesInTempOutputBuffer; /**< frames remaining in input buffer from previous adaption iteration */

    PaStreamCallbackTimeInfo *timeInfo;
    
    unsigned long hostInputFrameCount[2];
    PaUtilChannelDescriptor *hostInputChannels[2];
    unsigned long hostOutputFrameCount[2];
    PaUtilChannelDescriptor *hostOutputChannels[2];

    PaUtilTriangularDitherGenerator ditherGenerator;

    double samplePeriod;

    PaStreamCallback *streamCallback;
    void *userData;
} PaUtilBufferProcessor;


/**
    @param framesPerHostBuffer Specifies the number of frames per host buffer
    for the fixed buffer size mode, and the maximum number of frames
    per host buffer for the bounded host buffer size mode. It is ignored for
    the other modes.
    
    @note The interleave flag is ignored for host buffer formats. Host interleave
    is determined by the use of different SetInput and SetOutput functions.
*/
PaError PaUtil_InitializeBufferProcessor( PaUtilBufferProcessor* bufferProcessor,
            int numInputChannels, PaSampleFormat userInputSampleFormat,
            PaSampleFormat hostInputSampleFormat,
            int numOutputChannels, PaSampleFormat userOutputSampleFormat,
            PaSampleFormat hostOutputSampleFormat,
            double sampleRate,
            PaStreamFlags streamFlags,
            unsigned long framesPerUserBuffer, /* 0 indicates don't care */
            unsigned long framesPerHostBuffer,
            PaUtilHostBufferSizeMode hostBufferSizeMode,
            PaStreamCallback *streamCallback, void *userData );


void PaUtil_TerminateBufferProcessor( PaUtilBufferProcessor* bufferProcessor );


/**
 If you call PaUtil_InitializeBufferProcessor() in your OpenStream routine,
 make sure you call PaUtil_ResetBufferProcessor in your StartStream call.
 This routine flushes out any internally buffered data.
*/
void PaUtil_ResetBufferProcessor( PaUtilBufferProcessor* bufferProcessor );


/**
 @param timeInfo Timing information for the first sample of the buffer(s)
 passed to the buffer processor. The buffer processor may adjust this
 information as necessary.
*/
void PaUtil_BeginBufferProcessing( PaUtilBufferProcessor* bufferProcessor,
        PaStreamCallbackTimeInfo* timeInfo /* add callback flags parameter here */ );

/** returns the number of frames processed, this usually corresponds to the
 number of frames passed in, exept in the
 paUtilVariableHostBufferSizePartialUsageAllowed buffer size mode.

 On entry callback result should contain one of { paContinue, paComplete, or
 paAbort}. If paComplete is passed, the stream callback will not be called
 but any audio that was generated by previous stream callbacks will be copied
 to the output buffer(s). You can check whether the buffer processor's internal
 buffer is empty by calling PaUtil_IsBufferProcessorOuputEmpty().

 If the stream callback is called its result is stored in *callbackResult. If the
 stream callback returns paComplete or paAbort, all output buffers will be
 full of valid data - some of which may be zeros to acount for data that
 wasn't generated by the terminating callback.
*/
unsigned long PaUtil_EndBufferProcessing( PaUtilBufferProcessor* bufferProcessor,
        int *callbackResult );


/** a 0 frameCount indicates to use the framesPerHostBuffer value passed to init */
void PaUtil_SetInputFrameCount( PaUtilBufferProcessor* bufferProcessor,
        unsigned long frameCount );

        
/**  PaUtil_SetNoInput can be used to indicate that no input is available
    when priming the output of a full-duplex stream for
    paPrimeOutputBuffersUsingStreamCallback.
*/
void PaUtil_SetNoInput( PaUtilBufferProcessor* bufferProcessor );

                                             
void PaUtil_SetInputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data, unsigned int stride );

/** if channel count is zero use all channels as specified to initialize buffer processor */
void PaUtil_SetInterleavedInputChannels( PaUtilBufferProcessor* bufferProcessor,
        unsigned int firstChannel, void *data, unsigned int channelCount );

void PaUtil_SetNonInterleavedInputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data );


void PaUtil_Set2ndInputFrameCount( PaUtilBufferProcessor* bufferProcessor,
        unsigned long frameCount );

void PaUtil_Set2ndInputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data, unsigned int stride );

/** if channel count is zero use all channels as specified to initialize buffer processor */
void PaUtil_Set2ndInterleavedInputChannels( PaUtilBufferProcessor* bufferProcessor,
        unsigned int firstChannel, void *data, unsigned int channelCount );

void PaUtil_Set2ndNonInterleavedInputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data );

/** a 0 frameCount indicates to use the framesPerHostBuffer value passed to init */
void PaUtil_SetOutputFrameCount( PaUtilBufferProcessor* bufferProcessor,
        unsigned long frameCount );

void PaUtil_SetOutputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data, unsigned int stride );

/** if channel count is zero use all channels as specified to initialize buffer processor */
void PaUtil_SetInterleavedOutputChannels( PaUtilBufferProcessor* bufferProcessor,
        unsigned int firstChannel, void *data, unsigned int channelCount );

void PaUtil_SetNonInterleavedOutputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data );


void PaUtil_Set2ndOutputFrameCount( PaUtilBufferProcessor* bufferProcessor,
        unsigned long frameCount );

void PaUtil_Set2ndOutputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data, unsigned int stride );

/** if channel count is zero use all channels as specified to initialize buffer processor */
void PaUtil_Set2ndInterleavedOutputChannels( PaUtilBufferProcessor* bufferProcessor,
        unsigned int firstChannel, void *data, unsigned int channelCount );

void PaUtil_Set2ndNonInterleavedOutputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data );


/** Returns one (1) when there is callback generaated output in the bufferProcessor's
    internal buffer, and zero (0) when there is none. This method can be used
    to determine when it is appropriate to continue calling
    PaUtil_EndBufferProcessing() after it has returned a callbackResult of
    paComplete.
*/

int PaUtil_IsBufferProcessorOuputEmpty( PaUtilBufferProcessor* bufferProcessor );


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_PROCESS_H */
