This directory contains various programs to test PortAudio. The files 
named patest_* are tests, the files named padebug_* are just scratch 
files that may or may not work.

The following is a checklist indicating which files are currently up
to date with the V19 API. (x- indicates done, o- indicates not done).
Note that this does not necissarily mean that the tests pass, just that they
compile.
Feel free to fix some more of them, most simply require adjusting to the new API.

x- paqa_devs.c 
x- paqa_errs.c (needs reviewing)
x- patest1.c
x- patest_buffer.c
x- patest_callbackstop.c
x- patest_clip.c (last test fails, dither doesn't currently force clip in V19)
o- patest_dither.c
x- patest_hang.c
x- patest_latency.c
x- patest_leftright.c
x- patest_longsine.c
x- patest_many.c
x- patest_maxsines.c
x- patest_multi_sine.c
x- patest_pink.c
x- patest_prime.c
x- patest_read_record.c
x- patest_record.c
x- patest_ringmix.c
x- patest_saw.c
x- patest_sine.c
x- patest_sine8.c
x- patest_sine_formats.c
x- patest_sine_time.c
x- patest_start_stop.c
x- patest_stop.c
x- patest_sync.c
x- patest_toomanysines.c
x- patest_underflow.c
x- patest_wire.c
x- patest_write_sine.c
x- pa_devs.c
x- pa_fuzz.c
x- pa_minlat.c
