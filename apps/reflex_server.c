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

#include <assert.h>
#include <errno.h>
#include <ixev.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
// #include <ixev_timer.h>
#include <ix/list.h>
#include <ix/mempool.h>
#include <rte_cycles.h>
#include <rte_timer.h>

#include "reflex.h"

#define ROUND_UP(num, multiple) \
    ((((num) + (multiple)-1) / (multiple)) * (multiple))
#define BATCH_DEPTH 512
#define NAMESPACE 0

#define BINARY_HEADER binary_header_blk_t

#define NVME_ENABLE
// #define FINE_MEASURE

#define MAX_PAGES_PER_ACCESS 256  // max 1MB per req
#define PAGE_SIZE 4096

#define MAX_NUM_CONTIG_ALLOC_RETRIES 5
//#define MAX_REQ_COUNT 10 * 1024  // 10s * 1024 IOPS
#define MAX_CONN_COUNT 512
// #define MAX_PORT_RANGE 65535

#define TIMER_RESOLUTION_CYCLES 250ULL /* around 2 us at 125Mhz */

static int outstanding_reqs = 4096 * 64;
static int outstanding_req_bufs = 4096 * 64;  // 4096 * 64;
static unsigned long ns_size;
static unsigned long ns_sector_size = 512;  // use for now

static struct mempool_datastore nvme_req_buf_datastore;
static __thread struct mempool nvme_req_buf_pool;

static struct mempool_datastore nvme_req_datastore;
static __thread struct mempool nvme_req_pool;
static __thread int conn_opened;
static __thread long reqs_allocated = 0;
static __thread unsigned long conn_accepted = 0;
static __thread unsigned long conn_flushed = 0;
#ifdef FINE_MEASURE
static __thread unsigned long measurements[MAX_CONN_COUNT][4][MAX_REQ_COUNT];
#endif
// static __thread unsigned long SLO_handles[MAX_PORT_RANGE];
/*
    unsigned long recv_time;   // from NIC to be processed
    unsigned long nvme_time;   // from parsed to nvme_cb
    unsigned long queue_time;  // from nvme_cb to start sending
    unsigned long send_time;   // from start sending to actual sent
*/
static __thread struct rte_timer
    send_timer;  // only for single connection per thread
static __thread long cycles_between_resend = 100000;
static __thread long failed_header_sents_0 = 0;
static __thread long failed_header_sents_1 = 0;
static __thread long failed_payload_sents_0 = 0;
static __thread long failed_payload_sents_1 = 0;
static __thread long failed_other_sents_0 = 0;
static __thread long failed_other_sents_1 = 0;
// static __thread long successful_resend_attempts = 0;

struct nvme_req {
    struct ixev_nvme_req_ctx ctx;
    unsigned int lba_count;
    unsigned long lba;
    uint16_t opcode;
    struct pp_conn *conn;
    struct list_node link;
    struct ixev_ref ref;  // for zero-copy
    unsigned long timestamp;
    unsigned long service_time;
    void *remote_req_handle;
    char *buf[MAX_PAGES_PER_ACCESS];  // nvme buffer to read/write data into
    int current_sgl_buf;
};

struct pp_conn {
    struct ixev_ctx ctx;
    size_t rx_received;  // bytes received for the current ReFlex request
    size_t tx_sent;
    bool registered;
    bool rx_pending;  // if a ReFlex req is currently being received
    bool tx_pending;
    int nvme_pending;
    long in_flight_pkts;
    long sent_pkts;
    long list_len;
    unsigned long req_measured;
    unsigned long conn_id;
    struct list_head pending_requests;
    long nvme_fg_handle;  // nvme flow group handle
    long conn_fg_handle;  // set for src_port for now
    struct nvme_req *current_req;
    char data_send[sizeof(BINARY_HEADER)];  // use zero-copy for payload
    char data_recv[sizeof(BINARY_HEADER)];  // use zero-copy for payload
};

