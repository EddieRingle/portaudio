
#include "pa_stream.h"

#include "pa_linux_alsa.h"

PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    /* TODO: handle failure, xruns */

    if( stream->capture_interleaved )
    {
        snd_pcm_mmap_readi( stream->pcm_capture, buffer, frames );
    }
    else
    {
        snd_pcm_mmap_readn( stream->pcm_capture, (void**)buffer, frames );
    }

    return paNoError;
}


PaError WriteStream( PaStream* s,
                            void *buffer,
                            unsigned long frames )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    if( stream->playback_interleaved )
    {
        snd_pcm_mmap_writei( stream->pcm_playback, buffer, frames );
    }
    else
    {
        snd_pcm_mmap_writen( stream->pcm_playback, (void**)buffer, frames );
    }

    return paNoError;
}


unsigned long GetStreamReadAvailable( PaStream* s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    return snd_pcm_avail_update( stream->pcm_capture );
}


unsigned long GetStreamWriteAvailable( PaStream* s )
{
    PaAlsaStream *stream = (PaAlsaStream*)s;

    return snd_pcm_avail_update( stream->pcm_playback );
}


