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


typedef enum {
    paUtilFixedHostBufferSize,
    paUtilBoundedHostBufferSize,
    paUtilUnknownHostBufferSize,
    paUtilVariableHostBufferSizePartialUsageAllowed,   /* the only mode where process() may not consume the whole buffer */
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

    unsigned int numInputChannels;
    unsigned int bytesPerHostInputSample;
    unsigned int bytesPerUserInputSample;
    int userInputIsInterleaved;
    PaUtilConverter *inputConverter;

    unsigned int numOutputChannels;
    unsigned int bytesPerHostOutputSample;
    unsigned int bytesPerUserOutputSample;
    int userOutputIsInterleaved;
    PaUtilConverter *outputConverter;

    void *tempInputBuffer;          /* used for slips, block adaption, and conversion. */
    void **tempInputBufferPtrs;     /* storage for non-interleaved buffer pointers, NULL for interleaved user input */
    unsigned long framesInTempInputBuffer; /* frames remaining in input buffer from previous adaption iteration */

    void *tempOutputBuffer;         /* used for slips, block adaption, and conversion. */
    void **tempOutputBufferPtrs;    /* storage for non-interleaved buffer pointers, NULL for interleaved user output */
    unsigned long framesInTempOutputBuffer; /* frames remaining in input buffer from previous adaption iteration */

    PaTime hostOutTime;
    
    unsigned long hostInputFrameCount[2];
    PaUtilChannelDescriptor *hostInputChannels[2];
    unsigned long hostOutputFrameCount[2];
    PaUtilChannelDescriptor *hostOutputChannels[2];

    PaUtilTriangularDitherGenerator ditherGenerator;

    double samplePeriod;

    PortAudioCallback *userCallback;
    void *userData;
} PaUtilBufferProcessor;


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
            PortAudioCallback *userCallback, void *userData );
/**<

    @param framesPerHostBuffer Specifies the number of frames per host buffer
    for fixed the fixed buffer size mode, and the maximum number of frames
    per host buffer for the bounded host buffer size mode. It is ignored for
    the other modes.
    
    @note The interleave flag is ignored for host buffer formats. Host interleave
    is determined by the use of different SetInput and SetOutput functions.
*/

void PaUtil_TerminateBufferProcessor( PaUtilBufferProcessor* bufferProcessor );


void PaUtil_BeginBufferProcessing( PaUtilBufferProcessor* bufferProcessor, PaTime outTime );

unsigned long PaUtil_EndBufferProcessing( PaUtilBufferProcessor* bufferProcessor, int *callbackResult );
/*<< returns the number of frames processed */

void PaUtil_SetInputFrameCount( PaUtilBufferProcessor* bufferProcessor,
        unsigned long frameCount );
/*<< a 0 frameCount indicates to use the framesPerHostBuffer value passed to init */

void PaUtil_SetInputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data, unsigned int stride );

void PaUtil_SetInterleavedInputChannels( PaUtilBufferProcessor* bufferProcessor,
        unsigned int firstChannel, void *data, unsigned int channelCount );
/**< if channel count is zero use all channels as specified to initialize buffer processor */

void PaUtil_SetNonInterleavedInputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data );


void PaUtil_Set2ndInputFrameCount( PaUtilBufferProcessor* bufferProcessor,
        unsigned long frameCount );

void PaUtil_Set2ndInputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data, unsigned int stride );

void PaUtil_Set2ndInterleavedInputChannels( PaUtilBufferProcessor* bufferProcessor,
        unsigned int firstChannel, void *data, unsigned int channelCount );
/**< if channel count is zero use all channels as specified to initialize buffer processor */

void PaUtil_Set2ndNonInterleavedInputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data );


void PaUtil_SetOutputFrameCount( PaUtilBufferProcessor* bufferProcessor,
        unsigned long frameCount );
/*<< a 0 frameCount indicates to use the framesPerHostBuffer value passed to init */

void PaUtil_SetOutputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data, unsigned int stride );

void PaUtil_SetInterleavedOutputChannels( PaUtilBufferProcessor* bufferProcessor,
        unsigned int firstChannel, void *data, unsigned int channelCount );
/**< if channel count is zero use all channels as specified to initialize buffer processor */

void PaUtil_SetNonInterleavedOutputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data );


void PaUtil_Set2ndOutputFrameCount( PaUtilBufferProcessor* bufferProcessor,
        unsigned long frameCount );

void PaUtil_Set2ndOutputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data, unsigned int stride );

void PaUtil_Set2ndInterleavedOutputChannels( PaUtilBufferProcessor* bufferProcessor,
        unsigned int firstChannel, void *data, unsigned int channelCount );
/**< if channel count is zero use all channels as specified to initialize buffer processor */

void PaUtil_Set2ndNonInterleavedOutputChannel( PaUtilBufferProcessor* bufferProcessor,
        unsigned int channel, void *data );

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_PROCESS_H */