static struct mempool_datastore pp_conn_datastore;
static __thread struct mempool pp_conn_pool;

static __thread hqu_t handle;
static __thread unsigned long last_sent_time = 0;

static void pp_main_handler(struct ixev_ctx *ctx, unsigned int reason);

static void send_again_cb(__attribute__((unused)) struct rte_timer *tim,
                          void *arg) {
    struct pp_conn *conn = arg;
    int sent_reqs = 0;

    // printf("Thread [%d]: Send again is called back at %lu, %d requests
    // left.\n", rte_lcore_id(), timer_now(), conn->list_len);
    while (!list_empty(&conn->pending_requests)) {
        struct nvme_req *req =
            list_top(&conn->pending_requests, struct nvme_req, link);
        int ret = send_req(req);
        if (!ret) {
            list_pop(&conn->pending_requests, struct nvme_req, link);
            sent_reqs++;
            printf("Got one packet resent.\n");
        } else {
            // printf("Send failed again, but sent %d this time.\n", sent_reqs);
            // if (!sent_reqs) { // stop retrying if nothing more to be sent
            // 	if (rte_timer_reset(tim, cycles_between_resend, SINGLE,
            // rte_lcore_id(), send_again_cb, conn) != 0)
            // rte_exit(EXIT_FAILURE, "Send_again_cb setup failure.\n");
            // }
            return;
        }
    }
    // successful_resend_attempts++;
    // printf("Send succeeded this time.\n");
}

static void send_completed_cb(struct ixev_ref *ref) {
    struct nvme_req *req = container_of(ref, struct nvme_req, ref);
    struct pp_conn *conn = req->conn;
    int i, num4k;

#ifdef FINE_MEASURE
    measurements[conn->conn_id][3][conn->req_measured] =
        timer_now() - req->timestamp;  // send_time
    conn->req_measured++;
    if (conn->req_measured > MAX_REQ_COUNT) {
        fprintf(
            stderr,
            "finished requests for connection-%d\n exceeds logging capability",
            conn->conn_id);
        conn->req_measured = 0;
    }
#endif

    num4k = (req->lba_count * ns_sector_size) / 4096;
    if (((req->lba_count * ns_sector_size) % 4096) != 0) num4k++;
    for (i = 0; i < num4k; i++) {
        mempool_free(&nvme_req_buf_pool, req->buf[i]);
    }

    mempool_free(&nvme_req_pool, req);
    reqs_allocated--;
}

/*
 * returns 0 if send was successfull and -1 if tx path is busy
 */
