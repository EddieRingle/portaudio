#ifndef PORTAUDIO_H
#define PORTAUDIO_H

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/*
 * $Id$
 * PortAudio Portable Real-Time Audio Library
 * PortAudio API Header File
 * Latest version available at: http://www.audiomulch.com/portaudio/
 *
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
    This is a preliminary version of mult-host-api support. This version
    includes the following changes relative to R18:
     
     
    - the following proposed renamings have been performed:
        paInvalidDeviceId -> paInvalidDevice
        PaDeviceID -> PaDeviceIndex
        Pa_GetDefaultInputDeviceID() -> Pa_GetDefaultInputDevice()
        Pa_GetDefaultOutputDeviceID() -> Pa_GetDefaultOutputDevice()
     
        Pa_StreamActive() -> Pa_IsStreamActive()
        Pa_StreamTime() -> Pa_GetStreamTime()
        Pa_GetCPULoad() -> Pa_GetStreamCpuLoad()
     
    - added the following error codes:
        paNotInitialized,
     
    - renamed PortAudioStream to PaStream. PortAudioStream is still available
    as a #define, but this will be removed eventually.
     
    - added PaHostAPISpecificStreamInfo definition as per proposal
     
    - added paIncompatibleStreamInfo error code
     
    - renamed input/outputDeviceInfo to input/outputStreamInfo
     
    - changed definition of paPlatformSpecificFlags to be a mask
     
    - reformatted comments to doxygen java style
     
    - added paNonInterleaved contant to support non-interleaved sample buffers
        as per existing proposal.

    - changed streamInfo parameters to Pa_OpenStream to from void * to
        PaHostApiSpecificStreamInfo*

    - removed paPackedInt24, paInt24 is now a packed format
*/


typedef int PaError;
typedef enum {
    paNoError = 0,

    paNotInitialized = -10000,
    paHostError,
    paInvalidChannelCount,
    paInvalidSampleRate,
    paInvalidDevice,
    paInvalidFlag,
    paSampleFormatNotSupported,
    paBadIODeviceCombination,
    paInsufficientMemory,
    paBufferTooBig,
    paBufferTooSmall,
    paNullCallback,
    paBadStreamPtr,
    paTimedOut,
    paInternalError,
    paDeviceUnavailable,
    paIncompatibleStreamInfo,
    paStreamIsStopped,
    paStreamIsNotStopped
} PaErrorNum;
/**< Error codes returned by PortAudio functions. */


PaError Pa_Initialize( void );
/**< Library initialization function - call this before using the PortAudio.
*/


PaError Pa_Terminate( void );
/**< Library termination function - call this after using the PortAudio.
*/


long Pa_GetHostError( void );
/**< Retrieve a host specific error code.
 Can be called after receiving a PortAudio error number of paHostError.
*/


const char *Pa_GetErrorText( PaError errnum );
/**< Translate the supplied PortAudio error number into a human readable
 message.
*/


typedef int PaDeviceIndex;
/**< The type used to refer to audio devices. Values of this type usually
 range from 0 to (Pa_DeviceCount-1), and may also take on the PaNoDevice
 and paUseHostApiSpecificDeviceSpecification values.
     
 @see Pa_DeviceCount, paNoDevice, paUseHostApiSpecificDeviceSpecification
*/


#define paNoDevice (-1)
/**< A special PaDeviceIndex value indicating that no device is available,
 or should be used.

 @see PaDeviceIndex
*/


#define paUseHostApiSpecificDeviceSpecification (-2)
/**< A special PaDeviceIndex value indicating that the device(s) to be used
 are specified in the host api specific stream info structure.

 @see PaDeviceIndex
*/

/* Host API enumeration mechanism */


typedef int PaHostApiIndex;
/**< The type used to enumerate to host APIs at runtime. Values of this type
 range from 0 to (Pa_CountHostApis()-1).
     
 @see Pa_CountHostApis
*/


