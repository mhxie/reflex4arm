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

#include <ix/atomic.h>
#include <ix/cfg.h>
#include <ix/errno.h>
#include <ix/kstats.h>
#include <ix/log.h>
#include <ix/mempool.h>
#include <ix/syscall.h>
#include <limits.h>
#include <math.h>
#include <nvme/nvme_sw_table.h>
#include <nvme/nvme_tenant_mgmt.h>
#include <nvme/nvmedev.h>
#include <rte_per_lcore.h>
#include <rte_timer.h>
#include <spdk/nvme.h>
#include <sys/socket.h>

// extern from ix/cfg.h
int NVME_READ_COST;
int NVME_WRITE_COST;

// extern from nvmedev.h
DEFINE_BITMAP(g_ioq_bitmap, MAX_NUM_IO_QUEUES);
DEFINE_BITMAP(g_nvme_fgs_bitmap, MAX_NVME_FLOW_GROUPS);

// #define NO_SCHED
#define CYCLIC_LIST
#define SINGLE_THREADED

static struct spdk_nvme_ctrlr *nvme_ctrlr[CFG_MAX_NVMEDEV] = {NULL};
static long global_ns_id = 1;
static long global_ns_size = 1;
static long global_ns_sector_size = 1;
static long active_nvme_devices = 0;
static unsigned int cpu2ssd[CFG_MAX_NVMEDEV];
static int g_max_outstanding_requests = 512;
static long g_outstanding_requests = 0;
// struct pci_dev *g_nvme_dev[CFG_MAX_NVMEDEV];

#define MAX_OPEN_BATCH 32
#define NUM_NVME_REQUESTS (4096 * 256)  // 4096 * 64 //1024
#define SGL_PAGE_SIZE \
    4096  // should match PAGE_SIZE defined in dp/core/reflex_server.c
#define DEFAULT_IO_QUEUE_SIZE 256

RTE_DEFINE_PER_LCORE(int, open_ev[MAX_OPEN_BATCH]);
RTE_DEFINE_PER_LCORE(int, open_ev_ptr);
RTE_DEFINE_PER_LCORE(struct spdk_nvme_qpair *, qpair);
RTE_DEFINE_PER_LCORE(bool, mempool_initialized);

static DEFINE_SPINLOCK(nvme_bitmap_lock);

static struct mempool_datastore request_datastore;
static struct mempool_datastore ctx_datastore;

static struct nvme_flow_group g_nvme_fgs[MAX_NVME_FLOW_GROUPS];
static struct nvme_sw_table *g_nvme_sw_table;
static unsigned long global_token_rate =
    UINT_MAX;  // max token rate device can handle for current strictest latency
               // SLO
static atomic_u64_t global_leftover_tokens =
    ATOMIC_INIT(0);  // shared token bucket
static unsigned long global_LC_sum_token_rate =
    0;  // LC tenant token reservation summed across all LC tenants globally
static unsigned long global_num_best_effort_tenants =
    0;  // total num of best effort tenants
static unsigned long global_num_lc_tenants =
    0;  // total num of latency critical tenants
static atomic_t global_be_token_rate_per_tenant =
    ATOMIC_INIT(0);  // token rate per best effort tenant
static unsigned long global_lc_boost_no_BE =
    0;  // fair share of leftover tokens that LC tenant can use when no BE
        // registered

#define MAX_NUM_THREADS 24
static int scheduled_bit_vector[MAX_NUM_THREADS];

#define TOKEN_FRAC_GIVEAWAY 0.9
static long TOKEN_DEFICIT_LIMIT = 10000;
static int WRITE_BURST_COUNT = 10;
static bool global_readonly_flag = true;

#define SLO_REQ_SIZE 4096
#define QSTATS_INTERVAL rte_get_timer_hz()  // 1 second

RTE_DEFINE_PER_LCORE(struct mempool,
                     request_mempool __attribute__((aligned(64))));
RTE_DEFINE_PER_LCORE(struct mempool, ctx_mempool __attribute__((aligned(64))));
RTE_DEFINE_PER_LCORE(int, received_nvme_completions);

RTE_DEFINE_PER_LCORE(struct less_tenant_mgmt, tenant_manager);
static RTE_DEFINE_PER_LCORE(struct rte_timer, _qstats_timer);

RTE_DEFINE_PER_LCORE(unsigned long, last_sched_time);
RTE_DEFINE_PER_LCORE(unsigned long, last_sched_time_be);
RTE_DEFINE_PER_LCORE(unsigned long, local_extra_demand);
RTE_DEFINE_PER_LCORE(unsigned long, local_leftover_tokens);
RTE_DEFINE_PER_LCORE(int, lc_roundrobin_start);
RTE_DEFINE_PER_LCORE(int, roundrobin_start);
RTE_DEFINE_PER_LCORE(unsigned long, min_scaled_IOPS);
RTE_DEFINE_PER_LCORE(unsigned int, min_tenant_count);

static int nvme_compute_req_cost(int req_type, size_t req_len);

static void set_token_deficit_limit(void);

struct nvme_ctx *alloc_local_nvme_ctx(void) {
    return mempool_alloc(&percpu_get(ctx_mempool));
}

extern void free_local_nvme_ctx(struct nvme_ctx *req) {
    mempool_free(&percpu_get(ctx_mempool), req);
}

void print_queue_status() {
    struct less_tenant_mgmt *thread_tenant_manager =
        &percpu_get(tenant_manager);
    long fg_handle;
    struct nvme_flow_group *nvme_fg;

    printf(
        "There are still %ld requests pending in the table, %d requests in the "
        "SSD queue. See the snapshot below:\n",
        g_nvme_sw_table->total_request_count, g_outstanding_requests);
    iterate_all_tenants(fg_handle) {
        nvme_fg = bitmap_test(g_nvme_fgs_bitmap, fg_handle)
                      ? &g_nvme_fgs[fg_handle]
                      : NULL;
        if (nvme_fg != NULL) {
            printf("%ld-queue has %ld requests, ", fg_handle,
                   nvme_sw_table_count(g_nvme_sw_table, fg_handle));
            printf("demands = %ld, saved_tokens = %ld, token credit = %ld.\n",
                   g_nvme_sw_table->total_token_demand[fg_handle],
                   g_nvme_sw_table->saved_tokens[fg_handle],
                   g_nvme_sw_table->token_credit[fg_handle]);
        }
    }
}

/**
 * init_nvme_request_cpu - allocates the core-local nvme request region
 *
 * Returns 0 if successful, otherwise failure.
 */
int init_nvme_request_cpu(void) {
    struct less_tenant_mgmt *thread_tenant_manager =
        &percpu_get(tenant_manager);
    int ret;

    if (percpu_get(mempool_initialized)) {
        return 0;
    }

    if (CFG.num_nvmedev == 0 || CFG.ns_sizes[0] != 0) {
        printf("No NVMe devices found, skipping initialization\n");
        return 0;
    }

    struct mempool *m2 = &percpu_get(ctx_mempool);
    ret = mempool_create(m2, &ctx_datastore, MEMPOOL_SANITY_PERCPU,
                         percpu_get(cpu_id));
    if (ret) {
        // FIXME: implement mempool destroy
        // mempool_destroy(m);
        return ret;
    }

    init_less_tenant_mgmt(thread_tenant_manager);

    percpu_get(last_sched_time) = timer_now();
    percpu_get(last_sched_time_be) = rdtsc();  // timer_now();
    percpu_get(local_leftover_tokens) = 0;
    percpu_get(local_extra_demand) = 0;
    percpu_get(mempool_initialized) = true;

    return ret;
}