int send_req(struct nvme_req *req) {
    struct pp_conn *conn = req->conn;
    int ret = 0;
    BINARY_HEADER *header;

    if (!conn->tx_pending) {
        // setup header
        // printf("Now req->opcode is %d\n", req->opcode);
        header = (BINARY_HEADER *)&conn->data_send[0];
        header->magic = sizeof(BINARY_HEADER);  // RESP_PKT;
        header->opcode = req->opcode;
        header->service_time = req->service_time;
        header->resp_code = RESP_OK;
        header->req_handle = req->remote_req_handle;

        assert(header->req_handle);

        while (conn->tx_sent < (sizeof(BINARY_HEADER))) {
            ret = ixev_send(&conn->ctx, &conn->data_send[conn->tx_sent],
                            sizeof(BINARY_HEADER) - conn->tx_sent);
            // if (ret == -ENOBUFS) {
            //     failed_header_sents_0++;
            // } else if (ret == -EAGAIN) {
            //     failed_header_sents_1++;
            // } else if (ret < 0) {
            //     failed_other_sents_0++;
            // }

            if (ret < 0) {
                if (!conn->nvme_pending) {
                    log_err("ixev_send ret < 0, then ivev_close.\n");
                    ixev_close(&conn->ctx);
                    return -2;
                }
                return -1;
                ret = 0;
            }
            conn->tx_sent += ret;
        }

        conn->tx_pending = true;
        conn->tx_sent = 0;
    }
    ret = 0;
    // printf("break point: opcode is %d\n", req->opcode);
    if (req->opcode == CMD_GET) {
        // printf("Sending GET payloads of %d bytes\n", req->lba_count *
        // ns_sector_size);
        while (conn->tx_sent < req->lba_count * ns_sector_size) {
            int to_send =
                min(PAGE_SIZE - (conn->tx_sent % PAGE_SIZE),
                    (req->lba_count * ns_sector_size) - conn->tx_sent);

            ret = ixev_send_zc(
                &conn->ctx,
                &req->buf[req->current_sgl_buf][conn->tx_sent % PAGE_SIZE],
                to_send);
            if (ret < 0) {
                // if (ret == -ENOBUFS) {
                //     failed_payload_sents_0++;
                // } else if (ret == -EAGAIN) {
                //     failed_payload_sents_1++;
                // } else if (ret < 0) {
                //     failed_other_sents_1++;
                // }
                if (!conn->nvme_pending) {
                    log_err("Connection close 3\n");
                    ixev_close(&conn->ctx);
                }

                return -2;
            }
            if (ret == 0) log_err("fhmm ret is zero\n");

            conn->tx_sent += ret;
            if ((conn->tx_sent % PAGE_SIZE) == 0) req->current_sgl_buf++;
        }
        assert(req->current_sgl_buf <= req->lba_count);
        req->ref.cb = &send_completed_cb;
        req->ref.send_pos = req->lba_count * ns_sector_size;
        ixev_add_sent_cb(&conn->ctx, &req->ref);
    } else {  // PUT
        int i, num4k;
        num4k = (req->lba_count * ns_sector_size) / 4096;
        if (((req->lba_count * ns_sector_size) % 4096) != 0) num4k++;
        for (i = 0; i < num4k; i++) {
            mempool_free(&nvme_req_buf_pool, req->buf[i]);
        }
        mempool_free(&nvme_req_pool, req);
        reqs_allocated--;
        // conn->sent_pkts--;
    }
    conn->list_len--;
    conn->tx_sent = 0;
    conn->tx_pending = false;
    return 0;
}

int send_pending_reqs(struct pp_conn *conn) {
    int sent_reqs = 0;

    // if (rte_timer_stop(&send_timer) == -1) // FIXME: bind to the specific
    // conn
    // {
    // 	printf("Core %d: Timer is running", percpu_get(cpu_id));
    // }

    while (!list_empty(&conn->pending_requests)) {
        struct nvme_req *req =
            list_top(&conn->pending_requests, struct nvme_req, link);
        req->service_time = timer_now() - req->timestamp;

        int ret = send_req(req);
        if (!ret) {
#ifdef FINE_MEASURE
            measurements[conn->conn_id][2][conn->req_measured] =
                timer_now() - req->timestamp;  // queue_time
#endif
            sent_reqs++;
            list_pop(&conn->pending_requests, struct nvme_req, link);
        } else {
            // printf("Core %d || Send attempt failed (sent: %d/list_len:
            // %d/in_flight: %d/sent_pkts: %d).\n", percpu_get(cpu_id),
            // sent_reqs, conn->list_len, conn->in_flight_pkts,
            // conn->sent_pkts); exit(-1); failed_resend_attempts++;
            // rte_timer_init(&send_timer); // FIXME: bind to the specific conn
            // // printf("Init a timer for thread %d\n", rte_lcore_id());
            // if (rte_timer_reset(&send_timer, cycles_between_resend,
            // PERIODICAL, rte_lcore_id(), send_again_cb, conn) != 0)
            // 	rte_exit(EXIT_FAILURE, "Send_again_cb setup failure.\n");
            return sent_reqs;
        }
    }
    return sent_reqs;
}