typedef enum {
    paInDevelopment=0, /* use while developing support for a new host API */
    paWin32DirectSound=1,
    paWin32MME=2,
    paWin32ASIO=3,
    paMacOSSoundManager=4,
    paMacOSCoreAudio=5,
    paMacOSASIO=6,
    paOSS=7,
    paALSA=8,
    paIRIXAL=9,
    paBeOS=10
} PaHostApiTypeId;
/**< Unchanging unique identifiers for each supported host API. This type
    is used in the PaHostApiInfo structure. The values are guaranteed to be
    unique and to never change, thus allowing code to be written that
    conditionally uses host API specific extensions.
     
    New type ids will be allocated when support for a host API reaches
    "public alpha" status, prior to that developers should use the
    paInDevelopment type id.
     
    @see PaHostApiInfo
*/


PaHostApiIndex Pa_HostApiTypeIdToHostApiIndex( PaHostApiTypeId type );
/**< Convert a static host API unique identifier, into a runtime
 host API index.

 @param type A unique host API identifier belonging to the PaHostApiTypeId
 enumeration.

 @return A valid PaHostApiIndex ranging from 0 to (Pa_CountHostApis()-1), or
 -1 if the host API specified by the type parameter is not available.
 
 @see PaHostApiTypeId
*/


PaHostApiIndex Pa_CountHostApis( void );
/**< Retrieve the number of available host APIs. Even if a host API is
 available it may have no devices available.
 
 @return The number of available host APIs. May return 0 if PortAudio is
 not initialized or an error has occured.
     
 @see PaHostApiIndex
*/


PaHostApiIndex Pa_GetDefaultHostApi( void );
/**< Retrieve the index of the defualt hostAPI. The default host API will be
 the lowest common denominator host API on the current platform and is
 unlikely to provide the best performance.
     
 @return The default host API index.
*/


typedef struct
{
    int structVersion;
    PaHostApiTypeId type; /**< the well known unique identifier of this host API @see PaHostApiTypeId*/
    const char *name; /* a textual description of the host API for display on user interfaces */
}
PaHostApiInfo;
/**< A structure containing information about a particular host API. */


const PaHostApiInfo * Pa_GetHostApiInfo( PaHostApiIndex hostApi );
/**< Retrieve a pointer to a structure containing information about a specific
 host Api.
     
 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)
     
 @return A pointer to an immutable PaHostApiInfo structure describing
 a specific host API. If the hostApi parameter is out of range or an error
 is encountered, the function returns NULL.
     
 The returned structure is owned by the PortAudio implementation and must not
 be manipulated or freed. The pointer is only guaranteed to be valid between
 calls to Pa_Initialize() and Pa_Terminate().
*/


PaDeviceIndex Pa_HostApiDefaultInputDevice( PaHostApiIndex hostApi );
/**< Retrieve the default input device for the specified host API
     
 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)
     
 @return A device index ranging from 0 to (Pa_CountDevices()-1), or paNoDevice
 if there is no default input device available for the specified host API.
*/


PaDeviceIndex Pa_HostApiDefaultOutputDevice( PaHostApiIndex hostApi );
/**< Retrieve the default output device for the specified host API

 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)

 @return A device index ranging from 0 to (Pa_CountDevices()-1), or paNoDevice
 if there is no default output device available for the specified host API.
*/


int Pa_HostApiCountDevices( PaHostApiIndex hostApi );
/**< Retrieve the number of devices belonging to a specific host API.
 This function may be used in conjunction with Pa_HostApiDeviceIndexToDeviceIndex()
 to enumerate all devices for a specific host API.
 
 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)
     
 @return The number of devices belonging to the specified host API.
     
 @see Pa_HostApiDeviceIndexToDeviceIndex
*/


PaDeviceIndex Pa_HostApiDeviceIndexToDeviceIndex( PaHostApiIndex hostApi,
        int hostApiDeviceIndex );
/**< Convert a host-API-specific device index to standard PortAudio device index.
 This function may be used in conjunction with Pa_HostApiCountDevices() to
 enumerate all devices for a specific host API.

 @param hostApi A valid host API index ranging from 0 to (Pa_CountHostApis()-1)

 @param hostApiDeviceIndex A valid per-host device index in the range
 0 to (Pa_HostApiCountDevices(hostApi)-1)

 @see Pa_HostApiCountDevices
*/



/* Device enumeration and capabilities */


PaDeviceIndex Pa_CountDevices( void );
/**< Retrieve the number of available devices.
 @return The number of available devices. May return 0 if PortAudio is
 not initialized or an error has occured.
*/


