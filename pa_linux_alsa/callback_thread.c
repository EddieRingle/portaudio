#include <sys/poll.h>
#include <limits.h>
#include <math.h>  /* abs() */

#include "pa_linux_alsa.h"

void Stop( void *data );
char *ExtractAddress( const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset );

/*!
  \brief Poll on I/O filedescriptors

  We keep polling untill both all filedescriptors are ready (possibly both in and out), if either the capture
  or playback fd gets ready before the other we take it out of the equation
  */
static snd_pcm_sframes_t Wait( PaAlsaStream *stream )
{
    PaError result = paNoError;
    int needCapture = 0, needPlayback = 0;
    snd_pcm_sframes_t captureAvail = INT_MAX, playbackAvail = INT_MAX, commonAvail;
    struct pollfd *pfds = stream->pfds;
    int totalFds = stream->capture_nfds + stream->playback_nfds;

    assert( stream );

    if( stream->pcm_capture )
        needCapture = 1;

    if( stream->pcm_playback )
        needPlayback = 1;

    while( needCapture || needPlayback )
    {
	unsigned short revents;

        /* if the main thread has requested that we stop, do so now */
        pthread_testcancel();

        /*PA_DEBUG(( "still polling...\n" ));
        if( needCapture )
            PA_DEBUG(( "need capture.\n" ));
        if( needPlayback )
            PA_DEBUG(( "need playback.\n" )); */

        /* now poll on the combination of playback and capture fds. */
        ENSURE( poll( pfds, totalFds, 1000 ), paInternalError );

        /* check the return status of our pfds */
        if( needCapture )
        {
            ENSURE( snd_pcm_poll_descriptors_revents( stream->pcm_capture, stream->pfds,
                        stream->capture_nfds, &revents ), paUnanticipatedHostError );
            if( revents == POLLIN )
            {
                needCapture = 0;
                /* No need to keep polling on capture fd(s) */
                pfds += stream->capture_nfds;
                totalFds -= stream->capture_nfds;
            }
        }

        if( needPlayback )
        {
            unsigned short revents;
            ENSURE( snd_pcm_poll_descriptors_revents( stream->pcm_playback, stream->pfds +
                        stream->capture_nfds, stream->playback_nfds, &revents ), paUnanticipatedHostError );
            if( revents == POLLOUT )
            {
                needPlayback = 0;
                /* No need to keep polling on playback fd(s) */
                totalFds -= stream->playback_nfds;
            }
        }
    }

    /* we have now established that there are buffers ready to be
     * operated on.  Now determine how many frames are available. */
    if( stream->pcm_capture )
    {
        captureAvail = snd_pcm_avail_update( stream->pcm_capture );
        ENSURE( captureAvail, paUnanticipatedHostError );
    }

    if( stream->pcm_playback )
    {
        playbackAvail = snd_pcm_avail_update( stream->pcm_playback );
        ENSURE( playbackAvail, paUnanticipatedHostError );
    }

    commonAvail = MIN(captureAvail, playbackAvail);
    commonAvail -= commonAvail % stream->frames_per_period;

    return commonAvail;

error:
    return result;
}