static void nvme_written_cb(struct ixev_nvme_req_ctx *ctx,
                            unsigned int reason) {
    struct nvme_req *req = container_of(ctx, struct nvme_req, ctx);
    struct pp_conn *conn = req->conn;
    /*
        int num_bytes = req->lba_count * 512;
        int num_4kbufs = num_bytes /4096 + 1;
        printf("\n***WRITTEN: num_bytes %d, lba_count %u \n", num_bytes,
    req->lba_count); int i, j; for (i =0; i < num_4kbufs; i++){ for (j=0; j <
    4096; j++){ if (num_bytes > i*4096 + j) {
                                //printf("%x ", *(req->buf[i]+j));
                                printf("%c", *(req->buf[i]+j));
                        }
                }
        }
        printf("\n");
        */
    conn->list_len++;
    conn->in_flight_pkts--;
    conn->sent_pkts++;
#ifdef FINE_MEASURE
    measurements[conn->conn_id][1][conn->req_measured] =
        timer_now() - req->timestamp;  // nvme_time
#endif
    list_add_tail(&conn->pending_requests, &req->link);
    send_pending_reqs(conn);
    return;
}

static void nvme_response_cb(struct ixev_nvme_req_ctx *ctx,
                             unsigned int reason) {
    struct nvme_req *req = container_of(ctx, struct nvme_req, ctx);
    struct pp_conn *conn = req->conn;

    /*
        int num_bytes = req->lba_count * 512;
        int num_4kbufs = num_bytes /4096 + 1;
        printf("\n****READ: num_bytes %d, lba_count %u \n", num_bytes,
    req->lba_count); int i, j; for (i =0; i < num_4kbufs; i++){ for (j=0; j <
    4096; j++){ if (num_bytes > i*4096 + j) {
                                //printf("%x ", *(req->buf[i]+j));
                                printf("%c-", *(req->buf[i]+j));
                        }
                        else{
                                printf("\n i is %d, j %d\n", i, j);
                                break;
                        }
                }
        }
*/
    conn->list_len++;
    conn->in_flight_pkts--;
    conn->sent_pkts++;
#ifdef FINE_MEASURE
    measurements[conn->conn_id][1][conn->req_measured] =
        timer_now() - req->timestamp;  // nvme_time
#endif
    list_add_tail(&conn->pending_requests, &req->link);
    send_pending_reqs(conn);
    return;
}

static void nvme_opened_cb(hqu_t _handle, unsigned long _ns_size,
                           unsigned long _ns_sector_size) {
    ns_size = _ns_size;
    ns_sector_size = _ns_sector_size;
    if (ns_size) {
        handle = _handle;
    }
}

static void nvme_registered_flow_cb(long fg_handle, struct ixev_ctx *ctx,
                                    long ret) {
    if (ret < 0) {
        log_err("ERROR: couldn't register flow\n");
        // probably signifies you need a less strict SLO
    }

    struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
    conn->nvme_fg_handle = fg_handle;
    conn->registered = true;
    // printf("nvme fg_handle is %d.\n", fg_handle);

    BINARY_HEADER *header;

    header = (BINARY_HEADER *)&conn->data_send[0];
    header->magic = sizeof(BINARY_HEADER);  // RESP_PKT;
    header->opcode = CMD_REG;
    header->flow_handle = ctx;  // support control plane flow management
    header->token.supply = 0;
    header->token.assigned = 0;
    header->resp_code = RESP_OK;  // CMD_REG

    while (conn->tx_sent < (sizeof(BINARY_HEADER))) {
        ret = ixev_send(&conn->ctx, &conn->data_send[conn->tx_sent],
                        sizeof(BINARY_HEADER) - conn->tx_sent);

        if (ret < 0) {
            if (!conn->nvme_pending) {
                log_err("ixev_send ret < 0, then ivev_close.\n");
                ixev_close(&conn->ctx);
                return -2;
            }
            return -1;
            ret = 0;
        }
        conn->tx_sent += ret;
    }
    conn->tx_sent = 0;
}

