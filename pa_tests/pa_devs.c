/*
 * $Id$
 * pa_devs.c
 * List available devices.
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
/*******************************************************************/
int main(void);
int main(void)
{
    int      i,j;
    int      numDevices;
    const    PaDeviceInfo *deviceInfo;
    PaError  err;
    
    Pa_Initialize();

    printf("PortAudio Version Text = %s\nPortAudio Version Number = %d\n",
            Pa_GetVersionText(), Pa_GetVersion() );


    numDevices = Pa_CountDevices();
    if( numDevices < 0 )
    {
        printf("ERROR: Pa_CountDevices returned 0x%x\n", numDevices );
        err = numDevices;
        goto error;
    }
    
    printf("Number of devices = %d\n", numDevices );
    for( i=0; i<numDevices; i++ )
    {
        deviceInfo = Pa_GetDeviceInfo( i );
        printf("---------------------------------------------- #%d", i );
    /* Mark global default devices and API specific defaults. */
        if( i == Pa_GetDefaultInputDevice() ) printf(" Default Input");
        else if( i == Pa_HostApiDefaultInputDevice( deviceInfo->hostApi ) )
        {
            const PaHostApiInfo *hostInfo = Pa_GetHostApiInfo( deviceInfo->hostApi );
            printf(" Default %s Input", hostInfo->name );
        }
        
        if( i == Pa_GetDefaultOutputDevice() ) printf(" Default Output");
        else if( i == Pa_HostApiDefaultOutputDevice( deviceInfo->hostApi ) )
        {
            const PaHostApiInfo *hostInfo = Pa_GetHostApiInfo( deviceInfo->hostApi );
            printf(" Default %s Output", hostInfo->name );
        }

        printf("\nName                        = %s\n", deviceInfo->name );
        printf("Host API                    = %s\n",  Pa_GetHostApiInfo( deviceInfo->hostApi )->name );
        printf("Max Inputs = %d", deviceInfo->maxInputChannels  );
        printf(", Max Outputs = %d\n", deviceInfo->maxOutputChannels  );

        printf("Default Low Input Latency   = %8.3f\n", deviceInfo->defaultLowInputLatency  );
        printf("Default Low Output Latency  = %8.3f\n", deviceInfo->defaultLowOutputLatency  );
        printf("Default High Input Latency  = %8.3f\n", deviceInfo->defaultHighInputLatency  );
        printf("Default High Output Latency = %8.3f\n", deviceInfo->defaultHighOutputLatency  );

        printf("Default Sample Rate         = %8.2f\n", deviceInfo->defaultSampleRate  );

        
        /* FIXME: could list available sample rates here by polling
            Pa_IsFormatConverted()

        here's some sampling rates to check for

        static double possibleSampleRates_[]
        = {8000.0, 9600.0, 11025.0, 12000.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0};

        */

        /*
        if( deviceInfo->numSampleRates == -1 )
        {
            printf("Sample Rate Range = %f to %f\n", deviceInfo->sampleRates[0], deviceInfo->sampleRates[1] );
        }
        else
        {
            printf("Sample Rates =");
            for( j=0; j<deviceInfo->numSampleRates; j++ )
            {
                printf(" %8.2f,", deviceInfo->sampleRates[j] );
            }
            printf("\n");
        }
        */
       
        printf("\n");
    }
    Pa_Terminate();

    printf("----------------------------------------------\n");
    return 0;

error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return err;
}
