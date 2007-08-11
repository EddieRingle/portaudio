/*
 * PortAudio Portable Real-Time Audio Library
 * Windows WAVEFORMAT* data structure utilities
 * portaudio.h should be included before this file.
 *
 * Copyright (c) 2007 Ross Bencina
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however, 
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also 
 * requested that these non-binding requests be included along with the 
 * license above.
 */

#include "portaudio.h"
#include "pa_win_waveformat.h"

#include <windows.h>
#include <mmsystem.h>


static GUID pawin_ksDataFormatSubtypePcm = 
	{ (USHORT)(WAVE_FORMAT_PCM), 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };


#if !defined(WAVE_FORMAT_IEEE_FLOAT)
#define  WAVE_FORMAT_IEEE_FLOAT 0x0003
#endif

static GUID pawin_ksDataFormatSubtypeIeeeFloat = 
	{ (USHORT)(WAVE_FORMAT_IEEE_FLOAT), 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 };



void PaWin_InitializeWaveFormatEx( PaWinWaveFormat *waveFormat, 
		int numChannels, PaSampleFormat sampleFormat, double sampleRate,
		int bytesPerHostSample )
{
	WAVEFORMATEX *waveFormatEx = (WAVEFORMATEX*)waveFormat;
	unsigned long bytesPerFrame = numChannels * bytesPerHostSample;

	waveFormatEx->wFormatTag = WAVE_FORMAT_PCM;
	waveFormatEx->nChannels = (WORD)numChannels;
	waveFormatEx->nSamplesPerSec = (DWORD)sampleRate;
	waveFormatEx->nAvgBytesPerSec = waveFormatEx->nSamplesPerSec * bytesPerFrame;
	waveFormatEx->nBlockAlign = (WORD)bytesPerFrame;
	waveFormatEx->wBitsPerSample = bytesPerHostSample * 8;
	waveFormatEx->cbSize = 0;
}


void PaWin_InitializeWaveFormatExtensible( PaWinWaveFormat *waveFormat, 
		int numChannels, PaSampleFormat sampleFormat, double sampleRate,
		int bytesPerHostSample, PaWinWaveFormatChannelMask channelMask )
{
	WAVEFORMATEX *waveFormatEx = (WAVEFORMATEX*)waveFormat;
	unsigned long bytesPerFrame = numChannels * bytesPerHostSample;

#if !defined(WAVE_FORMAT_EXTENSIBLE)
#define  WAVE_FORMAT_EXTENSIBLE                 0xFFFE
#endif

	waveFormatEx->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	waveFormatEx->nChannels = (WORD)numChannels;
	waveFormatEx->nSamplesPerSec = (DWORD)sampleRate;
	waveFormatEx->nAvgBytesPerSec = waveFormatEx->nSamplesPerSec * bytesPerFrame;
	waveFormatEx->nBlockAlign = (WORD)bytesPerFrame;
	waveFormatEx->wBitsPerSample = bytesPerHostSample * 8;
	waveFormatEx->cbSize = 22;

	*((WORD*)&waveFormat->fields[PAWIN_INDEXOF_WVALIDBITSPERSAMPLE]) =
			waveFormatEx->wBitsPerSample;

	*((DWORD*)&waveFormat->fields[PAWIN_INDEXOF_DWCHANNELMASK]) = channelMask;
			
	*((GUID*)&waveFormat->fields[PAWIN_INDEXOF_SUBFORMAT]) =
			pawin_ksDataFormatSubtypePcm;
}


PaWinWaveFormatChannelMask PaWin_DefaultChannelMask( int numChannels )
{
	switch( numChannels ){
		case 1:
			return PAWIN_SPEAKER_MONO;
		case 2:
			return PAWIN_SPEAKER_STEREO; 
		//case 3:
		//	break;
		case 4:
			return PAWIN_SPEAKER_QUAD;
		//case 5:
		//	break;
		case 6:
			return PAWIN_SPEAKER_5POINT1_SURROUND; 
			break;
		//case 7:
		//	break;
		case 8:
			return PAWIN_SPEAKER_7POINT1_SURROUND;
	}

	return  PAWIN_SPEAKER_DIRECTOUT;
}