/**
 * init_nvme_request- allocate global nvme request mempool
 */
int init_nvme_request(void) {
    int ret;
    struct mempool_datastore *m = &request_datastore;
    struct mempool_datastore *m2 = &ctx_datastore;

    if (CFG.num_nvmedev == 0 || CFG.ns_sizes[0] != 0) {
        return 0;
    }

    ret = mempool_create_datastore(m2, NUM_NVME_REQUESTS,
                                   sizeof(struct nvme_ctx), "nvme_ctx");
    if (ret) {
        // mempool_pagemem_destroy(m);
        return ret;
    }

    // need to alloc req mempool for admin queue
    init_nvme_request_cpu();
    nvme_sw_table_init(&g_nvme_sw_table);
    if (g_nvme_sw_table == NULL) {
        panic("ERROR: failed to allocate nvme_sw_table\n");
        return -RET_NOMEM;
    }
#ifdef ENABLE_KSTATS
    rte_timer_init(&percpu_get(_qstats_timer));
    rte_timer_reset(&percpu_get(_qstats_timer), QSTATS_INTERVAL, PERIODICAL,
                    rte_lcore_id(), print_queue_status, NULL);
#endif

    set_token_deficit_limit();

    return 0;
}

/**
 * nvme_request_exit_cpu - frees the core-local nvme request region
 */
void nvme_request_exit_cpu(void) {
    // mempool_pagemem_destroy(&request_datastore);
    // mempool_pagemem_destroy(&ctx_datastore);
    // mempool_pagemem_destroy(&nvme_swq_datastore);
}

static bool probe_cb(void *cb_ctx, struct spdk_pci_device *dev,
                     struct spdk_nvme_ctrlr_opts *opts) {
    printf("probe return\n");
    if (dev == NULL) {
        log_err("nvmedev: failed to start driver\n");
        return -ENODEV;
    }

    printf("attaching to nvme device\n");
    return true;
}

static void attach_cb(void *cb_ctx, struct spdk_pci_device *dev,
                      struct spdk_nvme_ctrlr *ctrlr,
                      const struct spdk_nvme_ctrlr_opts *opts) {
    unsigned int num_ns, nsid;
    const struct spdk_nvme_ctrlr_data *cdata;
    struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);

    bitmap_init(g_ioq_bitmap, MAX_NUM_IO_QUEUES, 0);
    if (active_nvme_devices < CFG_MAX_NVMEDEV) {
        nvme_ctrlr[active_nvme_devices++] = ctrlr;
    } else {
        panic("ERROR: only support %d nvme devices\n", CFG_MAX_NVMEDEV);
        return -RET_INVAL;
    }
    cdata = spdk_nvme_ctrlr_get_data(ctrlr);

    if (!spdk_nvme_ns_is_active(ns)) {
        printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
               cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns));
        return;
    }

    printf("Attached to device %-20.20s (%-20.20s) controller: %p\n", cdata->mn,
           cdata->sn, ctrlr);

    num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
    printf("Found %i namespaces\n", num_ns);
    for (nsid = 1; nsid <= num_ns; nsid++) {
        struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
        printf("NS: %i, size: %lx\n", nsid, spdk_nvme_ns_get_size(ns));
    }
}

/**
 * nvmedev_init - initializes nvme devices
 *
 * Returns 0 if successful, otherwise fail.
 */
int init_nvmedev(void) {
    // if (CFG.num_nvmedev > 1)
    // 	printf("IX supports only one NVME device, ignoring all further
    // devices\n");
    if (CFG.num_nvmedev == 0 || CFG.ns_sizes[0] != 0) {
        return 0;
    } else if (CFG.num_nvmedev > g_cores_active) {
        // panic("ERROR: cores are fewer than SSDs\n");
        printf("WARNING: %d nvme devices are not available.\n",
               CFG.num_nvmedev - g_cores_active);
    }

    int i;
    int cpu_per_ssd =
        ceil((double)g_cores_active /
             CFG.num_nvmedev);  // #core should be a multiple of #nvme devices
    for (i = 0; i < CFG.num_nvmedev; i++) {
        cpu2ssd[i] = i / cpu_per_ssd;
    }
    printf("Each SSD will be processed by %d cores.", cpu_per_ssd);

    if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
        printf("spdk_nvme_probe() failed\n");
        return 1;
    }
    return 0;
}

int init_nvmeqp_cpu(void) {
    if (CFG.num_nvmedev == 0 || CFG.ns_sizes[0] != 0) return 0;
    assert(nvme_ctrlr);
    struct spdk_nvme_ctrlr *ctrlr = nvme_ctrlr[cpu2ssd[percpu_get(cpu_id)]];
    struct spdk_nvme_io_qpair_opts opts;

    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
    printf("Deafult io qpair opts: %d, %d, %d\n", opts.qprio,
           opts.io_queue_size, opts.io_queue_requests);
    // opts.qprio = 0;
    opts.io_queue_size = opts.io_queue_size;
    opts.io_queue_requests = opts.io_queue_requests;
    g_max_outstanding_requests = opts.io_queue_requests;

    percpu_get(qpair) =
        spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));

    assert(percpu_get(qpair));

    return 0;
}

void nvmedev_exit(void) {
    struct spdk_nvme_ctrlr *nvme = nvme_ctrlr[cpu2ssd[percpu_get(cpu_id)]];
    if (!nvme) return;
}

int allocate_nvme_ioq(void) {
    int q;

    spin_lock(&nvme_bitmap_lock);
    for (q = 1; q < MAX_NUM_IO_QUEUES; q++) {
        if (bitmap_test(g_ioq_bitmap, q)) continue;
        bitmap_set(g_ioq_bitmap, q);
        break;
    }
    spin_unlock(&nvme_bitmap_lock);

    if (q == MAX_NUM_IO_QUEUES) {
        return -ENOMEM;
    }

    return q;
}

struct nvme_string {
    uint16_t value;
    const char *str;
};
static const struct nvme_string generic_status[] = {
    {SPDK_NVME_SC_SUCCESS, "SUCCESS"},
    {SPDK_NVME_SC_INVALID_OPCODE, "INVALID OPCODE"},
    {SPDK_NVME_SC_INVALID_FIELD, "INVALID_FIELD"},
    {SPDK_NVME_SC_COMMAND_ID_CONFLICT, "COMMAND ID CONFLICT"},
    {SPDK_NVME_SC_DATA_TRANSFER_ERROR, "DATA TRANSFER ERROR"},
    {SPDK_NVME_SC_ABORTED_POWER_LOSS, "ABORTED - POWER LOSS"},
    {SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, "INTERNAL DEVICE ERROR"},
    {SPDK_NVME_SC_ABORTED_BY_REQUEST, "ABORTED - BY REQUEST"},
    {SPDK_NVME_SC_ABORTED_SQ_DELETION, "ABORTED - SQ DELETION"},
    {SPDK_NVME_SC_ABORTED_FAILED_FUSED, "ABORTED - FAILED FUSED"},
    {SPDK_NVME_SC_ABORTED_MISSING_FUSED, "ABORTED - MISSING FUSED"},
    {SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT, "INVALID NAMESPACE OR FORMAT"},
    {SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR, "COMMAND SEQUENCE ERROR"},
    {SPDK_NVME_SC_LBA_OUT_OF_RANGE, "LBA OUT OF RANGE"},
    {SPDK_NVME_SC_CAPACITY_EXCEEDED, "CAPACITY EXCEEDED"},
    {SPDK_NVME_SC_NAMESPACE_NOT_READY, "NAMESPACE NOT READY"},
    {0xFFFF, "GENERIC"}};

