#ifndef PA_STREAM_H
#define PA_STREAM_H
/*
 * $Id$
 * Portable Audio I/O Library
 * stream interface
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

#include "portaudio.h"

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


#define PA_STREAM_MAGIC (0x18273645)

typedef struct {
    /*
        all PaStreamInterface functions are guaranteed to be called with a
        non-null, valid <stream> parameter
    */
    PaError (*Close)( PaStream* stream );
    PaError (*Start)( PaStream *stream );
    PaError (*Stop)( PaStream *stream );
    PaError (*Abort)( PaStream *stream );
    PaError (*IsStopped)( PaStream *stream );
    PaError (*IsActive)( PaStream *stream );
    PaTime (*GetInputLatency)( PaStream* );
    PaTime (*GetOutputLatency)( PaStream* );
    PaTime (*GetTime)( PaStream *stream );
    double (*GetCpuLoad)( PaStream* stream );
    PaError (*Read)( PaStream* stream, void *buffer, unsigned long frames );
    PaError (*Write)( PaStream* stream, void *buffer, unsigned long frames );
    unsigned long (*GetReadAvailable)( PaStream* stream );
    unsigned long (*GetWriteAvailable)( PaStream* stream );
} PaUtilStreamInterface;


void PaUtil_InitializeStreamInterface( PaUtilStreamInterface *streamInterface,
    PaError (*Close)( PaStream* ),
    PaError (*Start)( PaStream* ),
    PaError (*Stop)( PaStream* ),
    PaError (*Abort)( PaStream* ),
    PaError (*IsStopped)( PaStream* ),
    PaError (*IsActive)( PaStream* ),
    PaTime (*GetInputLatency)( PaStream* ),
    PaTime (*GetOutputLatency)( PaStream* ),
    PaTime (*GetTime)( PaStream* ),
    double (*GetCpuLoad)( PaStream* ),
    PaError (*Read)( PaStream* stream, void *buffer, unsigned long frames ),
    PaError (*Write)( PaStream* stream, void *buffer, unsigned long frames ),
    unsigned long (*GetReadAvailable)( PaStream* stream ),
    unsigned long (*GetWriteAvailable)( PaStream* stream ) );


/*
    use PaUtil_DummyReadWrite and PaUtil_DummyGetAvailable for
    callback based streams.
*/
PaError PaUtil_DummyReadWrite( PaStream* stream,
                       void *buffer,
                       unsigned long frames );

                       
unsigned long PaUtil_DummyGetAvailable( PaStream* stream );

/*
    use PaUtil_DummyGetCpuLoad for read/write streams
*/
double PaUtil_DummyGetCpuLoad( PaStream* stream );


typedef struct PaUtilStreamRepresentation {
    unsigned long magic;    /* set to PA_STREAM_MAGIC */
    struct PaUtilStreamRepresentation *nextOpenStream; /* field used by multi-api code */
    PaUtilStreamInterface *streamInterface;
    PortAudioCallback *callback;
    void *userData;
} PaUtilStreamRepresentation;


void PaUtil_InitializeStreamRepresentation( PaUtilStreamRepresentation *streamRepresentation,
    PaUtilStreamInterface *streamInterface,
    PortAudioCallback callback,
    void *userData );

void PaUtil_TerminateStreamRepresentation( PaUtilStreamRepresentation *streamRepresentation );


#define PA_STREAM_INTERFACE( streamRepPtr )\
    ((PaUtilStreamRepresentation*) streamRepPtr )->streamInterface


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_STREAM_H */
