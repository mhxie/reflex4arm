/*
 * Copyright (c) 2015-2017, Stanford University
 *  
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  * Redistributions of source code must retain the above copyright notice, 
 *    this list of conditions and the following disclaimer.
 * 
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 *  * Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright 2013-16 Board of Trustees of Stanford University
 * Copyright 2013-16 Ecole Polytechnique Federale Lausanne (EPFL)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <ix/cpu.h>
#include <ix/log.h>
#include <ix/stddef.h>
#include <stdlib.h>

typedef struct kstats_distr {
    uint64_t count;
    uint64_t min_occ;
    uint64_t max_occ;
    uint64_t tot_occ;
    uint64_t min_lat;
    uint64_t max_lat;
    uint64_t tot_lat;

} kstats_distr;

typedef struct kstats_accumulate {
    kstats_distr *cur;
    uint64_t start_lat;
    uint64_t start_occ;
    uint64_t accum_time;
} kstats_accumulate;

typedef struct kstats {
#define DEF_KSTATS(_c) kstats_distr _c
#include "kstatvectors.h"
} kstats;

#ifdef ENABLE_KSTATS

RTE_DECLARE_PER_LCORE(kstats, _kstats);
RTE_DECLARE_PER_LCORE(kstats_accumulate, _kstats_accumulate);
RTE_DECLARE_PER_LCORE(int, _kstats_packets);
RTE_DECLARE_PER_LCORE(int, _kstats_batch_histogram[]);
RTE_DECLARE_PER_LCORE(int, _kstats_backlog_histogram[]);

#define KSTATS_BATCH_HISTOGRAM_SIZE 512
#define KSTATS_BACKLOG_HISTOGRAM_SIZE 512

extern void kstats_enter(kstats_distr *n, kstats_accumulate *saved_accu);
extern void kstats_leave(kstats_accumulate *saved_accu);

static inline void kstats_vector(kstats_distr *n) {
    percpu_get(_kstats_accumulate).cur = n;
}

static inline void kstats_packets_inc(int count) {
    percpu_get(_kstats_packets) += count;
}

static inline void kstats_batch_inc(int count) {
    if (count >= KSTATS_BATCH_HISTOGRAM_SIZE)
        panic("kstats batch histogram overflow\n");

    percpu_get(_kstats_batch_histogram)[count]++;
}

static inline void kstats_backlog_inc(int count) {
    if (count >= KSTATS_BACKLOG_HISTOGRAM_SIZE)
        panic("kstats backlog histogram overflow\n");

    percpu_get(_kstats_backlog_histogram)[count]++;
}

#define KSTATS_PUSH(TYPE, _save) \
    kstats_enter(&(percpu_get(_kstats)).TYPE, _save)
#define KSTATS_VECTOR(TYPE) \
    kstats_vector(&(percpu_get(_kstats)).TYPE)
#define KSTATS_POP(_save) \
    kstats_leave(_save)
//#define KSTATS_CURRENT_IS(TYPE)	(percpu_get(_kstats_accumulate).cur==&kstatsCounters.TYPE)
#define KSTATS_PACKETS_INC(_count) \
    kstats_packets_inc(_count)
#define KSTATS_BATCH_INC(_count) \
    kstats_batch_inc(_count)
#define KSTATS_BACKLOG_INC(_count) \
    kstats_backlog_inc(_count)

extern int kstats_init_cpu(void);

#else /* ENABLE_KSTATS */

#define KSTATS_PUSH(TYPE, _save)
#define KSTATS_VECTOR(TYPE)
#define KSTATS_POP(_save)
#define KSTATS_CURRENT_IS(TYPE) 0
#define KSTATS_PACKETS_INC(_count)
#define KSTATS_BATCH_INC(_count)
#define KSTATS_BACKLOG_INC(_count)

#endif /* ENABLE_KSTATS */