static const struct nvme_string command_specific_status[] = {
    {SPDK_NVME_SC_COMPLETION_QUEUE_INVALID, "INVALID COMPLETION QUEUE"},
    {SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER, "INVALID QUEUE IDENTIFIER"},
    {SPDK_NVME_SC_INVALID_QUEUE_SIZE, "INVALID QUEUE SIZE"},
    {SPDK_NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED, "ABORT CMD LIMIT EXCEEDED"},
    {SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED, "ASYNC LIMIT EXCEEDED"},
    {SPDK_NVME_SC_INVALID_FIRMWARE_SLOT, "INVALID FIRMWARE SLOT"},
    {SPDK_NVME_SC_INVALID_FIRMWARE_IMAGE, "INVALID FIRMWARE IMAGE"},
    {SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR, "INVALID INTERRUPT VECTOR"},
    {SPDK_NVME_SC_INVALID_LOG_PAGE, "INVALID LOG PAGE"},
    {SPDK_NVME_SC_INVALID_FORMAT, "INVALID FORMAT"},
    {SPDK_NVME_SC_CONFLICTING_ATTRIBUTES, "CONFLICTING ATTRIBUTES"},
    {SPDK_NVME_SC_INVALID_PROTECTION_INFO, "INVALID PROTECTION INFO"},
    {SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_RANGE, "WRITE TO RO RANGE"},
    {0xFFFF, "COMMAND SPECIFIC"}};

static const struct nvme_string media_error_status[] = {
    {SPDK_NVME_SC_WRITE_FAULTS, "WRITE FAULTS"},
    {SPDK_NVME_SC_UNRECOVERED_READ_ERROR, "UNRECOVERED READ ERROR"},
    {SPDK_NVME_SC_GUARD_CHECK_ERROR, "GUARD CHECK ERROR"},
    {SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR, "APPLICATION TAG CHECK ERROR"},
    {SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR, "REFERENCE TAG CHECK ERROR"},
    {SPDK_NVME_SC_COMPARE_FAILURE, "COMPARE FAILURE"},
    {SPDK_NVME_SC_ACCESS_DENIED, "ACCESS DENIED"},
    {0xFFFF, "MEDIA ERROR"}};

static const char *nvme_get_string(const struct nvme_string *strings,
                                   uint16_t value) {
    const struct nvme_string *entry;

    entry = strings;

    while (entry->value != 0xFFFF) {
        if (entry->value == value) {
            return entry->str;
        }
        entry++;
    }
    return entry->str;
}

static const char *get_status_string(uint16_t sct, uint16_t sc) {
    const struct nvme_string *entry;

    switch (sct) {
        case SPDK_NVME_SCT_GENERIC:
            entry = generic_status;
            break;
        case SPDK_NVME_SCT_COMMAND_SPECIFIC:
            entry = command_specific_status;
            break;
        case SPDK_NVME_SCT_MEDIA_ERROR:
            entry = media_error_status;
            break;
        case SPDK_NVME_SCT_VENDOR_SPECIFIC:
            return "VENDOR SPECIFIC";
        default:
            return "RESERVED";
    }

    return nvme_get_string(entry, sc);
}

void nvme_write_cb(void *ctx, const struct spdk_nvme_cpl *cpl) {
    struct nvme_ctx *n_ctx = (struct nvme_ctx *)ctx;

    g_nvme_fgs[n_ctx->fg_handle].completions++;

    if (spdk_nvme_cpl_is_error(cpl)) {
        printf("SPDK Write Failed!\n");
        printf(
            "%s (%02x/%02x) sqid:%d cid:%d cdw0:%x sqhd:%04x p:%x m:%x "
            "dnr:%x\n",
            get_status_string(cpl->status.sct, cpl->status.sc), cpl->status.sct,
            cpl->status.sc, cpl->sqid, cpl->cid, cpl->cdw0, cpl->sqhd,
            cpl->status.p, cpl->status.m, cpl->status.dnr);
    }

    usys_nvme_written(n_ctx->cookie, RET_OK);

    free_local_nvme_ctx(n_ctx);
}

void nvme_read_cb(void *ctx, const struct spdk_nvme_cpl *cpl) {
    struct nvme_ctx *n_ctx = (struct nvme_ctx *)ctx;

    g_nvme_fgs[n_ctx->fg_handle].completions++;

    if (spdk_nvme_cpl_is_error(cpl)) {
        printf("SPDK Read Failed!\n");
        printf(
            "%s (%02x/%02x) sqid:%d cid:%d cdw0:%x sqhd:%04x p:%x m:%x "
            "dnr:%x\n",
            get_status_string(cpl->status.sct, cpl->status.sc), cpl->status.sct,
            cpl->status.sc, cpl->sqid, cpl->cid, cpl->cdw0, cpl->sqhd,
            cpl->status.p, cpl->status.m, cpl->status.dnr);
    }

    usys_nvme_response(n_ctx->cookie, n_ctx->user_buf.buf, RET_OK);

    free_local_nvme_ctx(n_ctx);
}

long bsys_nvme_open(long dev_id, long ns_id) {
    struct spdk_nvme_ns *ns;
    int ioq;

    KSTATS_VECTOR(bsys_nvme_open);

    // FIXME: we may want 1 bitmap per device
    ioq = allocate_nvme_ioq();
    if (ioq < 0) {
        return -RET_NOBUFS;
    }
    bitmap_init(g_nvme_fgs_bitmap, MAX_NVME_FLOW_GROUPS, 0);

    percpu_get(open_ev[percpu_get(open_ev_ptr)++]) = ioq;
    ns = spdk_nvme_ctrlr_get_ns(nvme_ctrlr[cpu2ssd[percpu_get(cpu_id)]], ns_id);
    global_ns_size = spdk_nvme_ns_get_size(ns);
    global_ns_sector_size = spdk_nvme_ns_get_sector_size(ns);
    printf("NVMe device namespace size: %lu bytes, sector size: %lu\n",
           spdk_nvme_ns_get_size(ns), spdk_nvme_ns_get_sector_size(ns));
    return RET_OK;
}

long bsys_nvme_close(long dev_id, long ns_id, hqu_t handle) {
    KSTATS_VECTOR(bsys_nvme_close);
    printf("BSYS NVME CLOSE\n");
    bitmap_clear(g_ioq_bitmap, handle);
    usys_nvme_closed(handle, 0);
    return RET_OK;
}

int set_nvme_flow_group_id(long flow_group_id, long *fg_handle_to_set,
                           unsigned long cookie) {
    int i;
    int next_avail = 0;
    // first check if already registered this flow
    spin_lock(&nvme_bitmap_lock);
    for (i = 1; i < MAX_NVME_FLOW_GROUPS; i++) {
        if (bitmap_test(g_nvme_fgs_bitmap, i)) {
            // if already registered this flow group, return its index
            if (g_nvme_fgs[i].flow_group_id == flow_group_id &&
                g_nvme_fgs[i].tid == RTE_PER_LCORE(cpu_nr)) {
                *fg_handle_to_set = i;
                spin_unlock(&nvme_bitmap_lock);
                // if (g_nvme_fgs[i].cookie == cookie)
                return 1;
                // else
                //     return 2;
            }
        } else {
            if (next_avail == 0) {
                next_avail = i;
            }
        }
    }

    if (next_avail == MAX_NVME_FLOW_GROUPS) {
        spin_unlock(&nvme_bitmap_lock);
        return -ENOMEM;
    }

    bitmap_set(g_nvme_fgs_bitmap, next_avail);
    spin_unlock(&nvme_bitmap_lock);

    *fg_handle_to_set = next_avail;
    return 0;
}

