/*
 * $Id$
 * patest_sine_time.c
 * Play a sine wave using the Portable Audio api for several seconds.
 * Pausing in the middle.
 * use the Pa_GetStreamTime() and Pa_IsStreamActive() calls.
 *
 * Authors:
 *    Ross Bencina <rossb@audiomulch.com>
 *    Phil Burk <philburk@softsynth.com>
 *
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.audiomulch.com/portaudio/
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
#include "pa_util.h"
#define NUM_SECONDS   (8)
#define SAMPLE_RATE   (44100)
#define FRAMES_PER_BUFFER  (64)
#define NUM_BUFFERS   (0)

#ifndef M_PI
#define M_PI  (3.14159265)
#endif
#define TWOPI (M_PI * 2.0)

#define TABLE_SIZE   (200)
typedef struct
{
    double           left_phase;
    double           right_phase;
    volatile PaTimestamp   outTime;
}
paTestData;

/* This routine will be called by the PortAudio engine when audio is needed.
** It may called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int patestCallback(   void *inputBuffer, void *outputBuffer,
                             unsigned long framesPerBuffer,
                             PaTimestamp outTime, void *userData )
{
    paTestData *data = (paTestData*)userData;
    float *out = (float*)outputBuffer;
    unsigned int i;

    double left_phaseInc = 0.02;
    double right_phaseInc = 0.06;

    double left_phase = data->left_phase;
    double right_phase = data->right_phase;

    (void) outTime; /* Prevent unused variable warnings. */
    (void) inputBuffer;
    data->outTime = outTime;\

    for( i=0; i<framesPerBuffer; i++ )
    {
        left_phase += left_phaseInc;
        if( left_phase > TWOPI ) left_phase -= TWOPI;
        *out++ = (float) sin( left_phase );

        right_phase += right_phaseInc;
        if( right_phase > TWOPI ) right_phase -= TWOPI;
        *out++ = (float) sin( right_phase );
    }

    data->left_phase = left_phase;
    data->right_phase = right_phase;

    return paContinue;
}
/*******************************************************************/
static void ReportStreamTime( PaStream *stream, paTestData *data );
static void ReportStreamTime( PaStream *stream, paTestData *data )
{
    PaTimestamp  streamTime, latency, outTime;
    
    streamTime = Pa_GetStreamTime( stream );
    outTime = data->outTime;
    if( outTime < 0.0 )
    {
        printf("Stream time = %8.1f\n", streamTime );
    }
    else
    {
        latency = outTime - streamTime;
        printf("Stream time = %8.1f, outTime = %8.1f, latency = %8.1f\n",
            streamTime, outTime, latency );
    }
    fflush(stdout);
}

/*******************************************************************/
int main(void);
int main(void)
{
    PaStream *stream;
    PaError err;
    paTestData DATA;
    int totalSamps;
    printf("PortAudio Test: output sine wave. SR = %d, BufSize = %d\n", SAMPLE_RATE, FRAMES_PER_BUFFER);
    DATA.left_phase = DATA.right_phase = 0;
    totalSamps =  NUM_SECONDS * SAMPLE_RATE; /* Play for a few seconds. */
    err = Pa_Initialize();
    if( err != paNoError ) goto error;
    err = Pa_OpenStream(
              &stream,
              paNoDevice,/* default input device */
              0,              /* no input */
              paFloat32,  /* 32 bit floating point input */
              0, /* default latency */
              NULL,
              Pa_GetDefaultOutputDevice(), /* default output device */
              2,          /* stereo output */
              paFloat32,      /* 32 bit floating point output */
              0, /* default latency */
              NULL,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,            /* frames per buffer */
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              patestCallback,
              &DATA );
    if( err != paNoError ) goto error;
          
    /* Watch until sound is halfway finished. */
    printf("Play for %d seconds.\n", NUM_SECONDS/2 ); fflush(stdout);

    DATA.outTime = -1.0; // mark time for callback as undefined
    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;
    do
    {
        ReportStreamTime( stream, &DATA );
        Pa_Sleep(100);
    } while( Pa_GetStreamTime( stream ) < (totalSamps/2) );
    
    /* Stop sound until ENTER hit. */
    err = Pa_StopStream( stream );
    if( err != paNoError ) goto error;
    printf("Pause for 2 seconds.\n"); fflush(stdout);
    Pa_Sleep( 2000 );

    DATA.outTime = -1.0; // mark time for callback as undefined
    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;
    
    printf("Play until sound is finished.\n"); fflush(stdout);
    do
    {
        ReportStreamTime( stream, &DATA );
        Pa_Sleep(100);
    } while( Pa_GetStreamTime( stream ) < (totalSamps/2) );
    
    err = Pa_CloseStream( stream );
    if( err != paNoError ) goto error;
    Pa_Terminate();
    printf("Test finished.\n");
    return err;
error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return err;
}