PaDeviceIndex Pa_GetDefaultInputDevice( void );
/**< Retrieve the index of the default input device. The result can be
 used in the inputDevice parameter to Pa_OpenStream().
     
 @return The default input device index for the defualt host API, or paNoDevice
 if not input device is available.
*/


PaDeviceIndex Pa_GetDefaultOutputDevice( void );
/**< Retrieve the index of the default output device. The result can be
 used in the outputDevice parameter to Pa_OpenStream().
     
 @return The default output device index for the defualt host API, or paNoDevice
 if not output device is available.

 @note 
 On the PC, the user can specify a default device by
 setting an environment variable. For example, to use device #1.
<pre>
 set PA_RECOMMENDED_OUTPUT_DEVICE=1
</pre>
 The user should first determine the available device ids by using
 the supplied application "pa_devs".
*/


typedef unsigned long PaSampleFormat;
/**< A type used to specify one or more sample formats. They indicate
 the formats used to pass sound data between the callback and the
 stream. Each device has one or more "native" formats which may be used when
 optimum efficiency or control over conversion is required.
     
 Formats marked "always available" are supported (emulated) by all
 PortAudio implementations.
     
 The floating point representation (paFloat32) uses +1.0 and -1.0 as the
 maximum and minimum respectively.
     
 paUInt8 is an unsigned 8 bit format where 128 is considered "ground"
     
 The paNonInterleaved flag indicates that a multichannel buffer is passed
 as a set of non-interleaved pointers.
     
 @see Pa_OpenStream, Pa_OpenDefaultStream, PaDeviceInfo
 @see paFloat32, paInt16, paInt32, paInt24, paInt8
 @see paUInt8, paCustomFormat, paNonInterleaved
*/

#define paFloat32      ((PaSampleFormat) (1<<0)) /**< @see PaSampleFormat */
#define paInt32        ((PaSampleFormat) (1<<1)) /**< @see PaSampleFormat */
#define paInt24        ((PaSampleFormat) (1<<2)) /**< Packed 24 bit format. @see PaSampleFormat */
#define paInt16        ((PaSampleFormat) (1<<3)) /**< @see PaSampleFormat */
#define paInt8         ((PaSampleFormat) (1<<4)) /**< @see PaSampleFormat */
#define paUInt8        ((PaSampleFormat) (1<<5)) /**< @see PaSampleFormat */
#define paCustomFormat ((PaSampleFormat) (1<<16))/**< @see PaSampleFormat */

#define paNonInterleaved ((PaSampleFormat) (1<<31))

typedef struct
{
    int structVersion;  /* this is struct version 2 */
    const char *name;
    PaHostApiIndex hostApi; /* note this is a host API index, not a type id*/
    int maxInputChannels;
    int maxOutputChannels;

    /* THE FOLLOWING FIELDS WILL BE REMOVED in favour of IsSupported() */

    /* Number of discrete rates, or -1 if range supported. */
    int numSampleRates;
    /* Array of supported sample rates, or {min,max} if range supported. */
    const double *sampleRates;
    PaSampleFormat nativeSampleFormats;
}
PaDeviceInfo;
/**< A structure providing information and capabilities of PortAudio devices.
 Devices may support input, output or both input and output.
*/


const PaDeviceInfo* Pa_GetDeviceInfo( PaDeviceIndex device );
/**< Retrieve a pointer to a PaDeviceInfo structure containing information
 about the specified device.
 @return A pointer to an immutable PaDeviceInfo structure. If the device
 parameter is out of range the function returns NULL.
     
 @param device A valid device index in the range 0 to (Pa_CountDevices()-1)
     
 @note PortAudio manages the memory referenced by the returned pointer,
 the client must not manipulate or free the memory. The pointer is only
 guaranteed to be valid between calls to Pa_Initialize() and Pa_Terminate().
     
 @see PaDeviceInfo, PaDeviceIndex
*/


/* Streaming types and functions */