// adjust token deficit limit to allow LC tenants to burst, but not too much
static void set_token_deficit_limit(void) {
    printf(
        "DEVICE PARAMS: read cost %d, write cost %d, burst limits = %d "
        "writes\n",
        NVME_READ_COST, NVME_WRITE_COST, WRITE_BURST_COUNT);
    TOKEN_DEFICIT_LIMIT = WRITE_BURST_COUNT * NVME_WRITE_COST;
}

static unsigned long find_token_limit_from_devmodel(unsigned int lat_SLO) {
    int i = 0;
    unsigned long y0, y1, x0, x1;
    double y;

    for (i = 0; i < g_dev_model_size; i++) {
        if (lat_SLO < g_dev_models[i].p95_tail_latency) {
            break;
        }
    }
    if (i > 0) {
        if (global_readonly_flag) {
            if (i == g_dev_model_size) {
                return g_dev_models[i - 1].token_rdonly_rate_limit;
            }
            // linear interpolation of token limits provided in devmodel config
            // file
            y0 = g_dev_models[i - 1].token_rdonly_rate_limit;
            y1 = g_dev_models[i].token_rdonly_rate_limit;
            x0 = g_dev_models[i - 1].p95_tail_latency;
            x1 = g_dev_models[i].p95_tail_latency;
            assert(x1 - x0 != 0);
            y = y0 + ((y1 - y0) * (lat_SLO - x0) / (double)(x1 - x0));
            return (unsigned long)y;

        } else {
            if (i == g_dev_model_size) {
                return g_dev_models[i - 1].token_rate_limit;
            }
            // linear interpolation of token limits provided in devmodel config
            // file
            y0 = g_dev_models[i - 1].token_rate_limit;
            y1 = g_dev_models[i].token_rate_limit;
            x0 = g_dev_models[i - 1].p95_tail_latency;
            x1 = g_dev_models[i].p95_tail_latency;
            y = y0 + ((y1 - y0) * (lat_SLO - x0) / (double)(x1 - x0));
            assert(x1 - x0 != 0);
            return (unsigned long)y;
        }
    }
    printf("WARNING: provide dev model info for latency SLO %d\n", lat_SLO);
    if (global_readonly_flag) {
        return g_dev_models[0].token_rdonly_rate_limit;
    }
    return g_dev_models[0].token_rate_limit;
}

unsigned long lookup_device_token_rate(unsigned int lat_SLO) {
    switch (g_nvme_dev_model) {
        case DEFAULT_FLASH:
            return UINT_MAX;
        case FAKE_FLASH:
            return UINT_MAX;
        case FLASH_DEV_MODEL:
            return find_token_limit_from_devmodel(lat_SLO);
        default:
            printf("WARNING: undefined flash device model\n");
            return UINT_MAX;
    }

    log_err("Set device lookup for lat SLO %ld!\n", lat_SLO);
    return 500000;
}

unsigned long scaled_IOPS(unsigned long IOPS, int rw_ratio_100) {
    double scaledIOPS;
    double rw_ratio = (double)rw_ratio_100 / (double)100;

    /*
     * NOTE: when calculating token reservation for latency-critical tenants,
     * 		 assume SLO specificed for 4kB requests
     * 		 e.g. if your application's IOPS SLO is 100K IOPS for 8K IOs,
     * 		      register your app's SLO with ReFlex as 200K IOPS
     */
    scaledIOPS =
        (IOPS * rw_ratio * nvme_compute_req_cost(NVME_CMD_READ, SLO_REQ_SIZE)) +
        (IOPS * (1 - rw_ratio) *
         nvme_compute_req_cost(NVME_CMD_WRITE, SLO_REQ_SIZE));
    return (unsigned long)(scaledIOPS + 0.5);
}

static void readjust_lc_tenant_token_limits(void) {
    int i, j = 0;
    for (i = 0; i < MAX_NVME_FLOW_GROUPS; i++) {
        if (bitmap_test(g_nvme_fgs_bitmap, i)) {
            if (g_nvme_fgs[i].latency_critical_flag) {
                g_nvme_fgs[i].scaled_IOPuS_limit =
                    (g_nvme_fgs[i].scaled_IOPS_limit + global_lc_boost_no_BE) /
                    (double)1E6;
                j++;
                if (j == global_num_lc_tenants) {
                    return;
                }
            }
        }
    }
}

int recalculate_weights_add(long new_flow_group_idx) {
    unsigned long new_global_token_rate = 0;
    unsigned long new_global_LC_sum_token_rate = 0;
    unsigned long lc_token_rate_boost_when_no_BE = 0;
    unsigned int be_token_rate_per_tenant;

    spin_lock(&nvme_bitmap_lock);

    if (g_nvme_fgs[new_flow_group_idx].latency_critical_flag) {
        new_global_LC_sum_token_rate =
            global_LC_sum_token_rate +
            g_nvme_fgs[new_flow_group_idx].scaled_IOPS_limit;
        if (g_nvme_fgs[new_flow_group_idx].rw_ratio_SLO < 100) {
            global_readonly_flag = false;
        }

        new_global_token_rate = lookup_device_token_rate(
            g_nvme_fgs[new_flow_group_idx].latency_us_SLO);
        if (new_global_token_rate > global_token_rate) {
            new_global_token_rate =
                global_token_rate;  // keep limit based on strictest latency SLO
        }

        if (new_global_LC_sum_token_rate > new_global_token_rate) {
            // control plane notifies tenant can't meet its SLO
            // don't update the global token rate since won't regsiter this
            // tenant
            log_err("CANNOT SATISFY TENANT's SLO: %lu > %lu\n",
                    new_global_LC_sum_token_rate, new_global_token_rate);
            spin_unlock(&nvme_bitmap_lock);
            return -RET_CANTMEETSLO;
        }

        global_token_rate = new_global_token_rate;
        global_LC_sum_token_rate = new_global_LC_sum_token_rate;
        printf("Global token rate: %lu tokens/s.\n", global_token_rate);
        global_num_lc_tenants++;
    } else {
        global_num_best_effort_tenants++;
        global_readonly_flag =
            false;  // assume BE tenant has rd/wr mixed workload
    }

    if (global_num_best_effort_tenants) {
        be_token_rate_per_tenant =
            (global_token_rate - global_LC_sum_token_rate) /
            global_num_best_effort_tenants;
        lc_token_rate_boost_when_no_BE = 0;
    } else {
        be_token_rate_per_tenant = 0;
        if (global_num_lc_tenants)
            lc_token_rate_boost_when_no_BE =
                (global_token_rate - global_LC_sum_token_rate) /
                global_num_lc_tenants;
    }
    atomic_write(&global_be_token_rate_per_tenant, be_token_rate_per_tenant);

    // if number of BE tenants has changes from 0 to 1 or more (or vice versa)
    // adjust LC tenant boost (only want to boost if no BE tenants registered)
    if (lc_token_rate_boost_when_no_BE != global_lc_boost_no_BE) {
        global_lc_boost_no_BE = lc_token_rate_boost_when_no_BE;
        readjust_lc_tenant_token_limits();
    }
    spin_unlock(&nvme_bitmap_lock);

    return 1;
}

