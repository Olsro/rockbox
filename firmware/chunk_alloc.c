/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2023 by William Wilgus
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
/* [chunk_alloc] allows arrays (or any data) to be allocated in smaller chunks */

#include "chunk_alloc.h"
#include "panic.h"

//#define LOGF_ENABLE
#include "logf.h"

#ifndef MAX
#define MAX(a,b)            ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b)            ((a) < (b) ? (a) : (b))
#endif

/*Note on offsets returned
 * variable data will have variable offsets you need to
 * store these as [chunk_alloc] doesn't keep track
 * if you have a fixed size for each alloc you can do
 * offset / sizeof(data) or index * sizeof(data) to convert
 */

struct chunk
{
    int handle; /* data handle of buflib allocated bytes */
    size_t max_start_offset; /* start of last allocation */
};

#define CHUNK_ARRSZ(n) (sizeof(struct chunk) * n)

static struct chunk* get_chunk_array(struct buflib_context *ctx, int handle)
{
    return (struct chunk*)buflib_get_data(ctx, handle);
}

/* shrink or grow chunk allocation
 * chunks greater than max_chunks will be freed
 * new allocs will default to chunk_size
 * previous or current chunk < max_chunks will NOT be changed
 * Returns true on success false on failure
*/
bool chunk_realloc(struct chunk_alloc_header *hdr,
                   size_t chunk_size, size_t max_chunks)
{
    struct buflib_context *ctx = hdr->context;
    struct chunk *new_chunk = NULL;
    struct chunk *old_chunk;
    size_t min_chunk = 1;
    int new_handle = 0;

    if (max_chunks > hdr->count) /* need room for more chunks */
    {
        new_handle = buflib_alloc(ctx, CHUNK_ARRSZ(max_chunks));

        if (new_handle <= 0)
        {
            logf("%s Error OOM %ld chunks", __func__, max_chunks);
            return false;
        }
        new_chunk = get_chunk_array(ctx, new_handle);
        /* ensure all chunks data is zeroed, we depend on it */
        memset(new_chunk, 0, CHUNK_ARRSZ(max_chunks));
    }
    if (hdr->chunk_handle > 0) /* handle existing chunk */
    {
        logf("%s %ld chunks (%ld bytes) => %ld chunks (%ld bytes)", __func__,
         hdr->count, CHUNK_ARRSZ(hdr->count), max_chunks, CHUNK_ARRSZ(max_chunks));

        buflib_pin(ctx, hdr->chunk_handle);
        old_chunk = get_chunk_array(ctx, hdr->chunk_handle);

        if (new_chunk != NULL) /* copy any valid old chunks to new */
        {
            min_chunk = MIN(max_chunks, hdr->current + 1);
            logf("%s copying %ld chunks", __func__, min_chunk);
            memcpy(new_chunk, old_chunk, CHUNK_ARRSZ(min_chunk));
        }
        /* free any chunks that no longer fit */
        for (size_t i = max_chunks; i <= hdr->current; i++)
        {
            logf("%s discarding chunk[%ld]", __func__, i);
            buflib_free(ctx, old_chunk[i].handle);
        }
        buflib_unpin(ctx, hdr->chunk_handle);

        if (max_chunks < hdr->count && max_chunks > 0)
        {
            logf("%s shrink existing chunk array", __func__);
            min_chunk = max_chunks;
            buflib_shrink(ctx, hdr->chunk_handle,
                          old_chunk, CHUNK_ARRSZ(max_chunks));

            new_handle = hdr->chunk_handle;
        }
        else
        {
            logf("%s free existing chunk array", __func__);
            buflib_free(ctx, hdr->chunk_handle); /* free old chunk array */
        }

        hdr->current = min_chunk - 1;
    }
    else
    {
        logf("chunk_alloc_init %ld chunks (%ld bytes)",
                   hdr->count, (hdr->count));
    }

    hdr->chunk_handle = new_handle;
    hdr->chunk_size = chunk_size;
    hdr->count = max_chunks;

    return true;
}

/* frees all allocations */
void chunk_alloc_free(struct chunk_alloc_header *hdr)
{
    logf("%s freeing %ld chunks", __func__, hdr->count);
    chunk_realloc(hdr, 0, 0);
}

/* initialize chunk allocator
 * chunk_size specifies initial size of each chunk
 * a single allocation CAN be larger than this
 * max_chunks * chunk_size is the total expected size of the buffer
 * more data will not be allocated once all chunks used
 * Returns true on success or false on failure
*/
bool chunk_alloc_init(struct chunk_alloc_header *hdr,
                      struct buflib_context *ctx,
                      size_t chunk_size, size_t max_chunks)
{
    /* initialize header */
    hdr->chunk_handle = 0;
    hdr->chunk_bytes_total = 0;
    hdr->chunk_bytes_free = 0;
    hdr->current = 0;
    hdr->context = ctx;
    hdr->count = 0;

    return chunk_realloc(hdr, chunk_size, max_chunks);
}