static void nvme_unregistered_flow_cb(long flow_group_id, long ret) {
    if (ret) {
        log_err("ERROR: couldn't unregister flow\n");
    }
}

static struct ixev_nvme_ops nvme_ops = {
    .opened = &nvme_opened_cb,
    .registered_flow = &nvme_registered_flow_cb,
    .unregistered_flow = &nvme_unregistered_flow_cb,
};

static void receive_req(struct pp_conn *conn) {
    ssize_t ret;
    struct nvme_req *req;
    BINARY_HEADER *header;
    void *nvme_addr;

    while (1) {
        int num4k;
        if (!conn->rx_pending) {
            int i;

            ret = ixev_recv(&conn->ctx, &conn->data_recv[conn->rx_received],
                            sizeof(BINARY_HEADER) - conn->rx_received);
            if (ret <= 0) {
                if (ret != -EAGAIN) {
                    if (!conn->nvme_pending) {
                        log_err("Connection close 6\n");
                        ixev_close(&conn->ctx);
                    }
                }
                return;
            } else
                conn->rx_received += ret;

            if (conn->rx_received < sizeof(BINARY_HEADER)) return;

            // received the header
            header = (BINARY_HEADER *)&conn->data_recv[0];

            if (header->magic != sizeof(BINARY_HEADER))
                printf(
                    "The stored magic(%d) is not as expected(%d). cpu_nr: %d, "
                    "cpu_id: %d\n",
                    header->magic, sizeof(BINARY_HEADER), percpu_get(cpu_nr),
                    percpu_get(cpu_id));
            assert(header->magic == sizeof(BINARY_HEADER));

            // rely on client-side register, otherwise we need drop initial pkts
            if (header->opcode == CMD_REG) {
                if (header->token_demand == 0) {
                    unsigned long cookie = (unsigned long)&conn->ctx;
                    slo_t SLO = header->SLO;
                    uint32_t latency_SLO = SLO.latency_SLO_hi << 16;
                    latency_SLO += SLO.latency_SLO_lo;
                    conn->conn_fg_handle = header->SLO_val;  // update handle

                    ixev_nvme_register_flow(header->SLO_val, cookie,
                                            latency_SLO, SLO.IOPS_SLO,
                                            SLO.rw_ratio_SLO);
                } else {
                    // Update token demands from clients
                    // TODO: add a function call for flow adjustment
                    unsigned long cookie = (unsigned long)header->flow_handle;
                    // ixev_adjust_flow();
                }

                conn->rx_received = 0;
                continue;
            }

            // allocate nvme req
            conn->current_req = mempool_alloc(&nvme_req_pool);
            conn->current_req->timestamp = timer_now();
            if (!conn->current_req) {
                printf(
                    "Cannot allocate nvme_usr req. In flight requests: %lu "
                    "sent req %lu . list len %lu \n",
                    conn->in_flight_pkts, conn->sent_pkts, conn->list_len);
                return;
            }
            conn->current_req->current_sgl_buf = 0;

            // allocate lba_count sector sized nvme bufs
            num4k = (header->lba_count * ns_sector_size) / 4096;
            assert(num4k <= MAX_PAGES_PER_ACCESS);
            if (((header->lba_count * ns_sector_size) % 4096) != 0) num4k++;

            for (i = 0; i < num4k; i++) {
                conn->current_req->buf[i] = mempool_alloc(&nvme_req_buf_pool);
                if (conn->current_req->buf[i] == NULL) {
                    printf("ERROR: alloc of nvme_req_buf failed\n");
                    assert(0);
                }
                // printf("conn->current_req->buf[%d] is %p, expect next %x\n",
                //        i, conn->current_req->buf[i],
                //        (uint64_t)(conn->current_req->buf[i]) - 4096);
            }
            // printf("req buf is %p\n", conn->current_req->buf[0]);

            ixev_nvme_req_ctx_init(&conn->current_req->ctx);

            reqs_allocated++;
            conn->rx_pending = true;
            conn->rx_received = 0;
        }

        req = conn->current_req;
        header = (BINARY_HEADER *)&conn->data_recv[0];

        assert(header->magic == sizeof(BINARY_HEADER));

        if (header->opcode == CMD_SET) {
            while (conn->rx_received < header->lba_count * ns_sector_size) {
                int to_receive = min(
                    PAGE_SIZE - (conn->rx_received % PAGE_SIZE),
                    (header->lba_count * ns_sector_size) - conn->rx_received);

                ret = ixev_recv(&conn->ctx,
                                &req->buf[req->current_sgl_buf]
                                         [conn->rx_received % PAGE_SIZE],
                                to_receive);

                if (ret < 0) {
                    if (ret == -EAGAIN) return;

                    if (!conn->nvme_pending) {
                        printf("Connection close 3\n");
                        ixev_close(&conn->ctx);
                    }
                    return;
                }

                conn->rx_received += ret;
                if ((conn->rx_received % PAGE_SIZE) == 0) {
                    req->current_sgl_buf++;
                }
            }
            // 4KB sgl bufs should match number of 512B sectors
            assert(req->current_sgl_buf <= header->lba_count * 8);
        } else if (header->opcode == CMD_GET) {
        } else {
            printf("Received unsupported command, closing connection\n");
            ixev_close(&conn->ctx);
            return;
        }

        req->opcode = header->opcode;
        req->lba_count = header->lba_count;
        req->lba = header->lba;
        req->remote_req_handle = header->req_handle;

        req->ctx.handle = handle;
        req->conn = conn;

        nvme_addr = (void *)(header->lba << 9);
        if (nvme_addr >= ns_size) {
            printf("nvme_addr: %lu is larger than ns_size: %lu.\n",
                   (unsigned long)nvme_addr, ns_size);
        }
        assert((unsigned long)nvme_addr < ns_size);

        conn->in_flight_pkts++;
        num4k = (header->lba_count * ns_sector_size) / PAGE_SIZE;
        if (((header->lba_count * ns_sector_size) % PAGE_SIZE) != 0) num4k++;

#ifdef FINE_MEASURE
        measurements[conn->conn_id][0][conn->req_measured] =
            timer_now() - req->timestamp;  // recv_time
#endif
        switch (header->opcode) {
            case CMD_SET:
                ixev_set_nvme_handler(&req->ctx, IXEV_NVME_WR,
                                      &nvme_written_cb);
#ifndef NVME_ENABLE
                nvme_written_cb(&req->ctx, IXEV_NVME_WR);
#else
                // ixev_nvme_write(conn->nvme_fg_handle, req->buf[0],
                // header->lba, header->lba_count, (unsigned long)&req->ctx);
                ixev_nvme_writev(conn->nvme_fg_handle, (void **)&req->buf[0],
                                 num4k, header->lba, header->lba_count,
                                 (unsigned long)&req->ctx);
#endif
                conn->nvme_pending++;
                break;
            case CMD_GET:
                ixev_set_nvme_handler(&req->ctx, IXEV_NVME_RD,
                                      &nvme_response_cb);
#ifndef NVME_ENABLE
                // printf("Received GET msg - early reply\n");
                nvme_response_cb(&req->ctx, IXEV_NVME_RD);
#else
                // ixev_nvme_read(conn->nvme_fg_handle, req->buf[0],
                // header->lba, header->lba_count, (unsigned long)&req->ctx);
                ixev_nvme_readv(conn->nvme_fg_handle, (void **)&req->buf[0],
                                num4k, header->lba, header->lba_count,
                                (unsigned long)&req->ctx);
#endif
                conn->nvme_pending++;
                break;
            case CMD_REG:
                log_err("Should not see this choice here.\n");
                break;
            default:
                printf("Received illegal msg (opcode-%d) - dropping msg\n",
                       header->opcode);
                mempool_free(&nvme_req_pool, req);
                reqs_allocated--;
        }
        conn->rx_received = 0;
        conn->rx_pending = false;
    }
}

