/** @file paqa_errs.c
	@brief Self Testing Quality Assurance app for PortAudio
	Do lots of bad things to test error reporting.
	@author Phil Burk  http://www.softsynth.com
    
    Pieter Suurmond attempted to change to V19 API, but some
    strange things happen if one changes the two '#if (0)'s 
    below to '#if (1)'. The other 13 tests pass all right.
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

/*--------- Definitions ---------*/
#define MODE_INPUT        (0)
#define MODE_OUTPUT       (1)
#define FRAMES_PER_BUFFER (64)
#define SAMPLE_RATE       (44100.0)

typedef struct PaQaData
{
    unsigned long  framesLeft;
    int            numChannels;
    int            bytesPerSample;
    int            mode;
}
PaQaData;

static int gNumPassed = 0; /* Two globals */
static int gNumFailed = 0;

/*------------------- Macros ------------------------------*/
/* Print ERROR if it fails. Tally success or failure. Odd  */
/* do-while wrapper seems to be needed for some compilers. */

#define EXPECT(_exp) \
    do \
    { \
        if ((_exp)) {\
            gNumPassed++; \
        } \
        else { \
            printf("\nERROR - 0x%x - %s for %s\n", result, Pa_GetErrorText(result), #_exp ); \
            gNumFailed++; \
            goto error; \
        } \
    } while(0)

#define HOPEFOR(_exp) \
    do \
    { \
        if ((_exp)) {\
            gNumPassed++; \
        } \
        else { \
            printf("\nERROR - 0x%x - %s for %s\n", result, Pa_GetErrorText(result), #_exp ); \
            gNumFailed++; \
        } \
    } while(0)

/*-------------------------------------------------------------------------*/
/* This routine will be called by the PortAudio engine when audio is needed.
   It may be called at interrupt level on some machines so don't do anything
   that could mess up the system like calling malloc() or free().
*/
static int QaCallback( const void*                      inputBuffer,
                       void*                            outputBuffer,
                       unsigned long                    framesPerBuffer,
			           const PaStreamCallbackTimeInfo*  timeInfo,
			           PaStreamCallbackFlags            statusFlags,
                       void*                            userData )
{
    unsigned long   i;
    unsigned char*  out = (unsigned char *) outputBuffer;
    PaQaData*       data = (PaQaData *) userData;
    
    (void)inputBuffer; /* Prevent "unused variable" warnings. */

    /* Zero out buffer so we don't hear terrible noise. */
    if( data->mode == MODE_OUTPUT )
    {
        unsigned long numBytes = framesPerBuffer * data->numChannels * data->bytesPerSample;
        for( i=0; i<numBytes; i++ )
        {
            *out++ = 0;
        }
    }
    /* Are we through yet? */
    if( data->framesLeft > framesPerBuffer )
    {
        data->framesLeft -= framesPerBuffer;
        return 0;
    }
    else
    {
        data->framesLeft = 0;
        return 1;
    }
}

/*-------------------------------------------------------------------------------------------------*/
static int TestBadOpens( void )
{
    PaStream*           stream = NULL;
    PaError             result;
    PaQaData            myData;
    PaStreamParameters  ipp, opp;
    
    /* Setup data for synthesis thread. */
    myData.framesLeft = (unsigned long) (SAMPLE_RATE * 100); /* 100 seconds */
    myData.numChannels = 1;
    myData.mode = MODE_OUTPUT;

    /*----------------------------- No devices specified: */
    ipp.device                    = opp.device                    = paNoDevice;
    ipp.channelCount              = opp.channelCount              = 0; /* Also no channels. */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    /* Take the low latency of the default device for all subsequent tests. */
    ipp.suggestedLatency          = Pa_GetDeviceInfo(Pa_GetDefaultInputDevice())->defaultLowInputLatency;
    opp.suggestedLatency          = Pa_GetDeviceInfo(Pa_GetDefaultOutputDevice())->defaultLowOutputLatency;
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paInvalidDevice));

    /*----------------------------- Out of range input device specified: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = Pa_GetDeviceCount(); /* And no output device, and no channels. */
    opp.channelCount = 0;           opp.device = paNoDevice;
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paInvalidDevice));

    /*----------------------------- Out of range output device specified: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = paNoDevice; /* And no input device, and no channels. */
    opp.channelCount = 0;           opp.device = Pa_GetDeviceCount();
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paInvalidDevice));

    /*----------------------------- Zero input channels: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = Pa_GetDefaultInputDevice();
    opp.channelCount = 0;           opp.device = paNoDevice;    /* And no output device, and no output channels. */   
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paInvalidChannelCount));

