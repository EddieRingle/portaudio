/* This file is included in pa_mac_core.c. It contains the implementation
 * required for blocking I/O. It is separated from pa_mac_core.c simply to ease
 * development. */

/*
 * Functions for initializing, resetting, and destroying BLIO structures.
 *
 */

/* This should be called with the relevant info when initializing a stream for
   callback. */
static PaError initializeBlioRingBuffers(
                                       PaMacBlio *blio,
                                       PaSampleFormat inputSampleFormat,
                                       PaSampleFormat outputSampleFormat,
                                       size_t framesPerBuffer,
                                       long ringBufferSize,
                                       int inChan,
                                       int outChan )
{
   void *data;
   int result;

   /* zeroify things */
   bzero( blio, sizeof( PaMacBlio ) );
   /* this is redundant, but the buffers are used to check
      if the bufffers have been initialized, so we do it explicitly. */
   blio->inputRingBuffer.buffer = NULL;
   blio->outputRingBuffer.buffer = NULL;

   /* initialize simple data */
   blio->inputSampleFormat = inputSampleFormat;
   blio->inputSampleSize = computeSampleSizeFromFormat(inputSampleFormat);
   blio->outputSampleFormat = outputSampleFormat;
   blio->outputSampleSize = computeSampleSizeFromFormat(outputSampleFormat);
   blio->framesPerBuffer = framesPerBuffer;
   blio->inChan = inChan;
   blio->outChan = outChan;
   blio->statusFlags = 0;
   blio->errors = paNoError;

   /* setup ring buffers */
   if( inChan ) {
      data = calloc( ringBufferSize, blio->inputSampleSize );
      if( !data )
      {
         result = paInsufficientMemory;
         goto error;
      }

      assert( 0 == RingBuffer_Init(
            &blio->inputRingBuffer,
            ringBufferSize*blio->inputSampleSize,
            data ) );
   }
   if( outChan ) {
      data = calloc( ringBufferSize, blio->outputSampleSize );
      if( !data )
      {
         result = paInsufficientMemory;
         goto error;
      }

      assert( 0 == RingBuffer_Init(
            &blio->outputRingBuffer,
            ringBufferSize*blio->outputSampleSize,
            data ) );
   }

   resetBlioRingBuffers( blio );

   return 0;

 error:
   destroyBlioRingBuffers( blio );
   return result;
}

/* This should be called after stopping or aborting the stream, so that on next
   start, the buffers will be ready. */
static void resetBlioRingBuffers( PaMacBlio *blio )
{
   if( blio->outputRingBuffer.buffer ) {
      RingBuffer_Flush( &blio->outputRingBuffer );
      bzero( blio->outputRingBuffer.buffer,
             blio->outputRingBuffer.bufferSize );
      /* Advance buffer */
      RingBuffer_AdvanceWriteIndex( &blio->outputRingBuffer,
                                    blio->framesPerBuffer*blio->outChan*blio->outputSampleSize );
/*
      printf( "------%d\n" ,  blio->framesPerBuffer );
      printf( "------%d\n" ,  blio->outChan );
      printf( "------%d\n" ,  blio->outputSampleSize );
      printf( "------%d\n" ,  blio->framesPerBuffer*blio->outChan*blio->outputSampleSize );
*/
   }
   if( blio->inputRingBuffer.buffer ) {
      RingBuffer_Flush( &blio->inputRingBuffer );
      bzero( blio->inputRingBuffer.buffer,
             blio->inputRingBuffer.bufferSize );
   }
}

/*This should be called when you are done with the blio. It can safely be called
  multiple times. */
static void destroyBlioRingBuffers( PaMacBlio *blio )
{
   if( blio->inputRingBuffer.buffer )
      free( blio->inputRingBuffer.buffer );
   blio->inputRingBuffer.buffer = NULL;
   if( blio->outputRingBuffer.buffer )
      free( blio->outputRingBuffer.buffer );
   blio->outputRingBuffer.buffer = NULL;
}

/*
 * this is the BlioCallback function. It expects to recieve a PaMacBlio Object
 * pointer as userData.
 *
 */
static int BlioCallback( const void *input, void *output, unsigned long frameCount,
	const PaStreamCallbackTimeInfo* timeInfo,
        PaStreamCallbackFlags statusFlags,
        void *userData )
{
   PaMacBlio *blio = (PaMacBlio*)userData;
   long avail;
   long toRead;
   long toWrite;

   /* set flags returned by OS: */
   blio->statusFlags |= statusFlags ;

   /* --- Handle Input Buffer --- */
   if( blio->inChan ) {
      avail = RingBuffer_GetWriteAvailable( &blio->inputRingBuffer );

      /* check for underflow */
      if( avail < frameCount * blio->inputSampleSize * blio->inChan )
         blio->statusFlags |= paInputOverflow;

      toRead = MIN( avail, frameCount * blio->inputSampleSize * blio->inChan );

      /* copy the data */
      /*printf( "reading %d\n", toRead );*/
      assert( toRead == RingBuffer_Write( &blio->inputRingBuffer, input, toRead ) );
   }


   /* --- Handle Output Buffer --- */
   if( blio->outChan ) {
      avail = RingBuffer_GetReadAvailable( &blio->outputRingBuffer );

      /* check for underflow */
      if( avail < frameCount * blio->outputSampleSize * blio->outChan )
         blio->statusFlags |= paOutputUnderflow;

      toWrite = MIN( avail, frameCount * blio->outputSampleSize * blio->outChan );

      if( toWrite != frameCount * blio->outputSampleSize * blio->outChan )
         bzero( output+toWrite,
                frameCount * blio->outputSampleSize * blio->outChan - toWrite );
      /* copy the data */
      /*printf( "writing %d\n", toWrite );*/
      assert( toWrite == RingBuffer_Read( &blio->outputRingBuffer, output, toWrite ) );
   }

   return paContinue;
}