int recalculate_weights_remove(long flow_group_idx) {
    long i;
    unsigned int strictest_latency_SLO = UINT_MAX;
    unsigned int be_token_rate_per_tenant;
    unsigned long lc_token_rate_boost_when_no_BE = 0;

    spin_lock(&nvme_bitmap_lock);

    if (g_nvme_fgs[flow_group_idx].latency_critical_flag) {
        // find new strictest latency SLO
        global_readonly_flag = true;
        for (i = 0; i < MAX_NVME_FLOW_GROUPS; i++) {
            if (bitmap_test(g_nvme_fgs_bitmap, i) && i != flow_group_idx) {
                if (g_nvme_fgs[i].latency_critical_flag) {
                    if (g_nvme_fgs[i].latency_us_SLO < strictest_latency_SLO) {
                        strictest_latency_SLO = g_nvme_fgs[i].latency_us_SLO;
                    }
                    if (g_nvme_fgs[i].rw_ratio_SLO < 100) {
                        global_readonly_flag = false;
                    }
                }
            }
        }
        global_LC_sum_token_rate -=
            g_nvme_fgs[flow_group_idx].scaled_IOPS_limit;
        global_token_rate = lookup_device_token_rate(strictest_latency_SLO);

        // printf("Flow %ld finished %ld requests\n", flow_group_idx,
        // g_nvme_fgs[flow_group_idx].completions); printf("Global token rate:
        // %lu tokens/s\n", global_token_rate);

        global_num_lc_tenants--;
    } else {
        global_num_best_effort_tenants--;
    }

    if (global_num_best_effort_tenants) {
        global_readonly_flag = false;
        be_token_rate_per_tenant =
            (global_token_rate - global_LC_sum_token_rate) /
            global_num_best_effort_tenants;
        lc_token_rate_boost_when_no_BE = 0;
    } else {
        be_token_rate_per_tenant = 0;
        if (global_num_lc_tenants)
            lc_token_rate_boost_when_no_BE =
                (global_token_rate - global_LC_sum_token_rate) /
                global_num_lc_tenants;
    }
    atomic_write(&global_be_token_rate_per_tenant, be_token_rate_per_tenant);

    // if number of BE tenants has changes from 0 to 1 or more (or vice versa)
    // adjust LC tenant boost (only want to boost if no BE tenants registered)
    if (lc_token_rate_boost_when_no_BE != global_lc_boost_no_BE) {
        global_lc_boost_no_BE = lc_token_rate_boost_when_no_BE;
        readjust_lc_tenant_token_limits();
    }

    spin_unlock(&nvme_bitmap_lock);

    return 1;
}

// TODO: consider implementing separate per-thread lists for BE and LC tenants
// (will simplify some code for scheduler)
long bsys_nvme_register_flow(long flow_group_id, unsigned long cookie,
                             unsigned int latency_us_SLO, unsigned int IOPS_SLO,
                             unsigned short rw_ratio_SLO) {
    long fg_handle = 0;
    struct nvme_flow_group *nvme_fg;
    int ret = 0;
    int flow_state = 0;
    int lc_tenant_count = 0;
    struct less_tenant_mgmt *thread_tenant_manager;
    struct nvme_sw_queue *swq;
    KSTATS_VECTOR(bsys_nvme_register_flow);

    if (!g_nvme_sched_mode) {
        printf(
            "Register new-tenant %ld (flow_group: %ld). Managed by thread "
            "%ld, scheduler=off\n",
            fg_handle, flow_group_id, RTE_PER_LCORE(cpu_nr));
        nvme_fg = &g_nvme_fgs[0];
        nvme_fg->conn_ref_count++;

        usys_nvme_registered_flow(0, cookie, RET_OK);
        return RET_OK;
    }

    flow_state = set_nvme_flow_group_id(flow_group_id, &fg_handle, cookie);
    if (fg_handle < 0) {
        log_err("error: exceeded max (%d) nvme flow groups!\n",
                MAX_NVME_FLOW_GROUPS);
    }

    nvme_fg = &g_nvme_fgs[fg_handle];
    nvme_fg->completions = 0;

    if (latency_us_SLO == 0) {
        nvme_fg->latency_critical_flag = false;
    } else {
        nvme_fg->latency_critical_flag = true;
    }

    if (flow_state == 2) {
        // Old connection with new SLO
        bsys_nvme_unregister_flow(fg_handle);
        printf(
            "WARNING: tenant connection registered different SLO, will "
            "overwrite previous SLO for all of this tenant's connections. "
            "supporting only 1 SLO per tenant.\n");
        flow_state = 0;
    } else {
        nvme_fg->scaled_IOPS_limit = scaled_IOPS(IOPS_SLO, rw_ratio_SLO);
        nvme_fg->scaled_IOPuS_limit = nvme_fg->scaled_IOPS_limit / (double)1E6;
    }
    if (flow_state == 1) {
        // New connection with old SLO
        ret = recalculate_weights_add(fg_handle);
        if (ret < 0) {
            printf("WARNING: cannot satisfy SLO\n");
            return -RET_CANTMEETSLO;
        }
    } else if (flow_state == 0) {
        // New connection with new SLO
        nvme_fg->flow_group_id = flow_group_id;
        // nvme_fg->cookie = cookie;
        nvme_fg->latency_us_SLO = latency_us_SLO;
        nvme_fg->IOPS_SLO = IOPS_SLO;
        nvme_fg->rw_ratio_SLO = rw_ratio_SLO;
        nvme_fg->tid = RTE_PER_LCORE(cpu_nr);

        ret = recalculate_weights_add(fg_handle);
        if (ret < 0) {
            printf("WARNING: cannot satisfy SLO\n");
            return -RET_CANTMEETSLO;
        }

        thread_tenant_manager = &percpu_get(tenant_manager);

        nvme_fg->conn_ref_count = 0;
        if (latency_us_SLO == 0) {
            thread_tenant_manager->num_be_tenants++;
        } else {
            thread_tenant_manager->num_lc_tenants++;
            if (thread_tenant_manager->num_lc_tenants == 1) {
                percpu_get(min_scaled_IOPS) = nvme_fg->scaled_IOPS_limit;
                percpu_get(min_tenant_count) = 1;
            } else if (thread_tenant_manager->num_lc_tenants > 1) {
                if (percpu_get(min_scaled_IOPS) > nvme_fg->scaled_IOPS_limit) {
                    percpu_get(min_scaled_IOPS) = nvme_fg->scaled_IOPS_limit;
                    percpu_get(min_tenant_count) = 1;
                } else if (percpu_get(min_scaled_IOPS) ==
                           nvme_fg->scaled_IOPS_limit) {
                    percpu_get(min_tenant_count) += 1;
                }
            }
        }
    }

    nvme_fg->conn_ref_count++;
    usys_nvme_registered_flow(fg_handle, cookie, RET_OK);

    return RET_OK;
}

