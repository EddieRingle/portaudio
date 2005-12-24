/*
 * Mac spcific flags for PA.
 * portaudio.h should be included before this file.
 */

/*
 * A pointer to a paMacCoreStreamInfo may be passed as
 * the hostApiSpecificStreamInfo in the PaStreamParameters struct
 * when opening a stream. Use NULL, for the defaults. Note that for
 * duplex streams, both infos should be the same or behaviour
 * is undefined.
 */
typedef struct paMacCoreStreamInfo
{
    unsigned long size;         /**< size of whole structure including this header */
    PaHostApiTypeId hostApiType;/**< host API for which this data is intended */
    unsigned long version;      /**< structure version */
    unsigned long flags;        /* flags to modify behaviour */
} paMacCoreStreamInfo;

/* Use this function to initialize a paMacCoreStreamInfo struct
   using the requested flags. */
void paSetupMacCoreStreamInfo( paMacCoreStreamInfo *data, unsigned long flags )
{
   bzero( data, sizeof( paMacCoreStreamInfo ) );
   data->size = sizeof( paMacCoreStreamInfo );
   data->hostApiType = paCoreAudio;
   data->version = 0x01;
   data->flags = flags;
}

/*
 * Here is the struct that shoul
 */

/*
 * The following flags alter the behaviour of PA on the mac platform.
 * they can be ORed together. These should work both for opening and
 * checking a device.
 */
/* Allows PortAudio to change things like the device's frame size,
 * which allows for much lower latency, but might disrupt the device
 * if other programs are using it. */
const unsigned long paMacCore_ChangeDeviceParameters      = 0x01;

/* In combination with the above flag,
 * causes the stream opening to fail, unless the exact sample rates
 * are supported by the device. */
const unsigned long paMacCore_FailIfConversionRequired    = 0x02;



/*
 * Here are some "preset" combinations of flags (above) to get to some
 * common configurations. THIS IS OVERKILL, but if more flags are added
 * it won't be.
 */
/*This is the default setting: do as much sample rate conversion as possible
 * and as little mucking with the device as possible. */
const unsigned long paMacCorePlayNice = 0x00;
/*This setting is tuned for pro audio apps. It allows SR conversion on input
  and output, but it tries to set the appropriate SR on the device.*/
const unsigned long paMacCorePro      = 0x01;