/****************************************
MAKE THIS (1) and get a very weird error:  Invalid device instead of paInvalidChannelCount
****************************************/
#if (0)

    /*----------------------------- Zero output channels: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = paNoDevice; /* And no input device, and no input channels. */
    opp.channelCount = 0;           opp.device = Pa_GetDefaultOutputDevice();
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paInvalidChannelCount));
#endif

    /*----------------------------- Nonzero input and output channels but no output device: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 2;           ipp.device = Pa_GetDefaultInputDevice();        /* Both stereo. */
    opp.channelCount = 2;           opp.device = paNoDevice;
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paInvalidDevice));

    /*----------------------------- Nonzero input and output channels but no input device: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 2;           ipp.device = paNoDevice;
    opp.channelCount = 2;           opp.device = Pa_GetDefaultOutputDevice();
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paInvalidDevice));

    /*----------------------------- NULL stream pointer: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = paNoDevice;           /* Output is more likely than input. */
    opp.channelCount = 2;           opp.device = Pa_GetDefaultOutputDevice();    /* Only 2 output channels. */
    HOPEFOR(((result = Pa_OpenStream(NULL, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paBadStreamPtr));

/***************************************
MAKE THIS (1) and get very weird errors: Invalid device instead of paInvalidSampleRate,
***************************************  Invalid device instead of paNullCallback,
                                         Invalid device instead of paInvalidFlag. */
#if (0)
    /*----------------------------- Low sample rate: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = paNoDevice;
    opp.channelCount = 2;           opp.device = Pa_GetDefaultOutputDevice();
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     1.0, FRAMES_PER_BUFFER, /* 1 cycle per second (1 Hz) is too low. */
                                     paClipOff, QaCallback, &myData )) == paInvalidSampleRate));

    /*----------------------------- High sample rate: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = paNoDevice;
    opp.channelCount = 2;           opp.device = Pa_GetDefaultOutputDevice();
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     10000000.0, FRAMES_PER_BUFFER, /* 10^6 cycles per second (10 MHz) is too high. */
                                     paClipOff, QaCallback, &myData )) == paInvalidSampleRate));

    /*----------------------------- NULL callback: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = paNoDevice;
    opp.channelCount = 2;           opp.device = Pa_GetDefaultOutputDevice();
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff,
                                     NULL,
                                     &myData )) == paNullCallback));

    /*----------------------------- Bad flag: */
    ipp.hostApiSpecificStreamInfo = opp.hostApiSpecificStreamInfo = NULL;
    ipp.sampleFormat              = opp.sampleFormat              = paFloat32;
    ipp.channelCount = 0;           ipp.device = paNoDevice;
    opp.channelCount = 2;           opp.device = Pa_GetDefaultOutputDevice();
    HOPEFOR(((result = Pa_OpenStream(&stream, &ipp, &opp,
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     255,                      /* Is 8 maybe legal V19 API? */
                                     QaCallback, &myData )) == paInvalidFlag));
#endif


#if 0 /* FIXME - this is legal for some implementations. */
    HOPEFOR( ( /* Use input device as output device. */
                 (result = Pa_OpenStream(
                               &stream,
                               paNoDevice, 0, paFloat32, NULL,
                               Pa_GetDefaultInputDeviceID(), 2, paFloat32, NULL,
                               SAMPLE_RATE, FRAMES_PER_BUFFER, NUM_BUFFERS,
                               paClipOff, QaCallback, &myData )
                 ) == paInvalidDeviceId) );

    HOPEFOR( ( /* Use output device as input device. */
                 (result = Pa_OpenStream(
                               &stream,
                               Pa_GetDefaultOutputDeviceID(), 2, paFloat32, NULL,
                               paNoDevice, 0, paFloat32, NULL,
                               SAMPLE_RATE, FRAMES_PER_BUFFER, NUM_BUFFERS,
                               paClipOff, QaCallback, &myData )
                 ) == paInvalidDeviceId) );
#endif

    if( stream != NULL ) Pa_CloseStream( stream );
    return result;
}

/*-----------------------------------------------------------------------------------------*/
static int TestBadActions( void )
{
    PaStream*           stream = NULL;
    PaError             result;
    PaQaData            myData;
    PaStreamParameters  opp;

    /* Setup data for synthesis thread. */
    myData.framesLeft = (unsigned long)(SAMPLE_RATE * 100); /* 100 seconds */
    myData.numChannels = 1;
    myData.mode = MODE_OUTPUT;

    opp.device                    = Pa_GetDefaultOutputDevice(); /* Default output. */
    opp.channelCount              = 2;                           /* Stereo output.  */
    opp.hostApiSpecificStreamInfo = NULL;
    opp.sampleFormat              = paFloat32;
    opp.suggestedLatency          = Pa_GetDeviceInfo(opp.device)->defaultLowOutputLatency;

    HOPEFOR(((result = Pa_OpenStream(&stream, NULL, /* Take NULL as input parame-     */
                                     &opp,          /* ters, meaning try only output. */
                                     SAMPLE_RATE, FRAMES_PER_BUFFER,
                                     paClipOff, QaCallback, &myData )) == paNoError));

    HOPEFOR(((result = Pa_StartStream(NULL))    == paBadStreamPtr));
    HOPEFOR(((result = Pa_StopStream(NULL))     == paBadStreamPtr));
    HOPEFOR(((result = Pa_IsStreamActive(NULL)) == paBadStreamPtr));
    HOPEFOR(((result = Pa_CloseStream(NULL))    == paBadStreamPtr));

    /* Unfortunately, the following two checks (that come from V18) could not be performed 
       under the V19 API:
    // Does not result an int. Nothing is said about the absolute time of Pa_GetStreamTime()!
    HOPEFOR(((result = Pa_GetStreamTime(NULL)) != 0.0));
    // Passing NULL as PaUtilCpuLoadMeasurer* should result no load at all?
    HOPEFOR(((result = (PaError)PaUtil_GetCpuLoad(NULL)) != 0)); */

    if (stream != NULL) Pa_CloseStream(stream);
    return result;
}

/*---------------------------------------------------------------------*/
int main(void);
int main(void)
{
    PaError result;
    
    EXPECT(((result = Pa_Initialize()) == paNoError));
    TestBadOpens();
    TestBadActions();
error:
    Pa_Terminate();
    printf("QA Report: %d passed, %d failed.\n", gNumPassed, gNumFailed);
    return 0;
}
