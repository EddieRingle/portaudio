#ifndef PA_LINUX_ALSA_H
#define PA_LINUX_ALSA_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PaAlsaStreamInfo
{
    unsigned long size;
    PaHostApiTypeId hostApiType;
    unsigned long version;

    const char *deviceString;
}
PaAlsaStreamInfo;

void PaAlsa_InitializeStreamInfo( PaAlsaStreamInfo *info );

void PaAlsa_EnableRealtimeScheduling( PaStream *s, int enable );

void PaAlsa_EnableWatchdog( PaStream *s, int enable );

#ifdef __cplusplus
}
#endif

#endif