typedef void PaStream;
/**<
 A single PaStream can provide multiple channels of real-time
 streaming audio input and output to a client application.
 Pointers to PaStream objects are passed between PortAudio functions that
 operate on streams.
     
 @see Pa_OpenStream, Pa_OpenDefaultStream, Pa_OpenDefaultStream, Pa_CloseStream,
 Pa_StartStream, Pa_StopStream, Pa_AbortStream, Pa_IsStreamActive,
 Pa_GetStreamTime, Pa_GetStreamCpuLoad
     
*/

#define PortAudioStream PaStream;
/**< For backwards compatibility only. */


typedef unsigned long PaStreamFlags;
/**< Flags used to control the behavior of a stream. They are passed as
 parameters to Pa_OpenStream or Pa_OpenDefaultStream. Multiple flags may be
 ORed together.
     
 @see Pa_OpenStream, Pa_OpenDefaultStream
 @see paNoFlag, paClipOff, paDitherOff, paPlatformSpecificFlags
*/
#define   paNoFlag      (0)      /**< @see PaStreamFlags */
#define   paClipOff     (1<<0)   /**< Disable default clipping of out of range samples. @see PaStreamFlags */
#define   paDitherOff   (1<<1)   /**< Disable default dithering. @see PaStreamFlags */

#define   paPlatformSpecificFlags (0xFFFF0000) /**< A mask specifying the platform specific bits. @see PaStreamFlags */


typedef double PaTimestamp;
/**< The type used to represent a continuous sample clock with arbitrary
 start time that can be used for syncronization. The type is used for the
 outTime argument to the PortAudioCallback and as the result of
 Pa_GetStreamTime().
     
 @see PortAudioCallback, Pa_GetStreamTime
*/


typedef struct PaHostApiSpecificStreamInfo
{
    unsigned long size;    /**< size of whole structure including this header */
    PaHostApiTypeId hostApiType; /**< host API for which this data is intended */
    unsigned long version; /**< structure version */
}
PaHostApiSpecificStreamInfo;
/**< The common header of data structures passed to the inputStreamInfo and
 outputStreamInfo parameters of Pa_OpenStream().
     
 @see Pa_OpenStream
*/


typedef enum
{
    paContinue=0,
    paComplete=1,
    paAbort=2
}
PaCallbackResult;
/**<
 Possible return values for the PortAudioCallback.
*/


typedef int PortAudioCallback(
    void *input, void *output,
    unsigned long frameCount,
    PaTimestamp outTime, void *userData );
/**< FIXME: rename PaStreamCallback
 Functions of type PortAudioCallback are implemented by PortAudio clients.
 They consume, process or generate audio in response to requests from an
 active PortAudio stream.
     
 @param input and @param output are arrays of interleaved samples,
 the format, packing and number of channels used by the buffers are
 determined by parameters to Pa_OpenStream().
     
 @param frameCount The number of sample frames to be processed by
 the callback.
     
 @param outTime The time in samples when the buffer(s) processed by
 this callback will begin being played at the audio output.
 See also Pa_GetStreamTime()
     
 @param userData The value of a user supplied pointer passed to
 Pa_OpenStream() intended for storing synthesis data etc.
     
 @return
 FIXME: document PaCallbackResult here
 The callback can return a non-zero value to stop the stream. This may be
 useful in applications such as soundfile players where a specific duration
 of output is required. However, it is not necessary to utilise this mechanism
 as Pa_StopStream(), Pa_AbortStream() or Pa_CloseStream() can also be used to
 terminate the stream. A callback returning a non-zero value must fill the
 entire outputBuffer.
     
 @see Pa_OpenStream, Pa_OpenDefaultStream
     
 @note None of the other stream functions may be called from within the
 callback function except for Pa_StreamCPULoad().
*/


PaError Pa_OpenStream( PaStream** stream,
                       PaDeviceIndex inputDevice,
                       int numInputChannels,
                       PaSampleFormat inputSampleFormat,
                       unsigned long inputLatency,
                       PaHostApiSpecificStreamInfo *inputStreamInfo,
                       PaDeviceIndex outputDevice,
                       int numOutputChannels,
                       PaSampleFormat outputSampleFormat,
                       unsigned long outputLatency,
                       PaHostApiSpecificStreamInfo *outputStreamInfo,
                       double sampleRate,
                       unsigned long framesPerCallback,
                       PaStreamFlags streamFlags,
                       PortAudioCallback *callback,
                       void *userData );
