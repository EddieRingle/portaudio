#ifndef PA_LINUX_ALSA_H
#define PA_LINUX_ALSA_H

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#undef ALSA_PCM_NEW_HW_PARAMS_API
#undef ALSA_PCM_NEW_SW_PARAMS_API

#include <pthread.h>

#include "pa_util.h"
#include "pa_process.h"
#include "pa_cpuload.h"
#include "pa_stream.h"

#define MIN(x,y) ( (x) < (y) ? (x) : (y) )

#define STRINGIZE_HELPER(exp) #exp
#define STRINGIZE(exp) STRINGIZE_HELPER(exp)

/* TODO: Isolate call to SetLastHostErrorInfo with mutex? */
#define ENSURE(exp, code) \
    if( (exp) < 0 ) \
    { \
        if( (code) == paUnanticipatedHostError ) \
        { \
            PaUtil_SetLastHostErrorInfo( paALSA, (exp), snd_strerror( (exp) ) ); \
        } \
        PA_DEBUG(( "Expression '" #exp "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = (code); \
        goto error; \
    }

#define UNLESS(exp, code) \
    if( (exp) == 0 ) \
    { \
        PA_DEBUG(( "Expression '" #exp "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" )); \
        result = (code); \
        goto error; \
    }

typedef struct PaAlsaStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;

    snd_pcm_t *pcm_capture;
    snd_pcm_t *pcm_playback;

    int callback_finished;      /* bool: are we in the "callback finished" state? See if stream has been stopped in background */

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

    int pcmsSynced;		/* Have we successfully synced pcms */
    int callbackAbort;		/* Drop frames? */
    snd_pcm_uframes_t startThreshold;
}
PaAlsaStream;

#endif