static void pp_main_handler(struct ixev_ctx *ctx, unsigned int reason) {
    struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);

    // Lets always try to send
    if (true || reason == IXEVOUT) {
        send_pending_reqs(conn);
    }
    if (reason == IXEVHUP) {
        ixev_nvme_unregister_flow(conn->nvme_fg_handle);
        if (conn->sent_pkts > 0) {
            printf("Thread %d: IXEVHUP: Connection closed.\n",
                   percpu_get(cpu_id));
            // printf(
            //     "Failed sent: header - %lu/%lu | payload - %lu/%lu | others -
            //     "
            //     "%lu/%lu\n",
            //     failed_header_sents_0, failed_header_sents_1,
            //     failed_payload_sents_0, failed_payload_sents_1,
            //     failed_other_sents_0, failed_other_sents_1);
        }

        // failed_header_sents_0 = failed_header_sents_1 = 0;
        // failed_payload_sents_0 = failed_payload_sents_1 = 0;
        // failed_other_sents_0 = failed_header_sents_1 = 0;
        // failed_resend_attempts = 0;
        // successful_resend_attempts = 0;
        ixev_close(&conn->ctx);
        return;
    }
    receive_req(conn);
}

static struct ixev_ctx *pp_accept(struct ip_tuple *id) {
    struct pp_conn *conn = mempool_alloc(&pp_conn_pool);
    if (!conn) {
        printf("MEMPOOL ALLOC FAILED !\n");
        return NULL;
    }
    list_head_init(&conn->pending_requests);
    conn->rx_received = 0;
    conn->rx_pending = false;
    conn->tx_sent = 0;
    conn->tx_pending = false;
    conn->in_flight_pkts = 0x0UL;
    conn->sent_pkts = 0x0UL;
    conn->list_len = 0x0UL;
    conn->req_measured = 0;
    ixev_ctx_init(&conn->ctx);
    ixev_set_handler(&conn->ctx, IXEVIN | IXEVOUT | IXEVHUP, &pp_main_handler);
    conn_opened++;

