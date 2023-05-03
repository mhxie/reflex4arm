/*
 * Copyright (c) 2019-2023, UC Santa Cruz
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
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <nvme/nvmedev.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#define NVME_SW_QUEUE_SIZE 4096 * 512

struct nvme_sw_table {
    uint32_t queue_head[MAX_NVME_FLOW_GROUPS];
    uint32_t queue_tail[MAX_NVME_FLOW_GROUPS];
    bool queue_overflow[MAX_NVME_FLOW_GROUPS];
    uint16_t queue_overflow_count[MAX_NVME_FLOW_GROUPS];
    uint32_t total_token_demand[MAX_NVME_FLOW_GROUPS];
    uint32_t saved_tokens[MAX_NVME_FLOW_GROUPS];
    int32_t token_credit[MAX_NVME_FLOW_GROUPS];
    struct rte_hash *table;
    float smoothy_share;  // not in use as of now
};

void nvme_sw_table_init(struct nvme_sw_table *t);
int nvme_sw_table_add(struct nvme_sw_table *t, long fg_handle,
                      struct nvme_ctx *ctx);
int nvme_sw_table_pop_front(struct nvme_sw_table *t, long fg_handle,
                            struct nvme_ctx **ctx);
inline int nvme_sw_table_isempty(struct nvme_sw_table *t, long fg_handle);

int nvme_sw_table_peak_head_cost(struct nvme_sw_table *t, long fg_handle);
unsigned long nvme_sw_table_save_tokens(struct nvme_sw_table *t, long fg_handle,
                                        unsigned long tokens);
unsigned long nvme_sw_table_take_saved_tokens(struct nvme_sw_table *t,
                                              long fg_handle);

double nvme_sw_table_save_tokens_fraction(struct nvme_sw_table *t,
                                          long fg_handle, double tokens);
double nvme_sw_table_take_saved_tokens_fraction(struct nvme_sw_table *t,
                                                long fg_handle);