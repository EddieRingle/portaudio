#include <sys/poll.h>
#include <limits.h>
#include <math.h>  /* abs() */

#include "pa_linux_alsa.h"

void OnExit( void *data );
unsigned char *ExtractAddress( const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset );

/* Atomic restart of stream (we don't want the intermediate state visible) */
static PaError AlsaRestart( PaAlsaStream *stream )
{
    PaError result = paNoError;

    PA_DEBUG(( "Restarting audio\n" ));

    pthread_mutex_lock( &stream->mtx );
    PA_ENSURE( AlsaStop( stream, 0 ) );
    PA_ENSURE( AlsaStart( stream, 0 ) );

    PA_DEBUG(( "Restarted audio\n" ));

end:
    pthread_mutex_unlock( &stream->mtx );
    return result;
error:
   goto end;
}

/* Report xrun/restart audio */
static void HandleXrun( PaAlsaStream *stream, PaTime *underrun, PaTime *overrun )
{
    snd_pcm_status_t *st;
    PaTime now = PaUtil_GetTime();
    snd_timestamp_t t;

    snd_pcm_status_alloca( &st );

    if( stream->pcm_playback )
    {
        snd_pcm_status( stream->pcm_playback, st );
        if( snd_pcm_status_get_state( st ) == SND_PCM_STATE_XRUN )
        {
            snd_pcm_status_get_trigger_tstamp( st, &t );
            *underrun = now * 1000 - ((PaTime) t.tv_sec * 1000 + (PaTime) t.tv_usec / 1000);
        }
    }
    if( stream->pcm_capture )
    {
        snd_pcm_status( stream->pcm_capture, st );
        if( snd_pcm_status_get_state( st ) == SND_PCM_STATE_XRUN )
        {
            snd_pcm_status_get_trigger_tstamp( st, &t );
            *overrun = now * 1000 - ((PaTime) t.tv_sec * 1000 + (PaTime) t.tv_usec / 1000);
        }
    }

    AlsaRestart( stream );
}

/*!
  \brief Poll on I/O filedescriptors

  Poll till we've determined there's data for read or write, doing it simple for now.
  We might go on with output even if we're starved for input etc, so don't hang around
  forever waiting for both read and write.
  */
static snd_pcm_sframes_t Wait( PaAlsaStream *stream, PaTime *underrun, PaTime *overrun )
{
    PaError result = paNoError;
    int doPoll = 1; // needCapture = 0, needPlayback = 0;
    snd_pcm_sframes_t captureAvail = 0, playbackAvail = 0, commonAvail;
    struct pollfd *pfds = stream->pfds;
    int totalFds = stream->capture_nfds + stream->playback_nfds;
    int xrun = 0;   /* Under/overrun? */

    assert( stream );

    *underrun = *overrun = 0.0;

    /*
    if( stream->pcm_capture )
        needCapture = 1;

    if( stream->pcm_playback )
        needPlayback = 1;
    */

    int cnt = 0;
    while( doPoll )
    {
        ++cnt;
	unsigned short revents;

        /* if the main thread has requested that we stop, do so now */
        pthread_testcancel();

        /* now poll on the combination of playback and capture fds. */
        ENSURE( poll( pfds, totalFds, 1000 ), paInternalError );

        /* check the return status of our pfds */
        if( stream->pcm_capture )
        {
            ENSURE( snd_pcm_poll_descriptors_revents( stream->pcm_capture, stream->pfds,
                        stream->capture_nfds, &revents ), paUnanticipatedHostError );
            if( revents & POLLERR )
                xrun = 1;
            if( revents )
            {
                doPoll = 0;
                /* No need to keep polling on capture fd(s) */
                /*
                pfds += stream->capture_nfds;
                totalFds -= stream->capture_nfds;
                */
            }
        }

        if( stream->pcm_playback )
        {
            unsigned short revents;
            ENSURE( snd_pcm_poll_descriptors_revents( stream->pcm_playback, stream->pfds +
                        stream->capture_nfds, stream->playback_nfds, &revents ), paUnanticipatedHostError );
            if( revents & POLLERR )
                xrun = 1;
            if( revents )
            {
                doPoll = 0;
                /* No need to keep polling on playback fd(s) */
                /*totalFds -= stream->playback_nfds;*/
            }
                PA_DEBUG(("Poll timed out for the %dst time\n\n", cnt ));
        }
    }

    /* we have now established that there are buffers ready to be
     * operated on.  Now determine how many frames are available. */
    if( stream->pcm_capture )
    {
        captureAvail = snd_pcm_avail_update( stream->pcm_capture );
        if( captureAvail == -EPIPE )
            xrun = 1;
        else
            ENSURE( captureAvail, paUnanticipatedHostError );
    }

    if( stream->pcm_playback )
    {
        playbackAvail = snd_pcm_avail_update( stream->pcm_playback );
        if( playbackAvail == -EPIPE )
            xrun = 1;
        else
            ENSURE( playbackAvail, paUnanticipatedHostError );
    }

    if( xrun )
    {
        HandleXrun( stream, underrun, overrun );
        return 0;
    }

    commonAvail = MAX(captureAvail, playbackAvail); /* Will get rid of all data, somehow */
    commonAvail -= commonAvail % stream->frames_per_period;

    PA_DEBUG(( "Wait: captureAvail: %d, playbackAvail: %d, commonAvail: %d\n", captureAvail, playbackAvail, commonAvail ));

    return commonAvail;

error:
    return result;
}

