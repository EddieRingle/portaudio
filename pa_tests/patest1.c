/** @file patest1.c
	@brief Ring modulate the audio input with a sine wave for 20 seconds.
	@todo needs to be updated to use the V19 API
	@author Ross Bencina <rossb@audiomulch.com>
*/
/*
 * $Id$
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
#ifndef M_PI
#define M_PI  (3.14159265)
#endif
typedef struct
{
    float sine[100];
    int phase;
    int sampsToGo;
}
patest1data;
static int patest1Callback( void *inputBuffer, void *outputBuffer,
                            unsigned long bufferFrames,
                            PaTime outTime, void *userData )
{
    patest1data *data = (patest1data*)userData;
    float *in = (float*)inputBuffer;
    float *out = (float*)outputBuffer;
    int framesToCalc = bufferFrames;
    unsigned long i;
    int finished = 0;
    /* Check to see if any input data is available. */
    if(inputBuffer == NULL) return 0; /* FIXME: no longer needed in V19 */
    if( data->sampsToGo < bufferFrames )
    {
        framesToCalc = data->sampsToGo;
        finished = 1;
    }
    for( i=0; i<framesToCalc; i++ )
    {
        *out++ = *in++ * data->sine[data->phase];  /* left */
        *out++ = *in++ * data->sine[data->phase++];  /* right */
        if( data->phase >= 100 )
            data->phase = 0;
    }
    data->sampsToGo -= framesToCalc;
    /* zero remainder of final buffer if not already done */
    for( ; i<bufferFrames; i++ )
    {
        *out++ = 0; /* left */
        *out++ = 0; /* right */
    }
    return finished;
}
int main(int argc, char* argv[]);
int main(int argc, char* argv[])
{
    PaStream *stream;
    PaError err;
    patest1data data;
    int i;
    int inputDevice = Pa_GetDefaultInputDevice();
    int outputDevice = Pa_GetDefaultOutputDevice();
    /* initialise sinusoidal wavetable */
    for( i=0; i<100; i++ )
        data.sine[i] = sin( ((double)i/100.) * M_PI * 2. );
    data.phase = 0;
    data.sampsToGo = 44100 * 20;   // 20 seconds
    /* initialise portaudio subsytem */
    Pa_Initialize();
    err = Pa_OpenStream(
              &stream,
              inputDevice,
              2,              /* stereo input */
              paFloat32,  /* 32 bit floating point input */
              NULL,
              outputDevice,
              2,              /* stereo output */
              paFloat32,      /* 32 bit floating point output */
              NULL,
              44100.,
              512,            /* small buffers */
              0,              /* let PA determine number of buffers */
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              patest1Callback,
              &data );
    if( err == paNoError )
    {
        err = Pa_StartStream( stream );
        printf( "Press any key to end.\n" );
        getc( stdin ); //wait for input before exiting
        Pa_AbortStream( stream );

        printf( "Waiting for stream to complete...\n" );

        while( Pa_IsStreamActive( stream ) )
            Pa_Sleep(1000); /* sleep until playback has finished */

        err = Pa_CloseStream( stream );
    }
    else
    {
        fprintf( stderr, "An error occured while opening the portaudio stream\n" );
        if( err == paHostError )
        {
            fprintf( stderr, "Host error number: %d\n", Pa_GetHostError() );
        }
        else
        {
            fprintf( stderr, "Error number: %d\n", err );
            fprintf( stderr, "Error text: %s\n", Pa_GetErrorText( err ) );
        }
    }
    Pa_Terminate();
    printf( "bye\n" );

    return 0;
}