    conn->nvme_fg_handle = 0;
    conn->conn_id = conn_accepted;
    conn->registered = false;

    printf("pp_accept: id-%d, src-%d.%d.%d.%d:%d dst-%d\n", conn_accepted,
           id->src_ip >> 24, (id->src_ip << 8) >> 24, (id->src_ip << 16) >> 24,
           (id->src_ip << 24) >> 24, id->src_port, id->dst_port);
    conn_accepted++;
    if (conn_accepted > MAX_CONN_COUNT) {
        conn_accepted -= MAX_CONN_COUNT;
    }

    return &conn->ctx;
}

static void pp_release(struct ixev_ctx *ctx) {
    struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
    FILE *f;

    conn_opened--;
    if (conn_opened == 0) {
        // flush
#ifdef FINE_MEASURE
        f = fopen("stats.txt", "a");
        for (int i = conn_flushed; i < conn_accepted + 1; i++) {
            fprintf(f, "Connection-%d\n", i);
            for (int j = 0; j < MAX_REQ_COUNT; j++) {
                fprintf(f, "%ld %ld %ld %ld\n", measurements[i][0][j],
                        measurements[i][1][j], measurements[i][2][j],
                        measurements[i][3][j]);
            }
        }
        fclose(f);
#endif
        conn_flushed = conn_accepted + 1;
    }

    mempool_free(&pp_conn_pool, conn);
}

static struct ixev_conn_ops pp_conn_ops = {
    .accept = &pp_accept,
    .release = &pp_release,
};

static pthread_mutex_t launch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t launch_cond = PTHREAD_COND_INITIALIZER;

struct launch_req {
    struct launch_req *next;
};

static struct launch_req *launch_reqs;

