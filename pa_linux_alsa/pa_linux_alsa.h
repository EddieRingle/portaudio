
#include <alsa/asoundlib.h>

#include <pthread.h>

#include "pa_util.h"
#include "pa_process.h"
#include "pa_cpuload.h"
#include "pa_stream.h"

typedef struct PaAlsaStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    snd_pcm_t *pcm_capture;
    snd_pcm_t *pcm_playback;

    int callback_finished;      /* bool: are we in the "callback finished" state? */

    int frames_per_period;
    int playback_hostsampleformat;

    int capture_channels;
    int playback_channels;

    int capture_interleaved;    /* bool: is capture interleaved? */
    int playback_interleaved;   /* bool: is playback interleaved? */

    int callback_mode;          /* bool: are we running in callback mode? */
    pthread_t callback_thread;

    /* the callback thread uses these to poll the sound device, waiting
     * for data to be ready/available */
    unsigned int capture_nfds;
    unsigned int playback_nfds;
    struct pollfd *pfds;

    /* these aren't really stream state, the callback uses them */
    snd_pcm_uframes_t capture_offset;
    snd_pcm_uframes_t playback_offset;
}
PaAlsaStream;

