/*
 * Mac spcific flags for PA.
 */

/*
 * Flags, or'ed together, to alter the behaviour of PA.
 * A pointer to a paMacCoreStreamInfo may be passed as 
 * the hostApiSpecificStreamInfo in the PaStreamParameters struct
 * when opening a stream. Use NULL or a pointer to 0, for the defaults.
 * note that for duplex streams, both infos should be the same or behaviour
 * is undefined.
 */
typedef UInt32 paMacCoreStreamInfo;

/*
 * The following flags alter the behaviour of PA on the mac platform.
 * they can be ORed together. These should work both for opening and
 * checking a device.
 */
/* Allows PortAudio to change things like the device's frame size,
 * which allows for much lower latency, but might disrupt the device
 * if other programs are using it. */
const paMacCoreStreamInfo paMacCore_ChangeDeviceParameters      = 0x01;

/* In combination with the above flag,
 * causes the stream opening to fail, unless the exact sample rates
 * are supported by the device. */
const paMacCoreStreamInfo paMacCore_FailIfConversionRequired    = 0x02;



/*
 * Here are some "preset" combinations of flags (above) to get to some
 * common configurations. THIS IS OVERKILL, but if more flags are added
 * it won't be.
 */
/*This is the default setting: do as much sample rate conversion as possible
 * and as little mucking with the device as possible. */
const paMacCoreStreamInfo paMacCorePlayNice = 0x00;
/*This setting is tuned for pro audio apps. It allows SR conversion on input
  and output, but it tries to set the appropriate SR on the device.*/
const paMacCoreStreamInfo paMacCorePro      = 0x01;