snd_pcm_sframes_t SetUpBuffers( PaAlsaStream *stream, snd_pcm_uframes_t framesAvail )
{
    PaError result = paNoError;
    int i;
    snd_pcm_uframes_t captureFrames = INT_MAX, playbackFrames = INT_MAX, commonFrames;
    const snd_pcm_channel_area_t *areas, *area;
    char *buffer;

    assert( stream );

    if( stream->pcm_capture )
    {
        captureFrames = framesAvail;
        ENSURE( snd_pcm_mmap_begin( stream->pcm_capture, &areas, &stream->capture_offset, &captureFrames ),
                paUnanticipatedHostError );

        if( stream->capture_interleaved )
        {
            buffer = ExtractAddress( areas, stream->capture_offset );
            PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor,
                                                 0, /* starting at channel 0 */
                                                 buffer,
                                                 0  /* default numInputChannels */
                                               );
        }
        else
            /* noninterleaved */
            for( i = 0; i < stream->capture_channels; ++i )
            {
                area = &areas[i];
                buffer = ExtractAddress( area, stream->capture_offset );
                PaUtil_SetNonInterleavedInputChannel( &stream->bufferProcessor,
                                                      i,
                                                      buffer );
            }
    }

    if( stream->pcm_playback )
    {
        ENSURE( snd_pcm_mmap_begin( stream->pcm_playback, &areas, &stream->playback_offset, &playbackFrames ),
                paUnanticipatedHostError );

        if( stream->playback_interleaved )
        {
            buffer = ExtractAddress( areas, stream->playback_offset );
            PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor,
                                                 0, /* starting at channel 0 */
                                                 buffer,
                                                 0  /* default numInputChannels */
                                               );
        }
        else
            for( i = 0; i < stream->playback_channels; ++i )
            {
                area = &areas[i];
                buffer = ExtractAddress( area, stream->playback_offset );
                PaUtil_SetNonInterleavedOutputChannel( &stream->bufferProcessor,
                                                      i,
                                                      buffer );
            }
    }

    commonFrames = MIN(captureFrames, playbackFrames);
    commonFrames -= commonFrames % stream->frames_per_period;
    /*
    printf( "%d capture frames available\n", capture_framesAvail );
    printf( "%d frames playback available\n", playback_framesAvail );
    printf( "%d frames available\n", common_framesAvail );
    */

    if( stream->pcm_capture )
        PaUtil_SetInputFrameCount( &stream->bufferProcessor, commonFrames );

    if( stream->pcm_playback )
        PaUtil_SetOutputFrameCount( &stream->bufferProcessor, commonFrames );

    return (snd_pcm_sframes_t) commonFrames;

error:
    return result;
}

