#ifndef ANDROID_LOWMEM_H
#define ANDROID_LOWMEM_H

/*
 * Memory below 4 GiB for the arm64 build (android_lowmem.c).
 *
 * The game keeps addresses in 32 bits: its assembled data (`.int symbol`),
 * u32 members and variables, two halfwords of a task's data. The arm64 build
 * keeps everything it can be handed below 4 GiB, where such an address is the
 * address - below 2 GiB in fact, where rebuilding one from two signed
 * halfwords ((s16)hi << 16) cannot give it a sign: the library itself (loaded
 * there by android_port/boot), the heap
 * (malloc and the rest are wrapped to an arena here; android.mk) and the stacks
 * of the threads that run game code. On a 32-bit build all memory is there
 * already and these are the plain calls.
 */
#include <stdbool.h>
#include <stddef.h>

/* Address space below 4 GiB, PROT_NONE or readable and writable; NULL if none
 * is free. Released with AndroidLow_Release. */
void *AndroidLow_Reserve(size_t size, bool writable);
void AndroidLow_Release(void *base, size_t size);

/* A thread stack below 4 GiB with a guard page under it; NULL on failure. */
void *AndroidLow_Stack(size_t size);
void AndroidLow_FreeStack(void *stack, size_t size);

/* Whether the heap arena holds this pointer (the logs' check). */
bool AndroidLow_Owns(const void *p);
/* The arena's size and what is in use, for the log. */
void AndroidLow_Stats(size_t *arena, size_t *used);

#endif
