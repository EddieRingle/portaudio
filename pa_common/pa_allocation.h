#ifndef PA_ALLOCATION_H
#define PA_ALLOCATION_H
/*
 * Id:
 * Portable Audio I/O Library allocation context header
 * memory allocation context for tracking allocation groups
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
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
 */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/*
An allocation group is useful for keeping track of multiple blocks
of memory which are allocated at the same time (such as during initialization)
and need to be deallocated at the same time. The group maintains a list 
of allocated blocks, and can deallocate them all simultaneously which can
be usefull for cleaning up after a partially initialized object fails.

The PortAudio allocation group mechanism is built on top of the lower
level allocation functions defined in pa_util.h
*/



typedef struct
{
    long linkCount;
    struct PaUtilAllocationGroupLink *linkBlocks;
    struct PaUtilAllocationGroupLink *spareLinks;
    struct PaUtilAllocationGroupLink *allocations;
}PaUtilAllocationGroup;


PaUtilAllocationGroup* PaUtil_CreateAllocationGroup( void );

void PaUtil_DestroyAllocationGroup( PaUtilAllocationGroup* context );
/**< frees the group, but not the memory allocated through the group */

void* PaUtil_GroupAllocateMemory( PaUtilAllocationGroup* context, long size );

void PaUtil_GroupFreeMemory( PaUtilAllocationGroup* context, void *buffer );
/**< calling this is a relatively time consuming operation */

void PaUtil_FreeAllAllocations( PaUtilAllocationGroup* context );
/**< frees all allocations made through the group, doesn't free the group itself */


#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* PA_ALLOCATION_H */
