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


typedef struct PaUtilChannelDescriptor PaUtilChannelDescriptor;


typedef struct {
    unsigned long framesPerUserBuffer;
    unsigned long framesPerHostBuffer;

    unsigned int numInputChannels;
    unsigned int bytesPerHostInputSample;
    unsigned int bytesPerUserInputSample;
    int userInputIsInterleaved;
    PaUtilConverter *inputConverter;        /* NULL if no converter is required */

    unsigned int numOutputChannels;
    unsigned int bytesPerHostOutputSample;
    unsigned int bytesPerUserOutputSample;
    int userOutputIsInterleaved;
    PaUtilConverter *outputConverter;       /* NULL if no converter is required */

    void *tempInputBuffer;          /* used for slips, block adaption, and conversion. */
    void **tempInputBufferPtrs;     /* storage for non-interleaved buffer pointers, NULL for interleaved user input */
    void *tempOutputBuffer;         /* used for slips, block adaption, and conversion. */
    void **tempOutputBufferPtrs;    /* storage for non-interleaved buffer pointers, NULL for interleaved user output */

    PaUtilTriangularDitherGenerator ditherGenerator;

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
            unsigned long framesPerUserBuffer, unsigned long framesPerHostBuffer,
            PortAudioCallback *userCallback, void *userData );
/**< interleave flag is ignored for host buffer formats.. choose between the Process()
functions below instead.


*/

void PaUtil_TerminateBufferProcessor( PaUtilBufferProcessor* bufferProcessor );


int PaUtil_ProcessInterleavedBuffers( PaUtilBufferProcessor* bufferProcessor,
        void *input, void *output, PaTimestamp outTime );


int PaUtil_ProcessNonInterleavedBuffers( PaUtilBufferProcessor* bufferProcessor,
        void *input, void *output, PaTimestamp outTime );
        

typedef struct PaUtilChannelDescriptor{
    unsigned char *data;
    unsigned int stride;
}PaUtilChannelDescriptor;

int PaUtil_ProcessBuffers( PaUtilBufferProcessor* bufferProcessor,
        PaUtilChannelDescriptor *input, PaUtilChannelDescriptor *output,
        PaTimestamp outTime );
/**< 
    @param bufferProcessor Pointer to a buffer processor state data struct
        previously initialized with PaUtil_InitializeBufferProcessor()
        
    @param input A pointer to the first element in an array of
        PaUtilChannelDescriptors. Each of these elements points to the
        first sample of the channel, and provides a stride parameter for
        handling interleaved, non-interleaved and partially interleaved streams.
        NULL may be passed if no input buffer is available such as a half
        duplex stream, or during a buffer slip.

    @param output A pointer to the first element in an array of
        PaUtilChannelDescriptors. Each of these elements points to the
        first sample of the channel, and provides a stride parameter for
        handling interleaved, non-interleaved and partially interleaved streams.
        NULL may be passed if no input buffer is available such as a half
        duplex stream, or during a buffer slip.

    @param outTime The time at which the first sample of output will reach
        the dacs.

    @return The value returned by the user callback.

    @see PaUtil_ProcessInterleavedBuffers, PaUtil_ProcessNonInterleavedBuffers
*/


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_PROCESS_H */