long bsys_nvme_unregister_flow(long fg_handle) {
    struct less_tenant_mgmt *thread_tenant_manager;
    struct nvme_flow_group *nvme_fg;
    unsigned long smallest_IOPS_limit = ULONG_MAX;
    int i = 0;
    KSTATS_VECTOR(bsys_nvme_unregister_flow);

    if (!g_nvme_sched_mode) {
        nvme_fg = &g_nvme_fgs[0];
        nvme_fg->conn_ref_count--;
        printf("Flow deregistered, remaining %d connections\n",
               nvme_fg->conn_ref_count);
        usys_nvme_unregistered_flow(0, RET_OK);
        return RET_OK;
    }
    nvme_fg = &g_nvme_fgs[fg_handle];
    nvme_fg->conn_ref_count--;
    nvme_fg->scaled_IOPS_limit -=
        scaled_IOPS(nvme_fg->IOPS_SLO, nvme_fg->rw_ratio_SLO) / (double)1E6;
    if (nvme_fg->scaled_IOPS_limit < 0) {
        printf("Unexpected unregisteration (handle: %ld)\n", fg_handle);
    }

    recalculate_weights_remove(fg_handle);

    if (nvme_fg->conn_ref_count == 0) {
        thread_tenant_manager = &percpu_get(tenant_manager);
        if (!nvme_fg->latency_critical_flag) {
            thread_tenant_manager->num_be_tenants--;
        } else {
            thread_tenant_manager->num_lc_tenants--;
            if (thread_tenant_manager->num_lc_tenants == 1) {
                percpu_get(min_scaled_IOPS) = smallest_IOPS_limit;
                percpu_get(min_tenant_count) = 0;
            } else {
                if (percpu_get(min_scaled_IOPS) == nvme_fg->scaled_IOPS_limit) {
                    // fine new min, FIXME: use minheap to improve efficiency
                    percpu_get(min_tenant_count) -= 1;
                    if (percpu_get(min_tenant_count) == 0) {
                        for (i = 0; i < MAX_NVME_FLOW_GROUPS; i++) {
                            if (bitmap_test(g_nvme_fgs_bitmap, i) &&
                                i != fg_handle) {
                                if (g_nvme_fgs[i].latency_critical_flag) {
                                    if (g_nvme_fgs[i].scaled_IOPS_limit <
                                        smallest_IOPS_limit) {
                                        percpu_get(min_scaled_IOPS) =
                                            g_nvme_fgs[i].scaled_IOPS_limit;
                                        percpu_get(min_tenant_count) = 1;
                                    } else if (g_nvme_fgs[i]
                                                   .scaled_IOPS_limit ==
                                               smallest_IOPS_limit) {
                                        percpu_get(min_tenant_count) += 1;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        spin_lock(&nvme_bitmap_lock);
        bitmap_clear(g_nvme_fgs_bitmap, fg_handle);
        spin_unlock(&nvme_bitmap_lock);
    }

    usys_nvme_unregistered_flow(fg_handle, RET_OK);

    return RET_OK;
}

// request cost scales linearly with size above 4KB
// note: may need to adjust this if does not match your Flash device behavior
static int nvme_compute_req_cost(int req_type, size_t req_len) {
    if (req_len <= 0) {
        printf("ERROR: request size <= 0!\n");
        return 0;
    }

    int len_scale_factor = 1;

    if (req_len > 4096) {
        // divide req_len by 4096 and round up
        len_scale_factor = (req_len + 4096 - 1) / 4096;
    }

    if (req_type == NVME_CMD_READ) {
        return NVME_READ_COST * len_scale_factor;
    } else if (req_type == NVME_CMD_WRITE) {
        return NVME_WRITE_COST * len_scale_factor;
    }
    return 1;
}

static void sgl_reset_cb(void *cb_arg, uint32_t sgl_offset) {
    struct nvme_ctx *ctx = (struct nvme_ctx *)cb_arg;

    ctx->user_buf.sgl_buf.current_sgl = sgl_offset / SGL_PAGE_SIZE;
}

static int sgl_next_cb(void *cb_arg, uint64_t *address, uint32_t *length) {
    void *paddr;
    void __user *__restrict temp;
    struct nvme_ctx *ctx = (struct nvme_ctx *)cb_arg;

    if (ctx->user_buf.sgl_buf.current_sgl >= ctx->user_buf.sgl_buf.num_sgls) {
        *address = 0;
        *length = 0;
        printf("WARNING: nvme req size mismatch\n");
        assert(0);
    } else {
        temp = ctx->user_buf.sgl_buf.sgl[ctx->user_buf.sgl_buf.current_sgl];
        ctx->user_buf.sgl_buf.current_sgl++;
        *address = (void *)temp;
        *length = 4096;  // PGSIZE_4KB
    }
    return 0;
}

static void issue_nvme_req(struct nvme_ctx *ctx) {
    int ret;

    KSTATS_VECTOR(issue_nvme_req);

    // don't schedule request on flash if FAKE_FLASH test
    if (g_nvme_dev_model == FAKE_FLASH) {
        if (ctx->cmd == NVME_CMD_READ) {
            usys_nvme_response(ctx->cookie, ctx->user_buf.buf, RET_OK);
            percpu_get(received_nvme_completions)++;
        } else if (ctx->cmd == NVME_CMD_WRITE) {
            usys_nvme_written(ctx->cookie, RET_OK);
            percpu_get(received_nvme_completions)++;
        }
        free_local_nvme_ctx(ctx);

        return;
    }

    if (ctx->cmd == NVME_CMD_READ) {
        ret = spdk_nvme_ns_cmd_readv(ctx->ns, percpu_get(qpair), ctx->lba,
                                     ctx->lba_count, nvme_read_cb, ctx, 0,
                                     sgl_reset_cb, sgl_next_cb);

    } else if (ctx->cmd == NVME_CMD_WRITE) {
        ret = spdk_nvme_ns_cmd_writev(ctx->ns, percpu_get(qpair), ctx->lba,
                                      ctx->lba_count, nvme_write_cb, ctx, 0,
                                      sgl_reset_cb, sgl_next_cb);

    } else {
        panic("unrecognized nvme request\n");
    }
    g_outstanding_requests++;
    if (ret < 0) {
        printf("Error submitting nvme request\n");
        printf("Current outstanding: %ld\n", g_outstanding_requests);
        panic("Ran out of NVMe cmd buffer space\n");
    }
}

long bsys_nvme_write(hqu_t priority, void *buf, unsigned long lba,
                     unsigned int lba_count, unsigned long cookie) {
    printf("bsys_nvme_write should not be called\n");
}
long bsys_nvme_read(hqu_t priority, void *buf, unsigned long lba,
                    unsigned int lba_count, unsigned long cookie) {
    printf("bsys_nvme_read should not be called\n");
}

long bsys_nvme_writev(hqu_t fg_handle, void __user **__restrict buf,
                      int num_sgls, unsigned long lba, unsigned int lba_count,
                      unsigned long cookie) {
    struct spdk_nvme_ns *ns;
    struct nvme_ctx *ctx;
    struct nvme_ctx *pctx;
    int ret;

    KSTATS_VECTOR(bsys_nvme_writev);

    ns = spdk_nvme_ctrlr_get_ns(nvme_ctrlr[cpu2ssd[percpu_get(cpu_id)]],
                                global_ns_id);

    ctx = alloc_local_nvme_ctx();
    if (ctx == NULL) {
        printf(
            "ERROR: Cannot allocate memory for nvme_ctx in bsys_nvme_read\n");
        return -RET_NOMEM;
    }
    ctx->cookie = cookie;
    ctx->user_buf.sgl_buf.sgl = buf;
    ctx->user_buf.sgl_buf.num_sgls = num_sgls;

    // Store all info in ctx before add to software queue
    ctx->tid = percpu_get(cpu_nr);
    ctx->cmd = NVME_CMD_WRITE;
    ctx->ns = ns;
    ctx->lba = lba;
    ctx->lba_count = lba_count;

    if (!g_nvme_sched_mode) {
        // always using the first queue
        ctx->fg_handle = 0;
        ret = nvme_sw_table_push_back(g_nvme_sw_table, 0, ctx);
        if (ret != 0) {
            printf("sw table has %ld requests\n",
                   nvme_sw_table_count(g_nvme_sw_table, 0));
            return -RET_NOMEM;
        }
        while (nvme_sw_table_isempty(g_nvme_sw_table, 0) == 0) {
            if (g_outstanding_requests >= g_max_outstanding_requests) {
                break;
            }
            nvme_sw_table_pop_front(g_nvme_sw_table, 0, &pctx);
            issue_nvme_req(pctx);
        }
    } else {
        ctx->fg_handle = fg_handle;
        ctx->req_cost = nvme_compute_req_cost(
            NVME_CMD_WRITE, lba_count * global_ns_sector_size);
        if (g_nvme_fgs[fg_handle].latency_critical_flag &&
            nvme_sw_table_isempty(g_nvme_sw_table, fg_handle)) {
            nvme_lc_tenant_activate(&percpu_get(tenant_manager), fg_handle);
        }
        if (!g_nvme_fgs[fg_handle].latency_critical_flag &&
            nvme_sw_table_isempty(g_nvme_sw_table, fg_handle)) {
            nvme_be_tenant_activate(&percpu_get(tenant_manager), fg_handle);
        }
        ret = nvme_sw_table_push_back(g_nvme_sw_table, fg_handle, ctx);
        if (ret != 0) {
            print_queue_status();
            free_local_nvme_ctx(ctx);
            return -RET_NOMEM;
        }
    }

    return RET_OK;
}

long bsys_nvme_readv(hqu_t fg_handle, void __user **__restrict buf,
                     int num_sgls, unsigned long lba, unsigned int lba_count,
                     unsigned long cookie) {
    struct spdk_nvme_ns *ns;
    struct nvme_ctx *ctx;
    struct nvme_ctx *pctx;
    int ret;

    KSTATS_VECTOR(bsys_nvme_readv);

    ns = spdk_nvme_ctrlr_get_ns(nvme_ctrlr[cpu2ssd[percpu_get(cpu_id)]],
                                global_ns_id);

    ctx = alloc_local_nvme_ctx();
    if (ctx == NULL) {
        printf(
            "ERROR: Cannot allocate memory for nvme_ctx in bsys_nvme_read\n");
        return -RET_NOMEM;
    }
    ctx->cookie = cookie;
    ctx->user_buf.sgl_buf.sgl = buf;
    ctx->user_buf.sgl_buf.num_sgls = num_sgls;

    // Store all info in ctx before add to software queue
    ctx->tid = RTE_PER_LCORE(cpu_nr);
    ctx->cmd = NVME_CMD_READ;
    ctx->ns = ns;
    ctx->lba = lba;
    ctx->lba_count = lba_count;

    if (!g_nvme_sched_mode) {
        // always using the first queue
        ctx->fg_handle = 0;
        ret = nvme_sw_table_push_back(g_nvme_sw_table, 0, ctx);

        if (ret != 0) {
            printf("sw table has %ld requests\n",
                   nvme_sw_table_count(g_nvme_sw_table, 0));
            return -RET_NOMEM;
        }
        while (nvme_sw_table_isempty(g_nvme_sw_table, 0) == 0) {
            if (g_outstanding_requests >= g_max_outstanding_requests) {
                break;
            }
            nvme_sw_table_pop_front(g_nvme_sw_table, 0, &pctx);
            issue_nvme_req(pctx);
        }
    } else {
        ctx->fg_handle = fg_handle;
        ctx->req_cost = nvme_compute_req_cost(
            NVME_CMD_READ, lba_count * global_ns_sector_size);
        if (g_nvme_fgs[fg_handle].latency_critical_flag &&
            nvme_sw_table_isempty(g_nvme_sw_table, fg_handle)) {
            nvme_lc_tenant_activate(&percpu_get(tenant_manager), fg_handle);
            printf("LC tenant %ld activated\n", fg_handle);
            int active_count =
                (percpu_get(tenant_manager).lc_tail -
                 percpu_get(tenant_manager).lc_head + MAX_NVME_FLOW_GROUPS) %
                MAX_NVME_FLOW_GROUPS;
            printf("Tenant manager has %d active LC tenants\n", active_count);
        }
        if (!g_nvme_fgs[fg_handle].latency_critical_flag &&
            nvme_sw_table_isempty(g_nvme_sw_table, fg_handle)) {
            nvme_be_tenant_activate(&percpu_get(tenant_manager), fg_handle);
        }
        if (nvme_lc_tenant_isactivated(&percpu_get(tenant_manager),
                                       fg_handle) == false) {
            printf("ERROR: LC tenant %ld is not activated\n", fg_handle);
            return RET_FAULT;
        }
        ret = nvme_sw_table_push_back(g_nvme_sw_table, fg_handle, ctx);
        if (ret != 0) {
            print_queue_status();
            free_local_nvme_ctx(ctx);
            return RET_NOMEM;
        }
    }

    return RET_OK;
}

unsigned long try_acquire_global_tokens(unsigned long token_demand) {
    unsigned long new_token_level = 0;
    unsigned long avail_tokens = 0;

    while (1) {
        avail_tokens = atomic_u64_read(&global_leftover_tokens);

        if (token_demand > avail_tokens) {
            if (atomic_u64_cmpxchg(&global_leftover_tokens, avail_tokens, 0)) {
                return avail_tokens;
            }

        } else {
            new_token_level = avail_tokens - token_demand;
            if (atomic_u64_cmpxchg(&global_leftover_tokens, avail_tokens,
                                   new_token_level)) {
                return token_demand;
            }
        }
    }
}

/*
 * nvme_sched_lessv0_subround1: roundrobinly schedule latency critical tenant
 * traffic
 */
static inline int nvme_sched_lessv0_subround1(void) {
    struct less_tenant_mgmt *thread_tenant_manager;
    struct nvme_ctx *ctx;
    long fg_handle;
    unsigned long now;
    unsigned long time_delta;
    long POS_LIMIT = 0;
    unsigned long local_leftover = 0;
    unsigned long local_demand = 0;
    double token_increment;
    int i = -1;
    uint32_t count = 0;
    bool work_conserving = false;

    now = timer_now();  // in us
    time_delta = now - percpu_get(last_sched_time);
    percpu_get(last_sched_time) = now;

    thread_tenant_manager = &percpu_get(tenant_manager);

    if (g_nvme_sw_table->total_request_count + g_outstanding_requests <
        g_max_outstanding_requests) {
        work_conserving = true;
    }

    iterate_active_tenants_by_type(thread_tenant_manager, lc) {
        fg_handle =
            thread_tenant_manager->active_lc_tenants[i % MAX_NVME_FLOW_GROUPS];
        // printf("iterating active %ld-th tenant %ld\n", i, fg_handle);
        token_increment =
            (g_nvme_fgs[fg_handle].scaled_IOPuS_limit * time_delta) + 0.5;
        g_nvme_sw_table->token_credit[fg_handle] += (long)token_increment;
        while (nvme_sw_table_isempty(g_nvme_sw_table, fg_handle) == 0) {
            if (!work_conserving) {
                if (g_outstanding_requests >= g_max_outstanding_requests) break;
                if (g_nvme_sw_table->token_credit[fg_handle] <
                    -TOKEN_DEFICIT_LIMIT) {
                    printf("active LC tenant %d now is going to be requeued\n",
                           fg_handle);
                    nvme_lc_tenant_requeue(thread_tenant_manager, fg_handle);
                    count++;
                    break;
                }
            }
            nvme_sw_table_pop_front(g_nvme_sw_table, fg_handle, &ctx);
            issue_nvme_req(ctx);
            // how to pay off the loan when in the work conserving mode?
            g_nvme_sw_table->token_credit[fg_handle] -= ctx->req_cost;
        }
        // this algorithm always drains the queues in the front
        if (nvme_sw_table_isempty(g_nvme_sw_table, fg_handle)) {
            printf("active LC tenant %d now is going to be inactive\n",
                   fg_handle);
            count++;
        }
        POS_LIMIT = 3 * token_increment;
        if (g_nvme_sw_table->token_credit[fg_handle] > POS_LIMIT) {
            local_leftover += (g_nvme_sw_table->token_credit[fg_handle] *
                               TOKEN_FRAC_GIVEAWAY);
            g_nvme_sw_table->token_credit[fg_handle] -=
                g_nvme_sw_table->token_credit[fg_handle] * TOKEN_FRAC_GIVEAWAY;
        }
    }
    printf("%d LC tenants are going to be deactivated\n", count);
    nvme_lc_tenant_deactivate(thread_tenant_manager, count);

    percpu_get(local_leftover_tokens) = local_leftover;

    return 0;
}

/*
 * nvme_sched_lessv1: schedule with the LESS schedule
 */
static inline int nvme_sched_lessv1_subround1(void) { return 0; }

/*
 * nvme_sched_lessv2: schedule with the LESS schedule
 */
static inline int nvme_sched_lessv2_subround1(void) { return 0; }

/*
 * nvme_sched_subround2: schedule best-effort tenant traffic
 */
static inline void nvme_sched_subround2(void) {
    struct less_tenant_mgmt *thread_tenant_manager;
    struct nvme_ctx *ctx;
    long fg_handle;
    int i;
    unsigned long local_leftover = 0;
    unsigned long local_demand = 0;
    unsigned long be_tokens = 0;
    double token_increment = 0;
    unsigned long token_demand = 0;
    unsigned long global_tokens_acquired = 0;
    unsigned long now;
    unsigned long time_delta_cycles;
    uint32_t count = 0;

    local_leftover = percpu_get(local_leftover_tokens);

    thread_tenant_manager = &percpu_get(tenant_manager);

    iterate_active_tenants_by_type(thread_tenant_manager, be) {
        fg_handle =
            thread_tenant_manager->active_be_tenants[i % MAX_NVME_FLOW_GROUPS];
        local_demand += g_nvme_sw_table->total_token_demand[fg_handle] -
                        g_nvme_sw_table->saved_tokens[fg_handle];
    }
    // compare local leftover with local demand
    // synchronize access to global token bucket
    if (local_leftover > 0 &&
        local_demand == 0) {  // give away leftoever tokens to global pool
        atomic_u64_fetch_and_add(&global_leftover_tokens, local_leftover);
        return;
    } else if (local_leftover < local_demand) {  // try to get how much you
                                                 // need from global pool
        token_demand = local_demand - local_leftover;
        global_tokens_acquired =
            try_acquire_global_tokens(token_demand);  // atomic
        be_tokens = local_leftover + global_tokens_acquired;
    } else if (local_leftover >= local_demand) {
        be_tokens = local_leftover;
    }

    now = rdtsc();
    time_delta_cycles = now - percpu_get(last_sched_time_be);
    percpu_get(last_sched_time_be) = now;

    // serve best effort tenants in round-robin order
    iterate_active_tenants_by_type(thread_tenant_manager, be) {
        fg_handle =
            thread_tenant_manager->active_be_tenants[i % MAX_NVME_FLOW_GROUPS];
        be_tokens +=
            nvme_sw_table_take_saved_tokens(g_nvme_sw_table, fg_handle);
        token_increment = (atomic_read(&global_be_token_rate_per_tenant) *
                           time_delta_cycles) /
                          (double)(rte_get_timer_hz());
        be_tokens += (long)(token_increment + 0.5);
        while (nvme_sw_table_isempty(g_nvme_sw_table, fg_handle) == 0 &&
               nvme_sw_table_peak_head_cost(g_nvme_sw_table, fg_handle) <=
                   be_tokens) {
            if (g_outstanding_requests >= g_max_outstanding_requests) {
                break;
            }
            nvme_sw_table_pop_front(g_nvme_sw_table, fg_handle, &ctx);
            issue_nvme_req(ctx);
            be_tokens -= ctx->req_cost;
        }
        if (nvme_sw_table_isempty(g_nvme_sw_table, fg_handle)) count++;

        be_tokens -=
            nvme_sw_table_save_tokens(g_nvme_sw_table, fg_handle, be_tokens);
        // assert(be_tokens >= 0);
    }
    nvme_be_tenant_deactivate(thread_tenant_manager, count);

    if (be_tokens > 0) {
        atomic_u64_fetch_and_add(&global_leftover_tokens, be_tokens);
    }
}

/*
 * update_scheduler_bitvector:
 * 		- synchronizes clearing of the global token bucket to limit
 * global BE token accumulation
 * 		- mark global bitvector to indicate this thread has completed a
 * scheduling round
 * 		- if last thread to complete a round, clear the global vector
 * 		- updates to global vector are not atomic operations because
 * want to limit perf overhead and the exact timing of token bucket reset is
 * not critical, as long as we reset approximately after each thread has had
 * a chance to get tokens
 */
static void update_scheduled_bitvector(void) {
    int i;
    scheduled_bit_vector[RTE_PER_LCORE(cpu_nr)]++;

    for (i = 0; i < cpus_active; i++) {
        if (scheduled_bit_vector[i] == 0) break;
    }
    if (i == cpus_active) {  // all other threads scheduled at least once
        atomic_u64_write(&global_leftover_tokens, 0);

        // clear scheduled bit vector
        for (i = 0; i < cpus_active; i++) {
            scheduled_bit_vector[i] = 0;
        }
    }
}

int nvme_sched(void) {
#ifdef NO_SCHED
    return 0;
#endif
    int round1_ret;

    if (percpu_get(tenant_manager).num_lc_tenants == 0 &&
        percpu_get(tenant_manager).num_be_tenants == 0) {
        percpu_get(last_sched_time) = timer_now();
        percpu_get(last_sched_time_be) = rdtsc();
#ifndef SINGLE_THREADED
        update_scheduled_bitvector();
#endif
        return 0;
    }

    if (g_nvme_sched_mode == LESSv0) {
        // This should fix the starvation issue
        round1_ret =
            nvme_sched_lessv0_subround1();  // roundrobinly serve lc tenants
        if (!round1_ret)
            nvme_sched_subround2();  // roundrobinly serve be tenants
    } else if (g_nvme_sched_mode == LESSv1) {
        // convert the bursty schedule to smooth schedule
        round1_ret = nvme_sched_lessv1_subround1();
        if (!round1_ret) nvme_sched_subround2();
    } else if (g_nvme_sched_mode == LESSv2) {
        // prioritize the tenant with more recent bursts
        round1_ret = nvme_sched_lessv2_subround1();
        if (!round1_ret) nvme_sched_subround2();
    }

    percpu_get(local_leftover_tokens) = 0;
    percpu_get(local_extra_demand) = 0;

#ifndef SINGLE_THREADED
    update_scheduled_bitvector();
#endif

    return 0;
}

void nvme_process_completions() {
    int i;
    int max_completions = 4096;
    int this_completions;

    if (CFG.num_nvmedev == 0 || CFG.ns_sizes[0] != 0) return;

    for (i = 0; i < percpu_get(open_ev_ptr); i++) {
        usys_nvme_opened(percpu_get(open_ev[i]), global_ns_size,
                         global_ns_sector_size);
        percpu_get(received_nvme_completions)++;
    }
    percpu_get(open_ev_ptr) = 0;
    this_completions =
        spdk_nvme_qpair_process_completions(percpu_get(qpair), max_completions);
    g_outstanding_requests -= this_completions;
    percpu_get(received_nvme_completions) += this_completions;
}