/* FIXME: test that buffer is fully played out on stop */

static PaError ReadStream( PaStream* stream,
                           void *buffer,
                           unsigned long frames )
{
    PaMacBlio *blio = & ((PaMacCoreStream*)stream) -> blio;
    char *cbuf = (char *) buffer;
    VVDBUG(("ReadStream()\n"));
    PaError ret;

    while( frames > 0 ) {
       long avail = 0;
       long toRead;
       while( avail == 0 ) {
          avail = RingBuffer_GetReadAvailable( &blio->inputRingBuffer );
          /*FIXME: do a true block.*/
          if( avail == 0 )
             Pa_Sleep( PA_MAC_BLIO_BUSY_WAIT_SLEEP_INTERVAL );
       }
       toRead = MIN( avail, frames * blio->inputSampleSize * blio->inChan );
       toRead -= toRead % blio->inputSampleSize * blio->inChan ;
       RingBuffer_Read( &blio->inputRingBuffer, (void *)cbuf, toRead );
       cbuf += toRead;
       frames -= toRead / ( blio->inputSampleSize * blio->inChan );
    }

    /*   Report either paNoError or paOutputUnderflowed. */
    /*   may also want to report other errors, but this is non-standard. */
    ret = blio->statusFlags & paInputOverflow;

    /* report overflow only once: */
    blio->statusFlags &= ~paInputOverflow;

    return ret;
}


static PaError WriteStream( PaStream* stream,
                            const void *buffer,
                            unsigned long frames )
{
    PaMacBlio *blio = & ((PaMacCoreStream*)stream) -> blio;
    char *cbuf = (char *) buffer;
    VVDBUG(("WriteStream()\n"));
    PaError ret;

    while( frames > 0 ) {
       long avail = 0;
       long toWrite;
       while( avail == 0 ) {
          avail = RingBuffer_GetWriteAvailable( &blio->outputRingBuffer );
          /*FIXME: do a true block.*/
          if( avail == 0 )
             Pa_Sleep( PA_MAC_BLIO_BUSY_WAIT_SLEEP_INTERVAL );
       }
       toWrite = MIN( avail, frames * blio->outputSampleSize * blio->outChan );
       toWrite -= toWrite % blio->outputSampleSize * blio->outChan ;
       RingBuffer_Write( &blio->outputRingBuffer, (void *)cbuf, toWrite );
       cbuf += toWrite;
       frames -= toWrite / ( blio->outputSampleSize * blio->outChan );
    }

    /*   Report either paNoError or paOutputUnderflowed. */
    /*   may also want to report other errors, but this is non-standard. */
    ret = blio->statusFlags & paOutputUnderflow;

    /* report underflow only once: */
    blio->statusFlags &= ~paOutputUnderflow;

    return ret;
}

/*
 *
 */
static void waitUntilBlioWriteBufferIsFlushed( PaMacBlio *blio )
{
    if( blio->outputRingBuffer.buffer ) {
       long avail = RingBuffer_GetWriteAvailable( &blio->outputRingBuffer );
       while( avail != blio->outputRingBuffer.bufferSize ) {
          /*FIXME: do a true block.*/
          if( avail == 0 )
             Pa_Sleep( PA_MAC_BLIO_BUSY_WAIT_SLEEP_INTERVAL );
          avail = RingBuffer_GetWriteAvailable( &blio->outputRingBuffer );
       }
    }
}


static signed long GetStreamReadAvailable( PaStream* stream )
{
    PaMacBlio *blio = & ((PaMacCoreStream*)stream) -> blio;
    VVDBUG(("GetStreamReadAvailable()\n"));

    return RingBuffer_GetReadAvailable( &blio->inputRingBuffer )
                         / ( blio->outputSampleSize * blio->outChan );
}


static signed long GetStreamWriteAvailable( PaStream* stream )
{
    PaMacBlio *blio = & ((PaMacCoreStream*)stream) -> blio;
    VVDBUG(("GetStreamWriteAvailable()\n"));

    return RingBuffer_GetWriteAvailable( &blio->outputRingBuffer )
                         / ( blio->outputSampleSize * blio->outChan );
}