/* shrink current chunk to size used */
static void finalize(struct chunk_alloc_header *hdr, struct chunk *chunk_array)
{
    /*Note calling functions check if chunk_bytes_free > 0*/
    size_t idx = hdr->current;
    if (idx >= hdr->count)
        return;
    int handle = chunk_array[idx].handle;
    struct buflib_context *ctx = hdr->context;

    hdr->chunk_bytes_total -= hdr->chunk_bytes_free;
    hdr->chunk_bytes_free = 0;

    buflib_shrink(ctx, handle, NULL, hdr->chunk_bytes_total);

    logf("%s shrink hdr idx[%ld] offset[%ld]: new size: %ld",
    __func__, idx, chunk_array[idx].max_start_offset, hdr->chunk_bytes_total);
}

/* shrink current chunk to size used */
void chunk_alloc_finalize(struct chunk_alloc_header *hdr)
{
    if (hdr->chunk_bytes_free > 0)
    {
        struct chunk *chunk = get_chunk_array(hdr->context, hdr->chunk_handle);
        finalize(hdr, chunk);
    }
}

/* allocates from current chunk if size > bytes remaining
 * current chunk shrinks to size used and a new chunk is allocated
 * returns virtual offset on success or CHUNK_ALLOC_INVALID on error
*/
size_t chunk_alloc(struct chunk_alloc_header *hdr, size_t size)
{
    size_t idx = hdr->current;
    int handle = hdr->chunk_handle;
    struct buflib_context *ctx = hdr->context;

    buflib_pin(ctx, handle);
    struct chunk *chunk = get_chunk_array(ctx, handle);

    while (size > 0)
    {
        if (idx >= hdr->count)
        {
            logf("%s Error OOM -- out of chunks", __func__);
            break;
        }
        hdr->current = idx;

        if(chunk[idx].handle <= 0) /* need to make an new allocation */
        {
            size_t new_alloc_size = MAX(size, hdr->chunk_size);

            chunk[idx].handle = buflib_alloc(ctx, new_alloc_size);

            if (chunk[idx].handle <= 0)
            {
                logf("%s Error OOM", __func__);
                goto fail; /* OOM */
            }

            hdr->chunk_bytes_total = new_alloc_size;
            hdr->chunk_bytes_free = new_alloc_size;

            chunk[idx].max_start_offset =
                            (idx > 0 ? (chunk[idx - 1].max_start_offset) : 0);

             logf("%s New alloc hdr idx[%ld] offset[%ld]: available: %ld",
                  __func__, idx, chunk[idx].max_start_offset, new_alloc_size);
        }

        if(size <= hdr->chunk_bytes_free) /* request will fit */
        {
            size_t offset = chunk[idx].max_start_offset;
            chunk[idx].max_start_offset += size;
            hdr->chunk_bytes_free -= size;
            /*logf("%s hdr idx[%ld] offset[%ld] size: %ld",
                                __func__, idx, offset, size);*/

            buflib_unpin(ctx, handle);
            return offset;
        }
        else if (hdr->chunk_bytes_free > 0) /* shrink the current chunk */
        {
            finalize(hdr, chunk);
        }
        idx++;
    }
fail:
    buflib_unpin(ctx, handle);
    return CHUNK_ALLOC_INVALID;
}

/* returns chunk idx given virtual offset */
static size_t chunk_find_data_idx(struct chunk_alloc_header *hdr,
                                  size_t offset, struct chunk **chunk)
{
    size_t idx;
    *chunk = get_chunk_array(hdr->context, hdr->chunk_handle);
    /*logf("%s search for offset[%ld]", __func__, offset);*/
    for (idx = hdr->current; idx < hdr->count; idx--)
    {
        if (offset < (*chunk)[idx].max_start_offset
           && (idx == 0 || offset >= (*chunk)[idx - 1].max_start_offset))
        {
            /*logf("%s found hdr idx[%ld] max offset[%ld]",
                 __func__, idx, (*chunk)[idx].max_start_offset);*/
            return idx;
        }
    }
    panicf("%s Error offset %d does not exist", __func__, (unsigned int)offset);
    return CHUNK_ALLOC_INVALID;
}

/* get data - buffer chunk can't be moved while pinned
 * multiple calls will up pin count so put should be called for each
 * Returns data at offset
*/
void* chunk_get_data(struct chunk_alloc_header *hdr, size_t offset)
{
    struct chunk *chunk;
    size_t idx = chunk_find_data_idx(hdr, offset, &chunk);
    if (idx > 0)
        offset -= chunk[idx - 1].max_start_offset;
    logf("%s adjusted offset: %ld", __func__, offset);
    buflib_pin(hdr->context, chunk[idx].handle);
    return buflib_get_data(hdr->context, chunk[idx].handle) + offset;
}

/* release a pinned buffer, chunk can't be moved till pin count == 0 */
void chunk_put_data(struct chunk_alloc_header *hdr, size_t offset)
{
    struct chunk *chunk;
    size_t idx = chunk_find_data_idx(hdr, offset, &chunk);
    buflib_unpin(hdr->context, chunk[idx].handle);
}
