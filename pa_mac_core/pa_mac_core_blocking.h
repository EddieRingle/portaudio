
/*
 * Number of miliseconds to busy wait whil waiting for data in blocking calls.
 */
#define PA_MAC_BLIO_BUSY_WAIT_SLEEP_INTERVAL (10)

typedef struct {
    RingBuffer inputRingBuffer;
    RingBuffer outputRingBuffer;
    PaSampleFormat inputSampleFormat;
    size_t inputSampleSize;
    PaSampleFormat outputSampleFormat;
    size_t outputSampleSize;

    size_t framesPerBuffer;

    int inChan;
    int outChan;

    PaStreamCallbackFlags statusFlags;
    PaError errors;

    /* eventually, I'll need condition vars, too, but for now,
     *   I'm using busy-waits.
     */
}
PaMacBlio;

/*
 * This fnuction determines the size of a particular sample format.
 * if the format is not recognized, this returns zero.
 */
static size_t computeSampleSizeFromFormat( PaSampleFormat format )
{
   switch( format ) {
   case paFloat32: return 4;
   case paInt32: return 4; 
   case paInt24: return 3;
   case paInt16: return 2;
   case paInt8: case paUInt8: return 1;
   default: return 0;
   }
}


static PaError initializeBlioRingBuffers(
                                       PaMacBlio *blio,
                                       PaSampleFormat inputSampleFormat,
                                       PaSampleFormat outputSampleFormat,
                                       size_t framesPerBuffer,
                                       long ringBufferSize,
                                       int inChan,
                                       int outChan );
static void destroyBlioRingBuffers( PaMacBlio *blio );
static void resetBlioRingBuffers( PaMacBlio *blio );

static int BlioCallback(
        const void *input, void *output,
        unsigned long frameCount,
        const PaStreamCallbackTimeInfo* timeInfo,
        PaStreamCallbackFlags statusFlags,
        void *userData );

static void waitUntilBlioWriteBufferIsFlushed( PaMacBlio *blio );