/**< Opens a stream for either input, output or both.
     
 @param stream The address of a PaStream pointer which will receive
 a pointer to the newly opened stream.
     
 @param inputDevice A valid device index in the range 0 to (Pa_CountDevices()-1)
 specifying the device to be used for input. May be paNoDevice to indicate that
 an input device is not required.
     
 @param numInputChannels The number of channels of sound to be delivered to the
 callback. It can range from 1 to the value of maxInputChannels in the
 PaDeviceInfo record for the device specified by the inputDevice parameter.
 If inputDevice is paNoDevice numInputChannels is ignored.
     
 @param inputSampleFormat The sample format of inputBuffer provided to the callback
 function. inputSampleFormat may be any of the formats described by the
 PaSampleFormat enumeration (see above). PortAudio guarantees support for
 the device's native formats (nativeSampleFormats in the device info record)
 and additionally 16 and 32 bit integer and 32 bit floating point formats.
 Support for other formats is implementation defined.
     
 @param inputStreamInfo A pointer to an optional host api specific data structure
 containing additional information for device setup or stream processing.
 inputStreamInfo is never required for correct operation. If not used
 inputStreamInfo should be NULL. If inputStreamInfo is supplied, it's
 size and hostApi fields must be compatible with the input devices host api.

 @param inputLatency The desired number of frames of input latency. A value of
 zero indicates that the default or known reliable latency value should be used.

 @param outputDevice A valid device index in the range 0 to (Pa_CountDevices()-1)
 specifying the device to be used for output. May be paNoDevice to indicate that
 an output device is not required.
     
 @param numOutputChannels The number of channels of sound to be supplied by the
 callback. See the definition of numInputChannels above for more details.
     
 @param outputSampleFormat The sample format of the outputBuffer filled by the
 callback function. See the definition of inputSampleFormat above for more
 details.

 @param outputLatency The desired number of frames of output latency. A value of
 zero indicates that the default or known reliable latency value should be used

 @param outputStreamInfo A pointer to an optional host api specific data structure
 containing additional information for device setup or stream processing.
 outputStreamInfo is never required for correct operation. If not used
 outputStreamInfo should be NULL. If outputStreamInfo is supplied, it's
 size and hostApi fields must be compatible with the input devices host api.
     
 @param sampleRate The desired sampleRate. For full-duplex streams it is the
 sample rate for both input and output
     
 @param framesPerCallback The number of frames passed to the callback function.
 When this parameter is 0 it indicates that the callback will recieve an
 optimal number of frames for the requested latency settings.
     
 @param streamFlags Flags which modify the behaviour of the streaming process.
 This parameter may contain a combination of flags ORed together. Some flags may
 only be relevant to certain buffer formats.
     
 @param callback A pointer to a client supplied function that is responsible
 for processing and filling input and output buffers.
     
 @param userData A client supplied pointer which is passed to the callback
 function. It could for example, contain a pointer to instance data necessary
 for processing the audio buffers.
     
 @return
 Upon success Pa_OpenStream() returns paNoError and places a pointer to a
 valid PaStream in the stream argument. The stream is inactive (stopped).
 If a call to Pa_OpenStream() fails, a non-zero error code is returned (see
 PaError for possible error codes) and the value of stream is invalid.
     
 @see PortAudioCallback
*/


PaError Pa_OpenDefaultStream( PaStream** stream,
                              int numInputChannels,
                              int numOutputChannels,
                              PaSampleFormat sampleFormat,
                              double sampleRate,
                              unsigned long framesPerCallback,
                              PortAudioCallback *callback,
                              void *userData );
/**< A simplified version of Pa_OpenStream() that opens the default input
 and/or output devices. Most parameters have identical meaning
 to their Pa_OpenStream() counterparts, with the following exceptions:
     
 If either numInputChannels or numOutputChannels is 0 the respective device
 is not opened. This has the same effect as passing paNoDevice in the device
 arguments to Pa_OpenStream().
     
 sampleFormat applies to both the input and output buffers.
     
*/


PaError Pa_CloseStream( PaStream *stream );
/**< Closes an audio stream. If the audio stream is active it
 discards any pending buffers as if Pa_AbortStream() had been called.
     
*/