void *CallbackThread( void *userData )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*) userData;
    snd_pcm_sframes_t framesAvail, framesGot, framesProcessed;
    int *pres;

    assert( userData );

    pthread_cleanup_push( &Stop, stream );	/* Execute Stop on exit */

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, 44100.0 );

    while(1)
    {
        PaError callbackResult;
	PaStreamCallbackTimeInfo timeInfo = {0,0,0}; /* IMPLEMENT ME */

        pthread_testcancel();
        {
            /* calculate time info */
            snd_timestamp_t capture_timestamp;
            snd_timestamp_t playback_timestamp;
            snd_pcm_status_t *capture_status;
            snd_pcm_status_t *playback_status;
            snd_pcm_status_alloca( &capture_status );
            snd_pcm_status_alloca( &playback_status );

            if( stream->pcm_capture )
            {
                snd_pcm_status( stream->pcm_capture, capture_status );
                snd_pcm_status_get_tstamp( capture_status, &capture_timestamp );
            }
            if( stream->pcm_playback )
            {
                snd_pcm_status( stream->pcm_playback, playback_status );
                snd_pcm_status_get_tstamp( playback_status, &playback_timestamp );
            }

            /* Hmm, we potentially have both a playback and a capture timestamp.
             * Hopefully they are the same... */
            if( stream->pcm_capture && stream->pcm_playback )
            {
                float capture_time = capture_timestamp.tv_sec +
                                     ((float)capture_timestamp.tv_usec/1000000);
                float playback_time= playback_timestamp.tv_sec +
                                     ((float)playback_timestamp.tv_usec/1000000);
                if( fabsf(capture_time-playback_time) > 0.01 )
                    PA_DEBUG(("Capture time and playback time differ by %f\n", fabsf(capture_time-playback_time)));
                timeInfo.currentTime = capture_time;
            }
            else if( stream->pcm_playback )
            {
                timeInfo.currentTime = playback_timestamp.tv_sec +
                                       ((float)playback_timestamp.tv_usec/1000000);
            }
            else
            {
                timeInfo.currentTime = capture_timestamp.tv_sec +
                                       ((float)capture_timestamp.tv_usec/1000000);
            }

            if( stream->pcm_capture )
            {
                snd_pcm_sframes_t capture_delay = snd_pcm_status_get_delay( capture_status );
                timeInfo.inputBufferAdcTime = timeInfo.currentTime -
                    (float)capture_delay / stream->streamRepresentation.streamInfo.sampleRate;
            }

            if( stream->pcm_playback )
            {
                snd_pcm_sframes_t playback_delay = snd_pcm_status_get_delay( playback_status );
                timeInfo.outputBufferDacTime = timeInfo.currentTime +
                    (float)playback_delay / stream->streamRepresentation.streamInfo.sampleRate;
            }
        }


        /*
            IMPLEMENT ME:
                - handle buffer slips
        */

        /*
            depending on whether the host buffers are interleaved, non-interleaved
            or a mixture, you will want to call PaUtil_ProcessInterleavedBuffers(),
            PaUtil_ProcessNonInterleavedBuffers() or PaUtil_ProcessBuffers() here.
        */

        framesAvail = Wait( stream );
        ENSURE( framesAvail, framesAvail );     /* framesAvail might contain an error (negative value) */
        while( framesAvail > 0 )
        {
            PaStreamCallbackFlags cbFlags = 0;

            pthread_testcancel();

            /* Priming output */
            if( stream->startThreshold > 0 )
            {
                if( stream->pcm_capture )
                    PaUtil_SetNoInput( &stream->bufferProcessor );
                framesAvail = MIN( framesAvail, stream->startThreshold );
            }

            /* Now we know the soundcard is ready to produce/receive at least
             * one period.  We just need to get the buffers for the client
             * to read/write. */
            PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, cbFlags );

            framesGot = SetUpBuffers( stream, framesAvail );
            ENSURE( framesGot, framesGot );             /* framesGot might contain an error (negative) */

            PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

            callbackResult = paContinue;

            /* this calls the callback */
            framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                                          &callbackResult );

            PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

            /* inform ALSA how many frames we wrote */
            if( stream->pcm_capture )
                ENSURE( snd_pcm_mmap_commit( stream->pcm_capture, stream->capture_offset, framesGot ), paUnanticipatedHostError );
            if( stream->pcm_playback )
                ENSURE( snd_pcm_mmap_commit( stream->pcm_playback, stream->playback_offset, framesGot ), paUnanticipatedHostError );

            if( callbackResult != paContinue )
                break;

            framesAvail -= framesGot;
            if( stream->startThreshold > 0 )
                stream->startThreshold -= framesGot;

	    PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
        }


        /*
            If you need to byte swap outputBuffer, you can do it here using
            routines in pa_byteswappers.h
        */

        if( callbackResult != paContinue )
        {
            stream->callback_finished = 1;
            stream->callbackAbort = (callbackResult == paAbort);

            goto end;
            
        }
    }

    /* This code is unreachable, but important to include regardless because it
     * is possibly a macro with a closing brace to match the opening brace in
     * pthread_cleanup_push() above.  The documentation states that they must
     * always occur in pairs. */
    pthread_cleanup_pop( 1 );	/* Execute Stop on exit */

end:
    pthread_exit( NULL );

error:
    /* Pass on error code */
    pres = malloc( sizeof (int) );
    *pres = (result);
    
    pthread_exit( pres );
}

void Stop( void *data )
{
    PaAlsaStream *stream = (PaAlsaStream *) data;

    assert( data );

    if( stream->callbackAbort )
    {
        if( stream->pcm_playback )
            snd_pcm_drop( stream->pcm_playback );
        if( stream->pcm_capture && !stream->pcmsSynced )
            snd_pcm_drop( stream->pcm_capture );

        PA_DEBUG(( "Dropped frames\n" ));
        stream->callbackAbort = 0;
    }
    else
    {
        if( stream->pcm_playback )
            snd_pcm_drain( stream->pcm_playback );
        if( stream->pcm_capture && !stream->pcmsSynced )
            snd_pcm_drain( stream->pcm_capture );
    }

    PA_DEBUG(( "Stoppage\n" ));

    /* Eventually notify user all buffers have played */
    if( stream->streamRepresentation.streamFinishedCallback )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
}

char *ExtractAddress( const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset )
{
    return (char *) area->addr + (area->first + offset * area->step) / 8;
}