/* Get buffers from ALSA for read/write, and determine the amount of frames available.
   Underflow/underflow complicates matters
   */
static snd_pcm_sframes_t SetUpBuffers( PaAlsaStream *stream )
{
    PaError result = paNoError;
    int i;
    snd_pcm_uframes_t captureFrames = INT_MAX, playbackFrames = INT_MAX, commonFrames;
    const snd_pcm_channel_area_t *areas, *area;
    unsigned char *buffer;
    snd_pcm_sframes_t retVal;

    assert( stream );

    if( stream->pcm_capture )
    {
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


    playbackFrames -= (playbackFrames % stream->frames_per_period);
    captureFrames -= (captureFrames % stream->frames_per_period);

    /* Will be used to set PortAudio buffer size */
    commonFrames = MIN(captureFrames, playbackFrames);
    retVal = commonFrames;

    if( stream->pcm_playback && stream->pcm_capture )
    {
        /* Full-duplex, but we are starved for data in either end
           If we're out of input, go on. Input buffer will be zeroed.
           In the case of output underflow, drop input frames unless stream->neverDropInput.
           If we're starved for output, while keeping input, we need to pass a dummy pointer
           to the callback.
       */
        if( !commonFrames ) 
        {
            retVal = MAX( playbackFrames, captureFrames );

            if( !captureFrames )    /* Input underflow */
                commonFrames = playbackFrames;  /* We still want output */
            else                    /* Output underflow */
                if( stream->neverDropInput )    /* Output underflow, but do not drop input */
                    commonFrames = captureFrames;
        }
        else    /* Safe to commit commonFrames for both */
            playbackFrames = captureFrames = commonFrames;
    }
    
    if( stream->pcm_capture )
    {
        if( captureFrames || !commonFrames )
            PaUtil_SetInputFrameCount( &stream->bufferProcessor, commonFrames );
        else    /* We have input underflow */
            PaUtil_SetNoInput( &stream->bufferProcessor );
    }
    if( stream->pcm_playback )
    {
        if( playbackFrames || !commonFrames )
            PaUtil_SetOutputFrameCount( &stream->bufferProcessor, commonFrames );
        /*
        else    // We have output underflow, but keeping input data
            PaUtil_SetNoOutput( &stream->bufferProcessor ); // XXX
        */
    }

    // PA_DEBUG(( "SetUpBuffers: captureAvail: %d, playbackAvail: %d, commonFrames: %d\n\n", captureFrames, playbackFrames, commonFrames ));
    stream->playbackAvail = playbackFrames;
    stream->captureAvail = captureFrames;

    return retVal;

error:
    return (snd_pcm_sframes_t) result;
}

void *CallbackThread( void *userData )
{
    PaError result = paNoError;
    PaAlsaStream *stream = (PaAlsaStream*) userData;
    snd_pcm_sframes_t framesAvail, framesGot, framesProcessed;
    int *pres;
    PaTime underrun = 0.0, overrun = 0.0;

    assert( userData );

    pthread_cleanup_push( &OnExit, stream );	/* Execute OnExit when exiting */

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, 44100.0 );

    if( stream->startThreshold <= 0 )   /* Just fill buffer with silence before start */
        PA_ENSURE( AlsaStart( stream, 0 ) );

    while(1)
    {
        PaError callbackResult;
	PaStreamCallbackTimeInfo timeInfo = {0,0,0}; /* IMPLEMENT ME */
        PaStreamCallbackFlags cbFlags = 0;

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
            depending on whether the host buffers are interleaved, non-interleaved
            or a mixture, you will want to call PaUtil_ProcessInterleavedBuffers(),
            PaUtil_ProcessNonInterleavedBuffers() or PaUtil_ProcessBuffers() here.
        */

        if( underrun != 0.0 )
            cbFlags |= paOutputUnderflow;
        if( overrun != 0.0 )
            cbFlags |= paInputOverflow;

        framesAvail = Wait( stream, &underrun, &overrun );
        ENSURE( framesAvail, framesAvail );     /* framesAvail might contain an error (negative value) */
        while( framesAvail > 0 )
        {
            pthread_testcancel();

            /* Priming output */
            if( stream->startThreshold > 0 )
            {
                PA_DEBUG(( "Priming\n" ));
                cbFlags |= paPrimingOutput;
                framesAvail = MIN( framesAvail, stream->startThreshold );
            }

            /* now we know the soundcard is ready to produce/receive at least
             * one period.  we just need to get the buffers for the client
             * to read/write. */
            PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, cbFlags );

            framesGot = SetUpBuffers( stream );
            ENSURE( framesGot, framesGot );             /* framesGot might contain an error (negative) */
            /* Check for underflow/overflow */
            if( stream->pcm_playback && stream->pcm_capture )
            {
                if( !stream->captureAvail )
                {
                    cbFlags |= paInputUnderflow;
                    PA_DEBUG(( "Input underflow\n" ));
                }
                if( !stream->playbackAvail )
                {
                    if( !framesGot )    /* The normal case, dropping input */
                    {
                        cbFlags |= paInputOverflow;
                        PA_DEBUG(( "Input overflow\n" ));
                    }
                    else                /* Keeping input */
                    {
                        cbFlags |= paOutputOverflow;
                        PA_DEBUG(( "Output overflow\n" ));
                    }
                }
            }

            PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

            callbackResult = paContinue;

            /* this calls the callback */
            framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor,
                                                          &callbackResult );
            PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

            /* Inform ALSA how many frames we read/wrote
               Now, this number may differ between capture and playback, due to under/overflow.
               If we're dropping input frames, we effectively sink them here.
             */
            if( stream->pcm_capture )
                ENSURE( snd_pcm_mmap_commit( stream->pcm_capture, stream->capture_offset, stream->captureAvail ),
                        paUnanticipatedHostError );
            if( stream->pcm_playback )
                ENSURE( snd_pcm_mmap_commit( stream->pcm_playback, stream->playback_offset, stream->playbackAvail ),
                        paUnanticipatedHostError );

            /* If threshold for starting stream specified (priming buffer), decrement and compare */
            if( stream->startThreshold > 0 )
            {
                if( (stream->startThreshold -= framesGot) <= 0 )
                    PA_ENSURE( AlsaStart( stream, 1 ) );
            }

            if( callbackResult != paContinue )
                break;

            framesAvail -= framesGot;
            if( stream->startThreshold > 0 )
                stream->startThreshold -= framesGot;
        }


        /*
            If you need to byte swap outputBuffer, you can do it here using
            routines in pa_byteswappers.h
        */

        if( callbackResult != paContinue )
        {
            stream->callbackAbort = (callbackResult == paAbort);
            goto end;
            
        }
    }

    /* This code is unreachable, but important to include regardless because it
     * is possibly a macro with a closing brace to match the opening brace in
     * pthread_cleanup_push() above.  The documentation states that they must
     * always occur in pairs. */
    pthread_cleanup_pop( 1 );

end:
    pthread_exit( NULL );

error:
    /* Pass on error code */
    pres = malloc( sizeof (int) );
    *pres = (result);
    
    pthread_exit( pres );
}

void OnExit( void *data )
{
    PaAlsaStream *stream = (PaAlsaStream *) data;

    assert( data );

    stream->callback_finished = 1;  /* Let the outside world know stream was stopped in callback */

    AlsaStop( stream, stream->callbackAbort );
    if( stream->callbackAbort )
        stream->callbackAbort = 0;
    
    PA_DEBUG(( "Stoppage\n" ));

    /* Eventually notify user all buffers have played */
    if( stream->streamRepresentation.streamFinishedCallback )
        stream->streamRepresentation.streamFinishedCallback( stream->streamRepresentation.userData );
}

unsigned char *ExtractAddress( const snd_pcm_channel_area_t *area, snd_pcm_uframes_t offset )
{
    return (unsigned char *) area->addr + (area->first + offset * area->step) / 8;
}
