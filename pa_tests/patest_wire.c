/*
 * $Id$
 * patest_wire.c
 *
 * Pass input directly to output.
 * Note that some HW devices, for example many ISA audio cards
 * on PCs, do NOT support full duplex! For a PC, you normally need
 * a PCI based audio card such as the SBLive.
 *
 * Author: Phil Burk  http://www.softsynth.com
 *
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.portaudio.com
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
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
 *
 */
#include <stdio.h>
#include <math.h>
#include "portaudio.h"
/*
** Note that many of the older ISA sound cards on PCs do NOT support
** full duplex audio (simultaneous record and playback).
** And some only support full duplex at lower sample rates.
*/
#define SAMPLE_RATE            (44100)
#define FRAMES_PER_BUFFER      (64)
#define NUM_INPUT_CHANNELS     (2)
#define NUM_OUTPUT_CHANNELS    (2)

#if 0
    #define FLAG_INTERLEAVED   (paNonInterleaved)
    #define CALLBACK_FUNCTION  wireCallbackNonInterleaved
#else
    #define FLAG_INTERLEAVED   (0)
    #define CALLBACK_FUNCTION  wireCallback
#endif

#if 0
    #define INPUT_FORMAT  paFloat32
    typedef float INPUT_SAMPLE;
#else
    #define INPUT_FORMAT  paInt16
    typedef short INPUT_SAMPLE;
#endif

#if 1
    #define OUTPUT_FORMAT  paFloat32
    typedef float OUTPUT_SAMPLE;
#else
    #define OUTPUT_FORMAT  paInt16
    typedef short OUTPUT_SAMPLE;
#endif

double gInOutScaler = 1.0;
#define CONVERT_IN_TO_OUT(in)  ((OUTPUT_SAMPLE) ((in) * gInOutScaler))

static int wireCallback( void *inputBuffer, void *outputBuffer,
                         unsigned long framesPerBuffer,
                         PaTimestamp outTime, void *userData );

/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int wireCallback( void *inputBuffer, void *outputBuffer,
                         unsigned long framesPerBuffer,
                         PaTimestamp outTime, void *userData )
{
    OUTPUT_SAMPLE *out = (OUTPUT_SAMPLE*)outputBuffer;
    INPUT_SAMPLE *in = (INPUT_SAMPLE*)inputBuffer;

    unsigned int i;
    (void) outTime;
    /* This may get called with NULL inputBuffer during initial setup. */
    if( inputBuffer == NULL) return 0;

    for( i=0; i<framesPerBuffer; i++ )
    {
        OUTPUT_SAMPLE left = CONVERT_IN_TO_OUT( *in++ );  /* left */
        *out++ = left;
#if ( NUM_OUTPUT_CHANNELS == 2 )
    #if ( NUM_OUTPUT_CHANNELS == 2 )
            *out++ = CONVERT_IN_TO_OUT( *in++ );  /* right */
    #else
            *out++ = left;  /* right output */
    #endif
#endif
    }

    return 0;
}

static int wireCallbackNonInterleaved( void *inputBuffer, void *outputBuffer,
                         unsigned long framesPerBuffer,
                         PaTimestamp outTime, void *userData )
{
    OUTPUT_SAMPLE **outBuffers = (OUTPUT_SAMPLE**)outputBuffer;
    INPUT_SAMPLE **inBuffers = (INPUT_SAMPLE**)inputBuffer;
    int inDone = 0;
    int outDone = 0;

    unsigned int i, inChannel, outChannel;
    (void) outTime;

    /* This may get called with NULL inputBuffer during initial setup. */
    if( inputBuffer == NULL) return 0;

    inChannel=0, outChannel=0;
    while( !(inDone && outDone) )
    {
        INPUT_SAMPLE *in = inBuffers[inChannel];
        OUTPUT_SAMPLE *out = outBuffers[outChannel];

        if( INPUT_FORMAT == OUTPUT_FORMAT )
        {
                for( i=0; i<framesPerBuffer; i++ )
                {
                    *out++ = CONVERT_IN_TO_OUT( *in++ );
                }
        }

        if(inChannel < (NUM_INPUT_CHANNELS - 1)) inChannel++;
        else inDone = 1;
        if(outChannel < (NUM_OUTPUT_CHANNELS - 1)) outChannel++;
        else outDone = 1;
    }
    return 0;
}

/*******************************************************************/
int main(void);
int main(void)
{
    PaStream *stream;
    PaError err;
   
    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    if( FLAG_INTERLEAVED == paNonInterleaved )
    {
        printf("PortAudio Test: NON interleaved!\n" );
    }
    printf("PortAudio Test: input channels = %d\n", NUM_INPUT_CHANNELS );
    printf("PortAudio Test: output channels = %d\n", NUM_OUTPUT_CHANNELS );
    printf("PortAudio Test: input format = %d\n", INPUT_FORMAT );
    printf("PortAudio Test: output format = %d\n", OUTPUT_FORMAT );
    printf("PortAudio Test: input device ID  = %d\n", Pa_GetDefaultInputDevice() );
    printf("PortAudio Test: output device ID = %d\n", Pa_GetDefaultOutputDevice() );

    if( INPUT_FORMAT == OUTPUT_FORMAT )
    {
        gInOutScaler = 1.0;
    }
    else if( (INPUT_FORMAT == paInt16) && (OUTPUT_FORMAT == paFloat32) )
    {
        gInOutScaler = 1.0/32768.0;
    }
    else if( (INPUT_FORMAT == paFloat32) && (OUTPUT_FORMAT == paInt16) )
    {
        gInOutScaler = 32768.0;
    }

    err = Pa_OpenStream(
              &stream,
              Pa_GetDefaultInputDevice(), /* default output device */
              NUM_INPUT_CHANNELS,
              INPUT_FORMAT | FLAG_INTERLEAVED,
              0,               /* input latency */
              NULL,
              Pa_GetDefaultOutputDevice(), /* default output device */
              NUM_OUTPUT_CHANNELS,
              OUTPUT_FORMAT | FLAG_INTERLEAVED,
              0,               /* output latency */
              NULL,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,            /* frames per buffer */
              paClipOff,       /* we won't output out of range samples so don't bother clipping them */
              CALLBACK_FUNCTION,
              NULL );          /* no data */
    if( err != paNoError ) goto error;
    
    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;
    
    printf("Full duplex sound test in progress.\n");
    printf("Hit ENTER to exit test.\n");  fflush(stdout);
    getchar();
    
    printf("Closing stream.\n");
    err = Pa_CloseStream( stream );
    if( err != paNoError ) goto error;
    Pa_Terminate();
    
    printf("Full duplex sound test complete.\n"); fflush(stdout);
    return 0;
error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return -1;
}