PaError Pa_StartStream( PaStream *stream );
/**< Commences audio processing.
*/


PaError Pa_StopStream( PaStream *stream );
/**< Terminates audio processing. It waits until all pending
 audio buffers have been played before it returns.
*/


PaError Pa_AbortStream( PaStream *stream );
/**< Terminates audio processing immediately without waiting for pending
 buffers to complete.
*/


PaError Pa_IsStreamStopped( PaStream *stream );
/**< @return Returns one (1) when the stream is stopped, zero (0) when
    the stream is running, or a negative error number if the stream
    is invalid.
    FIXME: update this to reflect new state machine
*/


PaError Pa_IsStreamActive( PaStream *stream );
/**< @return Returns one (1) when the stream is active (ie playing
 or recording audio), zero (0) when not playing, or a negative error number
 if the stream is invalid.

 A stream is active after a successful call to Pa_StartStream(), until it
 becomes inactive either as a result of a call to Pa_StopStream() or
 Pa_AbortStream(), or as a result of a non-zero return value from the
 user callback. In the latter case, the stream is considered inactive after
 the last buffer has finished playing.

 FIXME: update this to reflect new state machine
 
 @see Pa_StopStream, Pa_AbortStream
*/


PaTimestamp Pa_GetStreamTime( PaStream *stream );
/**< @return Retrieve the current output time in samples for the stream.
 This time may be used as a time reference (for example synchronizing audio to
 MIDI).
*/


double Pa_GetStreamCpuLoad( PaStream* stream );
/**< Retrieve CPU usage information for the specified stream.
 The "CPU Load" is a fraction of total CPU time consumed by the stream's
 audio processing routines including, but not limited to the client supplied
 callback.
     
 This function may be called from the callback function or the application.
     
 @return
 A floating point value, typically between 0.0 and 1.0, where 1.0 indicates
 that the callback is consuming the maximum number of CPU cycles possible to
 maintain real-time operation. A value of 0.5 would imply that PortAudio and
 the sound generating callback was consuming roughly 50% of the available CPU
 time. The return value may exceed 1.0.
*/


PaError Pa_ReadStream( PaStream* stream,
                       void *buffer,
                       unsigned long frames );
/**<
 FIXME: write documentation here
 @see http://www.portaudio.com/docs/proposals.html#Blocking
*/


PaError Pa_WriteStream( PaStream* stream,
                        void *buffer,
                        unsigned long frames );
/**<
 FIXME: write documentation here
 @see http://www.portaudio.com/docs/proposals.html#Blocking
*/


unsigned long Pa_GetStreamReadAvailable( PaStream* stream );
/**<
 FIXME: write documentation here
 @see http://www.portaudio.com/docs/proposals.html#Blocking
*/


unsigned long Pa_GetStreamWriteAvailable( PaStream* stream );
/**<
 FIXME: write documentation here
 @see http://www.portaudio.com/docs/proposals.html#Blocking
*/


/*
FIXME: remove this function?
     
int Pa_GetMinNumBuffers( int framesPerBuffer, double sampleRate );
*/
/**<
 @return The minimum number of buffers required by the current host based on minimum latency.
     
 On the PC, for the DirectSound implementation, latency can be optionally set
 by user by setting an environment variable.
 For example, to set latency to 200 msec, put:
 <pre>
    set PA_MIN_LATENCY_MSEC=200
 </pre>
 in the AUTOEXEC.BAT file and reboot.
 If the environment variable is not set, then the latency will be determined
 based on the OS. Windows NT has higher latency than Win95.
*/


/* Miscellaneous utilities */


PaError Pa_GetSampleSize( PaSampleFormat format );
/**<
 @return The size in bytes of a single sample in the specified format,
 or paSampleFormatNotSupported if the format is not supported.
*/


void Pa_Sleep( long msec );
/**< Puts the caller to sleep for at least 'msec' milliseconds.
 It may sleep longer than requested so don't rely on this for accurate
 musical timing.
     
 This function is provided only as a convenience for authors of portable code
 (such as the tests and examples in the PortAudio distribution.)
*/


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PORTAUDIO_H */
