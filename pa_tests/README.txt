This directory contains various programs to test PortAudio. The files 
named patest_* are tests, the files named padebug_* are just scratch 
files that may or may not work.

The following is a checklist indicating which files are currently up
to date with the V19 API. (x- indicates done, o- indicates not done).
Note that this does not necissarily mean that the tests pass, just that they
compile.
Feel free to fix some more of them, most simply require adjusting to the new API.

o- paqa_devs.c 
o- paqa_errs.c 
o- patest1.c
o- patest_buffer.c
x- patest_callbackstop.c
x- patest_clip.c (last test fails, dither doesn't currently force clip in V19)
o- patest_dither.c
o- patest_hang.c
o- patest_latency.c
x- patest_leftright.c
o- patest_longsine.c
o- patest_many.c
o- patest_maxsines.c
o- patest_multi_sine.c
o- patest_pink.c
x- patest_prime.c
x- patest_read_record.c
x- patest_record.c
x- patest_ringmix.c
o- patest_saw.c
x- patest_sine.c
o- patest_sine8.c
x- patest_sine_formats.c
o- patest_sine_time.c
x- patest_start_stop.c
x- patest_stop.c
x- patest_sync.c
o- patest_toomanysines.c
o- patest_underflow.c
o- patest_wire.c
x- patest_write_sine.c
x- pa_devs.c
x- pa_fuzz.c
o- pa_minlat.c

