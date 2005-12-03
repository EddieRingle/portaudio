/*
 * This is a special purpose ring buffer for holding input data
 * from AUHAL until the corresponding output is ready.
 *
 */

/* FIXME: double chack that this AudioBufferList stuff is portable.
          See TN2113. */

typedef enum {
      WRITE_SAFE = 0,
      WRITING,
      READ_SAFE,
      READING
} BufferState;

typedef struct {
   size_t size;
   AudioBufferList *buffers;
   BufferState *state;
   size_t readIdx;
   size_t writeIdx;
   size_t totalFrames;
} InputRingBuffer ;

/*
 * "lag" means that a certain number of buffers should be zeroed
 *   and cosidered already written-to and ready to be read from.
 *
 */
void ResetRingBuffer( InputRingBuffer *irb, size_t lag, bool wipe )
{
   bzero( irb->state, sizeof( BufferState ) * irb->size );

   irb->readIdx = 0;
   if( lag == 0 || lag >= irb->size )
      irb->writeIdx = 0;
   else {
      size_t i;
      irb->writeIdx = irb->size - lag;
      for( i=0; i<irb->writeIdx; ++i )
         irb->state[i] = READ_SAFE;
   }

   if( wipe )
      bzero( irb->buffers[0].mBuffers[0].mData,
             irb->totalFrames * sizeof( float ) );
}

PaError InitializeRingBuffer( InputRingBuffer *irb, size_t size,
                              size_t lag, UInt32 channels, UInt32 frames )
{
   size_t i;
   float * alloc;

   irb->size = size;
   irb->readIdx = irb->writeIdx = 0;

   irb->buffers = (AudioBufferList *) calloc( size, sizeof(AudioBufferList) );
   if( !irb->buffers )
      return paInsufficientMemory;

   irb->totalFrames = size * frames * channels ;
   alloc = (float *) calloc( irb->totalFrames, sizeof(float) );
   if( !alloc ) {
      free( irb->buffers );
      return paInsufficientMemory;
   }
   irb->state = (BufferState *) calloc( irb->size, sizeof( BufferState ) );
   if( !irb->state ) {
      free( alloc );
      free( irb->buffers );
      return paInsufficientMemory;
   }

   for( i=0; i<size; ++i )
   {
      irb->buffers[i].mNumberBuffers = 1;
      irb->buffers[i].mBuffers[0].mNumberChannels=channels;
      irb->buffers[i].mBuffers[0].mDataByteSize  =frames*channels*sizeof(float);
      irb->buffers[i].mBuffers[0].mData          =alloc;
      alloc += frames*channels;
   }

   ResetRingBuffer( irb, lag, false );

   return paNoError;
}

void DisposeRingBuffer( InputRingBuffer *irb )
{
   free( irb->buffers[0].mBuffers[0].mData );
   free( irb->buffers );
}

AudioBufferList *GetBufferForWrite( InputRingBuffer *irb )
{
   if( irb->state[ irb->writeIdx ] != WRITE_SAFE )
      return NULL;
   irb->state[ irb->writeIdx ] = WRITING;
   return irb->buffers + irb->writeIdx;
}

void DoneWithWrite( InputRingBuffer *irb )
{
   irb->state[ irb->writeIdx ] = READ_SAFE;
   irb->writeIdx = ( irb->writeIdx + 1 ) % irb->size;
}

AudioBufferList *GetBufferForRead( InputRingBuffer *irb )
{
   if( irb->state[ irb->readIdx ] != READ_SAFE )
      return NULL;
   irb->state[ irb->readIdx ] = READING;
   return irb->buffers + irb->readIdx;
}

void DoneWithRead( InputRingBuffer *irb )
{
   irb->state[ irb->readIdx ] = WRITE_SAFE;
   irb->readIdx = ( irb->readIdx + 1 ) % irb->size;
}
