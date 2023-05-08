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

#include <nvme/nvme_sw_table.h>
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

void nvme_sw_table_init(struct nvme_sw_table *t) {
    int i;
    t = rte_malloc(NULL, sizeof(struct nvme_sw_table), 0);

    struct rte_hash_parameters params = {.name = "test",
                                         .entries = NVME_SW_TABLE_SIZE * 8,
                                         .key_len = sizeof(uint32_t),
                                         .hash_func = rte_jhash,
                                         .hash_func_init_val = 0,
                                         .socket_id = rte_socket_id()};

    t->table = rte_hash_create(&params);

    for (i = 0; i < MAX_NVME_FLOW_GROUPS; i++) {
        t->queue_head[i] = 0;
        t->queue_tail[i] = 0;
        t->queue_overflow_count[i] = 0;
        t->total_token_demand[i] = 0;
        t->saved_tokens[i] = 0;
        t->token_credit[i] = 0;
    }
    t->total_request_count = 0;

    if (t->table == NULL) {
        rte_free(t);
        printf("Unable to create hash table: %s\n", rte_strerror(rte_errno));
        return ENOMEM;
    } else {
        printf("Successfully created the hash table!!!\n");
    }
}
int nvme_sw_table_push_back(struct nvme_sw_table *t, long fg_handle,
                            struct nvme_ctx *ctx) {
    // Key Format: seq_number (15 bit) | queue_id (12 bit) | thread_id (5 bit)
    uint32_t key = RTE_PER_LCORE(cpu_nr) + fg_handle
                   << 5 + t->queue_tail[fg_handle] << 17;
    int ret = rte_hash_add_key_data(t->table, (void *)&key, (void *)ctx);
    t->total_token_demand[fg_handle] += ctx->req_cost;

    t->queue_tail[fg_handle]++;
    t->total_request_count++;
    if (unlikely(t->total_request_count >= NVME_SW_TABLE_SIZE)) {
        printf("ERROR: Cannot push more requests into the table\n");
        return RET_NOMEM;
    }
    if (unlikely(t->queue_tail[fg_handle] >= NVME_SW_TABLE_SIZE)) {
        t->queue_tail[fg_handle] = 0;
        t->queue_overflow_count[fg_handle]++;
    }

    return ret;
}
int nvme_sw_table_pop_front(struct nvme_sw_table *t, long fg_handle,
                            struct nvme_ctx **ctx) {
    int ret;
    if (unlikely(nvme_sw_table_isempty(t, fg_handle))) {
        return -1;
    }
    uint32_t key = RTE_PER_LCORE(cpu_nr) + fg_handle
                   << 5 + t->queue_head[fg_handle] << 17;
    ret = rte_hash_lookup_data(t->table, (void *)&key, (void **)ctx);
    if (ret < 0) {
        printf("ERROR: Cannot find the request in the table\n");
        return ret;
    }
    ret = rte_hash_del_key(t->table, (void *)&key);
    if (ret < 0) {
        printf("ERROR: Cannot pop the request from the table\n");
        return ret;
    }
    t->total_token_demand[fg_handle] -= (*ctx)->req_cost;

    t->queue_head[fg_handle]++;
    t->total_request_count--;
    if (unlikely(t->queue_head[fg_handle] >= NVME_SW_TABLE_SIZE)) {
        t->queue_head[fg_handle] = 0;
    }
    return 0;
}
int nvme_sw_table_isempty(struct nvme_sw_table *t, long fg_handle) {
    return t->queue_head[fg_handle] == t->queue_tail[fg_handle];
}
uint16_t nvme_sw_table_count(struct nvme_sw_table *t, long fg_handle) {
    if (t->queue_head[fg_handle] <= t->queue_tail[fg_handle]) {
        return t->queue_tail[fg_handle] - t->queue_head[fg_handle];
    } else {
        return NVME_SW_TABLE_SIZE - t->queue_head[fg_handle] +
               t->queue_tail[fg_handle];
    }
}

int nvme_sw_table_peak_head_cost(struct nvme_sw_table *t, long fg_handle) {
    if (unlikely(nvme_sw_table_isempty(t, fg_handle))) {
        return -1;
    }

    struct nvme_ctx *ctx;
    uint32_t key = RTE_PER_LCORE(cpu_nr) + fg_handle
                   << 5 + t->queue_head[fg_handle] << 17;
    int ret = rte_hash_lookup_data(t->table, (void *)&key, (void **)&ctx);
    if (ret < 0) {
        printf("ERROR: Cannot find the request in the table\n");
        return ret;
    }
    return ctx->req_cost;
}

unsigned long nvme_sw_table_save_tokens(struct nvme_sw_table *t, long fg_handle,
                                        unsigned long tokens) {
    // only save tokens up to how much have demand, return the rest
    if (t->total_token_demand[fg_handle] == 0) {
        t->saved_tokens[fg_handle] = 0;
        return 0;
    }
    if (t->total_token_demand[fg_handle] > tokens) {
        t->saved_tokens[fg_handle] += tokens;
        return tokens;
    }
    if (t->total_token_demand[fg_handle] <= tokens) {
        t->saved_tokens[fg_handle] += t->total_token_demand[fg_handle];
        return t->total_token_demand[fg_handle];
    }
    return 0;
}

unsigned long nvme_sw_table_take_saved_tokens(struct nvme_sw_table *t,
                                              long fg_handle) {
    unsigned long saved_tokens = t->saved_tokens[fg_handle];
    t->saved_tokens[fg_handle] = 0;
    return saved_tokens;
}