void *pp_main(void *arg) {
    int ret;
    conn_opened = 0;
    printf("pp_main on cpu %d, thread self is %x\n", percpu_get(cpu_nr),
           pthread_self());
    struct launch_req *req;
    uint64_t prev_tsc = 0, cur_tsc, diff_tsc;

    ret = ixev_init_thread();
    if (ret) {
        fprintf(stderr, "unable to init IXEV\n");
        return NULL;
    };

    ret = mempool_create(&nvme_req_pool, &nvme_req_datastore,
                         MEMPOOL_SANITY_GLOBAL, 0);
    if (ret) {
        fprintf(stderr, "unable to create mempool\n");
        return NULL;
    }

    ret = mempool_create(&nvme_req_buf_pool, &nvme_req_buf_datastore,
                         MEMPOOL_SANITY_GLOBAL, 0);
    if (ret) {
        fprintf(stderr, "unable to create mempool\n");
        return NULL;
    }

    ret = mempool_create(&pp_conn_pool, &pp_conn_datastore,
                         MEMPOOL_SANITY_GLOBAL, 0);
    if (ret) {
        fprintf(stderr, "unable to create mempool\n");
        return NULL;
    }

    ixev_nvme_open(NAMESPACE, 1);

    printf("%lu cycles / seconds, will resend responses after %lu cycles\n",
           rte_get_timer_hz(), cycles_between_resend);

    while (1) {
        ixev_wait();
        // cur_tsc = timer_now();
        // diff_tsc = cur_tsc - prev_tsc;
        // if (diff_tsc > TIMER_RESOLUTION_CYCLES) {
        // 	rte_timer_manage();
        // 	prev_tsc = cur_tsc;
        // }
    }

    return NULL;
}

int reflex_server_main(int argc, char *argv[]) {
    int i, nr_cpu;
    pthread_t tid;
    int ret;
    unsigned int pp_conn_pool_entries;

    nr_cpu = cpus_active;  // sys_nrcpus();
    if (nr_cpu < 1) {
        fprintf(stderr, "got invalid cpu count %d\n", nr_cpu);
        exit(-1);
    }

    ret =
        mempool_create_datastore(&nvme_req_datastore, outstanding_reqs,
                                 sizeof(struct nvme_req), "nvme_req_datastore");
    if (ret) {
        fprintf(stderr, "unable to create datastore\n");
        return ret;
    }

    pp_conn_pool_entries = ROUND_UP(16 * 4096, MEMPOOL_DEFAULT_CHUNKSIZE);

    ixev_init_conn_nvme(&pp_conn_ops, &nvme_ops);
    if (ret) {
        fprintf(stderr, "failed to initialize ixev nvme\n");
        return ret;
    }
    ret = mempool_create_datastore(&pp_conn_datastore, pp_conn_pool_entries,
                                   sizeof(struct pp_conn), "pp_conn");
    if (ret) {
        fprintf(stderr, "unable to create mempool\n");
        return ret;
    }

    ret = mempool_create_datastore_align(&nvme_req_buf_datastore,
                                         outstanding_req_bufs, PAGE_SIZE,
                                         "nvme_req_buf_datastore");

    if (ret) {
        fprintf(stderr, "unable to create datastore\n");
        return ret;
    }

    if (remove("stats.txt") == 0)
        log_info("Deleted last stats successfully\n");
    else
        log_info("Unable to delete the stats\n");

    for (i = 1; i < nr_cpu; i++) {
        // ret = pthread_create(&tid, NULL, start_cpu, (void *)(unsigned long)
        // i);
        log_info("rte_eal_remote_launch...pp_main\n");
        ret = rte_eal_remote_launch(pp_main, (void *)(unsigned long)i, i);

        if (ret) {
            log_err("init: unable to start app\n");
            return -EAGAIN;
        }
    }

    log_info("Started ReFlex server...\n");
    pp_main(NULL);
    return 0;
}
