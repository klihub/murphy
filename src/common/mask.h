/*
 * Copyright (c) 2014, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MURPHY_MASK_H__
#define __MURPHY_MASK_H__

#include <stdint.h>

#include <murphy/common/macros.h>
#include <murphy/common/mm.h>


MRP_CDECL_BEGIN

#define MRP_MASK_EMPTY { .nbit = 64, .bits = 0 }
#define MRP_MASK(m)    mrp_mask_t m = MRP_MASK_EMPTY

/**
 * a trivial bitmask of arbitrary size
 */

typedef struct {
    int           nbit;                  /* number of bits in this mask */
    union {
        uint64_t  bits;                  /* bits for nbit <= 64 */
        uint64_t *bitp;                  /* bits for nbit >  64 */
    };
} mrp_mask_t;


/** Initialize the given mask. */
static inline void mrp_mask_init(mrp_mask_t *m)
{
    m->nbit = 64;
    m->bits = 0;
}


/** Reset the given mask. */
static inline void mrp_mask_reset(mrp_mask_t *m)
{
    if (m->nbit > 64)
        mrp_free(m->bitp);

    mrp_mask_init(m);
}


/** Set the given bit in the mask. */
static inline int mrp_mask_set(mrp_mask_t *m, int bit)
{
    int w, b, n;

    if (bit > m->nbit - 1) {
        n = bit / 64 + 1;

        if (m->nbit == 64) {
            uint64_t *bitp;

            bitp = mrp_allocz(n * 64);

            if (bitp == NULL)
                return -1;

            bitp[0] = m->bits;
            m->bitp = bitp;
            m->nbit = n * 64;
        }
        else {
            if (!mrp_reallocz(m->bitp, m->nbit / 64, n))
                return -1;
        }
    }

    b = bit & 63;

    if (m->nbit == 64)
        m->bits |= (1 << b);
    else {
        w = bit / 64;
        m->bitp[w] |= (1 << b);
    }

    return 0;
}


/** Clear the given bit in the mask. */
static inline void mrp_mask_clear(mrp_mask_t *m, int bit)
{
    int w, b;

    if (bit > m->nbit - 1)
        return;

    b = bit & 63;

    if (m->nbit == 64)
        m->bits &= ~(1 << b);
    else {
        w = bit / 64;
        m->bitp[w] &= ~(1 << b);
    }
}


/** Test the given bit in the mask. */
static inline int mrp_mask_test(mrp_mask_t *m, int bit)
{
    int w, b;

    if (bit > m->nbit - 1)
        return 0;

    b = bit & 63;

    if (m->nbit == 64)
        return m->bits & (1 << b);
    else {
        w = bit / 64;
        return m->bitp[w] & (1 << b);
    }
}


/** Copy the given mask, overwriting dst. */
static inline int mrp_mask_copy(mrp_mask_t *dst, mrp_mask_t *src)
{
    if (src->nbit == 64) {
        dst->nbit = 64;
        dst->bits = src->bits;
    }
    else {
        dst->nbit = src->nbit;
        dst->bitp = mrp_alloc(dst->nbit * 64);

        if (dst->bitp == NULL)
            return -1;

        memcpy(dst->bitp, src->bitp, dst->nbit * 64);
    }

    return 0;
}

MRP_CDECL_END

#endif /* __MURPHY_MASK_H__ */
