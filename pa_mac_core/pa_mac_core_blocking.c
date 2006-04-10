static PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VVDBUG(("ReadStream()\n"));

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VVDBUG(("WriteStream()\n"));

    /* suppress unused variable warnings */
    (void) buffer;
    (void) frames;
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VVDBUG(("GetStreamReadAvailable()\n"));

    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VVDBUG(("GetStreamWriteAvailable()\n"));

    /* suppress unused variable warnings */
    (void) stream;

    /* IMPLEMENT ME, see portaudio.h for required behavior*/

    return 0;
}

