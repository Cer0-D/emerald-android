# dlmalloc 2.8.6

`malloc.c` is Doug Lea's malloc, version 2.8.6 (2012-08-29), unmodified. It is
in the public domain ([CC0 1.0](http://creativecommons.org/publicdomain/zero/1.0/)),
as its header says.

The arm64 build uses it as an `mspace` over a region reserved below 4 GiB
(`android_port/src/android_lowmem.c`): the game keeps addresses in 32 bits,
so the memory it is handed must sit there.

Source: the copy distributed with Boost.Container 1.92.0
(`libs/container/src/dlmalloc_2_8_6.c`), identical to Doug Lea's release.
