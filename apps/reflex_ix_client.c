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
#include <fcntl.h>
#include <ix/cfg.h>
#include <ix/list.h>
#include <ix/mempool.h>
// #include <ix/timer.h>
#include <ixev.h>
#include <netinet/in.h>
#include <pthread.h>
#include <reflex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <rte_timer.h>

#define BINARY_HEADER binary_header_blk_t

#define ROUND_UP(num, multiple) \
    ((((num) + (multiple)-1) / (multiple)) * (multiple))

#define MAKE_IP_ADDR(a, b, c, d)                                      \
    (((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | \
     (uint32_t)d)

#define MAX_SECTORS_PER_ACCESS 64
#define MAX_LATENCY 2000
#define MAX_IOPS 2000000
#define NUM_TESTS 8
#define DURATION 1
#define MAX_NUM_MEASURE MAX_IOPS *DURATION
#define PAGE_SIZE 4096
#define MAX_PAGES_PER_ACCESS 256

static const unsigned long sweep[NUM_TESTS] = {100000, 110000, 120000, 130000,
                                               132000, 134000, 136000, 138000};

// FIXEME: hard-coding sector size for now
static int ns_sector_size = 512;
static int log_ns_sector_size = 9;
// static long ns_size = 0xE8E0DB6000;	// Samsung a801
// static long ns_size = 0x37E3EE56000;	// Samsung 1721
// static long ns_size = 0x1749a956000;    // Samsung 1725
// static long ns_size = 0x5d27216000;  // Intel P3600 400GB capacity

static struct mempool_datastore nvme_usr_datastore;
static const int outstanding_reqs = 4096 * 8;
static volatile int started_conns = 0;
static pthread_barrier_t barrier;
static int req_size = 2;  // in lbas, 1024 bytes
static volatile int nr_threads = 1;
static unsigned long iops = 0;
static int sequential = 0;
// static int verify = 0;  // read write verification
struct ip_tuple *ip_tuple[64];
static int read_percentage = 100;
static int SWEEP = 1;
static bool preconditioning = 0;
static unsigned long global_target_IOPS = 50000;
static int qdepth = 0;
static int run_time = 0;
time_t start_time;
time_t curr_time;
static int port = -1;
static char *ip = NULL;

static __thread struct mempool req_pool;
static __thread int conn_opened;
static __thread unsigned long last_send = 0;
static __thread unsigned long bench_start = 0;
static __thread unsigned long avg = 0;
static __thread unsigned long max = 0;
static __thread unsigned long measure = 0;
static __thread unsigned long failed_alloc_reqs = 0;
static __thread unsigned long num_measured_reads = 0;
static __thread long sent = 0;
// static __thread long sent_batch = 0;
static __thread unsigned long measurements[MAX_LATENCY];
static __thread unsigned long missed_sends = 0;
static __thread bool running = false;
static __thread bool terminate = false;
static __thread int run = 0;
static __thread int tid;
static __thread long cycles_between_req;
static __thread unsigned long phase_start;
static __thread long NUM_MEASURE;
static __thread char *last_req_buf = NULL;  // for verification

static struct mempool_datastore nvme_req_buf_datastore;
static __thread struct mempool nvme_req_buf_pool;

static inline uint32_t intlog2(const uint32_t x) {
    uint32_t y;
    y = log(x) / log(2);
    // asm ( "\tbsr %1, %0\n"
    //       : "=r"(y)
    //       : "r" (x)
    // 	);
    return y;
}

int compare_ul(const void *a, const void *b) {
    const unsigned long *da = (const unsigned long *)a;
    const unsigned long *db = (const unsigned long *)b;

    return (*da > *db) - (*da < *db);
}

long get_percentile(unsigned long m[], unsigned long num_measure,
                    unsigned long percentile) {
    unsigned int i = 0;
    unsigned long tmp = 0;

    while (tmp < (num_measure * percentile) / 100 && i < MAX_LATENCY) {
        tmp += m[i];
        i++;
    }
    return i;
}

struct nvme_req {
    uint8_t cmd;
    unsigned long lba;
    unsigned int lba_count;
    struct ixev_nvme_req_ctx ctx;
    size_t size;
    struct pp_conn *conn;
    struct ixev_ref ref;  // for zero-copy
    struct list_node link;
    unsigned long sent_time;
    void *remote_req_handle;
    char *buf[MAX_PAGES_PER_ACCESS];  // nvme buffer to read/write data into
    int current_sgl_buf;
};

struct pp_conn {
    struct ixev_ctx ctx;
    size_t rx_received;  // the amount of data received/sent for the current
                         // ReFlex request
    size_t tx_sent;
    bool rx_pending;  // is there a ReFlex req currently being received/sent
    bool tx_pending;
    bool alive;
    int nvme_pending;
    long in_flight_pkts;
    long sent_pkts;
    long list_len;
    // bool receive_loop;
    unsigned long last_count;  // aka seq_count when verify = 0
    struct list_head pending_requests;
    long nvme_fg_handle;  // nvme flow group handle
    char data_send[sizeof(BINARY_HEADER)];
    char data_recv[sizeof(BINARY_HEADER)];
};

static struct mempool_datastore pp_conn_datastore;
static __thread struct mempool pp_conn_pool;

static int parse_ip_addr(const char *str, uint32_t *addr) {
    unsigned char a, b, c, d;

    if (sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return -EINVAL;

    *addr = MAKE_IP_ADDR(a, b, c, d);
    return 0;
}

static void receive_req(struct pp_conn *conn) {
    ssize_t ret;
    struct nvme_req *req;
    BINARY_HEADER *header;
    int measure_cond, report_cond, terminate_cond;
    int i;
    int num4k = num4k = (req_size * ns_sector_size) / 4096;
    if (((req_size * ns_sector_size) % 4096) != 0) num4k++;

    while (1) {
        if (!conn->rx_pending) {
            ret = ixev_recv(&conn->ctx, &conn->data_recv[conn->rx_received],
                            sizeof(BINARY_HEADER) - conn->rx_received);
            if (ret <= 0) {
                if (ret != -EAGAIN) {
                    if (!conn->nvme_pending) {
                        printf("Connection close 6\n");
                        if (conn->alive) {
                            ixev_close(&conn->ctx);
                            conn->alive = false;
                        }
                    }
                }
                break;
            } else {
                conn->rx_received += ret;
            }
            if (conn->rx_received < sizeof(BINARY_HEADER))
                return;
            else  // received the header
                conn->rx_received = 0;
        }

        // received the header && remaining payload bytes
        conn->rx_pending = true;
        header = (BINARY_HEADER *)&conn->data_recv[0];
        assert(header->magic == sizeof(BINARY_HEADER));

        if (!running) {
            assert(header->opcode == CMD_REG);
            if (header->lba_count == RESP_OK) {
                printf("Registration accepted.\n");
                running = true;
            } else {
                printf("Registration rejected.\n");
                ixev_close(&conn->ctx);
                conn->alive = false;
            }
            conn->rx_pending = false;
            conn->rx_received = 0;
            break;
        }

        req = header->req_handle;  // NVMe Req Handler
        assert(req);

        if (header->opcode == CMD_GET) {
            while (conn->rx_received < req_size * ns_sector_size) {
                // printf("Entering recv while, conn->rx_received is %d.\n",
                // conn->rx_received);
                int to_receive =
                    min(PAGE_SIZE - (conn->rx_received % PAGE_SIZE),
                        req_size * ns_sector_size - conn->rx_received);
                // printf("To receive is %d, offset is %d\n", to_receive,
                // conn->rx_received % PAGE_SIZE); printf("----------- at %p &
                // %p\n", req->buf[0], req->buf[1]); printf("-----------
                // req->current_sgl_buf is %d\n", req->current_sgl_buf);
                // printf("----------- at %p\n",
                // &req->buf[req->current_sgl_buf][conn->rx_received %
                // PAGE_SIZE]);
                ret = ixev_recv(&conn->ctx,
                                &req->buf[req->current_sgl_buf]
                                         [conn->rx_received % PAGE_SIZE],
                                to_receive);
                // printf("Ret is %d.\n", ret);
                if (ret <= 0) {
                    if (ret != -EAGAIN) {
                        assert(0);
                        if (!conn->nvme_pending) {
                            printf("Connection close 7\n");
                            if (conn->alive) {
                                ixev_close(&conn->ctx);
                                conn->alive = false;
                            }
                        }
                    }
                    return;
                }

                conn->rx_received += ret;
                if ((conn->rx_received % PAGE_SIZE) == 0) {
                    req->current_sgl_buf++;
                }

                //                 if (verify) {  // not working with 4k version
                //                     if (strncmp(&req->buf[0],
                //                                 last_req_buf,
                //                                 req_size * ns_sector_size) ==
                //                                 0) {
                // #ifdef CLI_DEBUG
                //                         printf("Request %d: verification
                //                         passed (@%p).\n", sent,
                //                         last_req_buf);
                // #endif
                //                     } else {
                //                         printf("Request %d: got different
                //                         data (@%p) from the server.\n", sent,
                //                         last_req_buf);
                //                     }
                //                 }
            }
            assert(req->current_sgl_buf <= header->lba_count * 8);
        } else if (header->opcode == CMD_SET) {
        } else if (header->opcode == CMD_REG) {
            printf("Got CMD_REG again.\n");
            return;
        }
        else {
            printf("Received unsupported command %d, closing connection\n", header->opcode);
            if (conn->alive) {
                ixev_close(&conn->ctx);
                conn->alive = false;
            }
            return;  // exception
        }

        if (!SWEEP && qdepth) {
            measure_cond = 1;  // no sweeping and close-loop
        } else {
            measure_cond =
                measure >= NUM_MEASURE &&
                measure < NUM_MEASURE * 2;  // sweep or open loop, reach a
                                            // mesure more than limit
            // measure_cond = false;
        }
        // if (req->cmd == CMD_GET) { //only report read latency (not write)
        if (measure_cond) {
            // unsigned long now = rdtsc();
            unsigned long latency = (rdtsc() - req->sent_time) * 1E6 / rte_get_timer_hz();
            if (latency >= MAX_LATENCY)
                measurements[MAX_LATENCY - 1]++;
            else
                measurements[latency]++;

            avg += latency;
            if (latency > max) max = latency;

            num_measured_reads++;
        }
        //}
        measure++;
        req->sent_time = 0;

        //         if (verify && header->opcode == CMD_SET) {
        //             // save last buf
        //             if (last_req_buf != NULL) {
        // #ifdef CLI_DEBUG
        //                 if (strncmp(req->buf,
        //                             last_req_buf,
        //                             req_size * ns_sector_size) == 0) {
        //                     printf("WARNING: writing same data (@%p) with
        //                     last request (@%p).\n", req->buf,
        //                     last_req_buf);
        //                 }
        // #endif
        //                 for (i = 0; i < num4k; i++) {
        //                     mempool_free(&nvme_req_buf_pool,
        //                     req->buf[i]);
        //                 }
        //             }
        //             last_req_buf = req->buf;
        //         } else {
        for (i = 0; i < num4k; i++) {
            mempool_free(&nvme_req_buf_pool, req->buf[i]);
        }
        // }
        mempool_free(&req_pool, req);

        conn->rx_pending = false;
        conn->rx_received = 0;

        if (!SWEEP && qdepth) {
            time(&curr_time);
            report_cond = difftime(curr_time, start_time) > run_time &&
                          tid == nr_threads - 1 && num_measured_reads != 0;
            terminate_cond = difftime(curr_time, start_time) > run_time;
        } else {
            report_cond = measure == NUM_MEASURE * 2 &&
                          num_measured_reads != 0;  //&& tid == nr_threads-1;
            terminate_cond = measure == NUM_MEASURE * 3;
            assert(measure < NUM_MEASURE * 3 + 1);
        }

        if (report_cond) {
            unsigned long usecs = 1000UL * 1000UL;
            unsigned long target_IOPS, guide_IOPS;

            if (!qdepth) {
                assert(measure <= MAX_NUM_MEASURE + NUM_MEASURE);
                assert(num_measured_reads <= NUM_MEASURE);
            }

            if (SWEEP) {
                target_IOPS = sweep[run];
            } else {
                target_IOPS = global_target_IOPS;
            }

            guide_IOPS = nr_threads * (NUM_MEASURE * usecs) * 1E6 /
                         ((rdtsc() - phase_start) / rte_get_timer_hz());
            printf(
                "RqIOPS:\t IOPS:\t Avg:\t 10th:\t 20th:\t 30th:\t 40th:\t "
                "50th:\t 60th:\t 70th:\t 80th:\t 90th:\t 95th:\t 99th:\t "
                "max:\t missed:\n");
            printf(
                "%lu\t %lu\t %lu\t %lu\t %lu\t %lu\t %lu\t %lu\t %lu\t "
                "%lu\t "
                "%lu\t %lu\t %lu\t %lu\t %lu\t %lu\n",
                target_IOPS, guide_IOPS, avg / num_measured_reads,
                get_percentile(measurements, num_measured_reads, 10),
                get_percentile(measurements, num_measured_reads, 20),
                get_percentile(measurements, num_measured_reads, 30),
                get_percentile(measurements, num_measured_reads, 40),
                get_percentile(measurements, num_measured_reads, 50),
                get_percentile(measurements, num_measured_reads, 60),
                get_percentile(measurements, num_measured_reads, 70),
                get_percentile(measurements, num_measured_reads, 80),
                get_percentile(measurements, num_measured_reads, 90),
                get_percentile(measurements, num_measured_reads, 95),
                get_percentile(measurements, num_measured_reads, 99), max,
                missed_sends);
            run++;
            if ((double)target_IOPS / guide_IOPS > 2) {
                printf(
                    "Got weird IOPS, the num of measured read is %lu, the "
                    "measure is %lu, the sent is %lu.\n",
                    num_measured_reads, measure, sent);
            }
        }

        if (terminate_cond) {
#ifdef CLI_DEBUG
            printf("CPU %d: debug: terminating\n", percpu_get(cpu_nr));
#endif
            if (!qdepth) assert(sent == NUM_MEASURE * 3);
            terminate = true;
            measure = 0;
            num_measured_reads = 0;
            avg = 0;
            max = 0;
            sent = 0;
            missed_sends = 0;
            int i;
            for (i = 0; i < MAX_LATENCY; i++) {
                measurements[i] = 0;
            }
        }

        if (qdepth) {
            // close loop: send another request
            send_handler(&conn->ctx, 1);
        }
    }
}

/*
 * returns 0 if send was successfull and -1 if tx path is busy
 */
int send_client_req(struct nvme_req *req) {
    struct pp_conn *conn = req->conn;
    int ret = 0;
    BINARY_HEADER *header;

    assert(conn);

    if (!conn->tx_pending) {
        // setup header
        header = (BINARY_HEADER *)&conn->data_send[0];
        header->magic = sizeof(BINARY_HEADER);
        header->opcode = req->cmd;
        header->lba = req->lba;
        header->lba_count = req->lba_count;
        header->req_handle = req;

        while (conn->tx_sent < sizeof(BINARY_HEADER)) {
            ret = ixev_send(&conn->ctx, &conn->data_send[conn->tx_sent],
                            sizeof(BINARY_HEADER) - conn->tx_sent);
            if (ret == -EAGAIN || ret == -ENOBUFS) {
                printf("Send buf failed -1. %d.\n", ret);
                return -1;
            } else if (ret < 0) {
                printf("ixev_send - ret is %d.\n", ret);
                if (!conn->nvme_pending) {
                    printf("[%d] Connection close 2\n", percpu_get(cpu_id));
                    if (conn->alive) {
                        ixev_close(&conn->ctx);
                        conn->alive = false;
                    }
                }
                return -2;
                ret = 0;
            }
            conn->tx_sent += ret;
        }
        if (conn->tx_sent != sizeof(BINARY_HEADER)) {
            printf("tx_sent is %d, header is %d, last ret is %d.\n",
                   conn->tx_sent, sizeof(BINARY_HEADER), ret);
        }
        assert(conn->tx_sent == sizeof(BINARY_HEADER));
        conn->tx_pending = true;
        conn->tx_sent = 0;
    }
    ret = 0;
    // send request first, then send write data
    if (req->cmd == CMD_SET) {
        while (conn->tx_sent < req->lba_count * ns_sector_size) {
            assert(req->lba_count * ns_sector_size);
            int to_send = min(PAGE_SIZE - (conn->tx_sent % PAGE_SIZE),
                              req_size * ns_sector_size - conn->tx_sent);
            ret = ixev_send_zc(
                &conn->ctx,
                &req->buf[req->current_sgl_buf][conn->tx_sent % PAGE_SIZE],
                to_send);
            if (ret < 0) {
                if (ret == -EAGAIN || ret == -ENOBUFS) {
                    printf("Send buf failed -2. %d.\n", ret);
                    return -2;
                }
                if (!conn->nvme_pending) {
                    printf("Connection close 3\n");
                    if (conn->alive) {
                        ixev_close(&conn->ctx);
                        conn->alive = false;
                    }
                }
                return -2;
            }
            if (ret == 0) printf("fhmm ret is zero\n");

            conn->tx_sent += ret;
            if ((conn->tx_sent % PAGE_SIZE) == 0) {
                req->current_sgl_buf++;
            }
        }
    }
    conn->tx_sent = 0;
    conn->tx_pending = false;
    return 0;
}

int send_pending_client_reqs(struct pp_conn *conn) {
    int sent_reqs = 0;
    // int chksum = conn->list_len;

    while (!list_empty(&conn->pending_requests)) {
        int ret;
        // assert(sent_reqs+conn->list_len == chksum); // checkpoint @4
        struct nvme_req *req =
            list_top(&conn->pending_requests, struct nvme_req, link);
        // assert(req->conn); // checkpoint @5
        // assert(conn == req->conn); // checkpoint @6
        req->sent_time = rdtsc();
        ret = send_client_req(req);
        if (!ret) {
            sent_reqs++;
            list_pop(&conn->pending_requests, struct nvme_req, link);
            conn->list_len--;
        } else
            return ret;
    }
    return sent_reqs;
}

void register_flow(struct pp_conn *conn, unsigned int latency_us_SLO,
                  unsigned long IOPS_SLO, unsigned int rw_ratio_SLO) {
    int ret;
    BINARY_HEADER *header = (BINARY_HEADER *)&conn->data_send[0];
    header->magic = sizeof(BINARY_HEADER);
    header->opcode = CMD_REG;
    header->lba = IOPS_SLO;
    header->lba_count = latency_us_SLO << 7;
    header->lba_count += rw_ratio_SLO;
    header->req_handle = NULL;

    printf("registering a new flow: IOPS_SLO-%ld, latency_SLO-%d, rw_ratio-%d\n",
            IOPS_SLO, latency_us_SLO, rw_ratio_SLO);

    while (conn->tx_sent < sizeof(BINARY_HEADER)) {
        ret = ixev_send(&conn->ctx, header,
                        sizeof(BINARY_HEADER) - conn->tx_sent);
        if (ret < 0)
            printf("ixev_send failed - ret is %d.\n", ret);
        conn->tx_sent += ret;
    }
    conn->tx_sent = 0;
}

void send_handler(void *arg, int num_req) {
    struct nvme_req *req;
    struct ixev_ctx *ctx = (struct ixev_ctx *)arg;
    struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
    unsigned long now;
    unsigned long ns_size = CFG.ns_sizes[0];
    int ssents = 0;
    int i;

    int num4k = (req_size * ns_sector_size) / PAGE_SIZE;
    assert(num4k <= MAX_PAGES_PER_ACCESS);
    if (((req_size * ns_sector_size) % PAGE_SIZE) != 0) num4k++;

    int send_cond;
    int measure_cond;  //
    if (!SWEEP && qdepth) {
        time(&curr_time);
        if (difftime(curr_time, start_time) > run_time) return;

        send_cond = num_req;
        measure_cond = 1;
    } else {
        if (sent == NUM_MEASURE * 3) return;
        if (((now = rdtsc()) - last_send) < (cycles_between_req)) return;
        if (sent == NUM_MEASURE) phase_start = now;
        if (sent == 0) bench_start = now;

        send_cond = ((now - bench_start) / cycles_between_req) >= sent &&
                    sent < (NUM_MEASURE * 3);
        measure_cond = sent >= NUM_MEASURE && sent < NUM_MEASURE * 2;
    }

    while (send_cond) {
        ssents++;
        if (ssents > 32) {
            // never send more than max batch size
            break;
        }

        // setup next request
        req = mempool_alloc(&req_pool);
        if (!req) {
            receive_req(conn);
            // limited qd, if we run out of req, try again later
            failed_alloc_reqs++;
            break;
        }

        ixev_nvme_req_ctx_init(&req->ctx);
        req->lba_count = req_size;

        req->conn = conn;
        req->current_sgl_buf = 0;

        void *req_buf_array[num4k];
        for (i = 0; i < num4k; i++) {
            req_buf_array[i] = mempool_alloc(&nvme_req_buf_pool);
            if (req_buf_array[i] == NULL) {
                printf("ERROR: alloc of nvme_req_buf failed\n");
                assert(0);
            }
            req->buf[i] = req_buf_array[i];
            // printf("req_buf_array[%d] is %p, expect next %x\n", i,
            // req_buf_array[i], (uint64_t)(req_buf_array[i]) - 4096);
        }
        // printf("req buf is %p\n", req->buf[0]);
        // printf("req current_sgl_buf is %d(%p)\n", req->current_sgl_buf,
        // &req->current_sgl_buf);

        const char charset[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMOPQRSTUVWZYZ";
        // if (verify || preconditioning) {  // generate random data to
        // write
        if (preconditioning) {
            int size = req_size * ns_sector_size;
            for (size_t i = 0; i < req_size; i++) {
                for (size_t j = 0; j < PAGE_SIZE; j++) {
                    int key = rand() % (int)(sizeof charset - 1);
                    req->buf[i][j] = charset[key];
                }
            }
        }
#ifdef CLI_DEBUG
        if (!req->buf) {
            printf("NVME_REQ_BUF: MEMPOOL ALLOC FAILED !\n");
            return NULL;
        }
#endif
        if ((rand() % 99) < read_percentage)
            req->cmd = CMD_GET;
        else
            req->cmd = CMD_SET;

        if (preconditioning) req->cmd = CMD_SET;

        //         if (verify) {
        //             if (sent % 2) {
        // #ifdef CLI_DEBUG
        //                 printf("Sending GET to verify...\n");
        // #endif
        //                 req->cmd = CMD_GET;
        //             } else {
        // #ifdef CLI_DEBUG
        //                 printf("Sending SET to initialize...\n");
        // #endif
        //                 req->cmd = CMD_SET;
        //             }
        //         }
        // only do aligned accesses
        if (!sequential) {
            // if (!(verify && sent % 2)) {
            req->lba = rand() % (ns_size >> log_ns_sector_size);
            // align
            req->lba = req->lba & ~7;
            // #ifdef CLI_DEBUG
            //                 printf("Requesting lba @%lu\n", req->lba);
            // #endif
            //                 conn->last_count = req->lba;
            //             } else {
            //                 req->lba = conn->last_count;
            //             }
        } else {
            // if (!(verify && sent % 2))
            conn->last_count += req_size;  // add when SET
            req->lba = conn->last_count;
#ifdef CLI_DEBUG
            printf("Requesting lba @%lu\n", req->lba);
#endif
            if (req->lba >= (ns_size >> log_ns_sector_size)) {
                req->lba %= (ns_size >> log_ns_sector_size);
                conn->last_count = req->lba;
            }
            if (sequential &&
                (req->lba % (((ns_size >> log_ns_sector_size) / req_size) /
                             1000)) == 0)  // cross the 1/100 of ssd namespace
                printf("CPU %d || lba %lu %lu %lu\n", percpu_get(cpu_id),
                       req->lba, NUM_MEASURE, ns_size >> log_ns_sector_size);
        }
        conn->list_len++;
        list_add_tail(&conn->pending_requests, &req->link);

        // struct nvme_req *tmp_req = list_top(&conn->pending_requests,
        // struct nvme_req, link); assert(tmp_req->conn); // checkpoint @2
        // assert(conn
        // == tmp_req->conn); // checkpoint @3

        if (sent == NUM_MEASURE) {
            phase_start = rdtsc();
            __sync_synchronize();  // more accurate timing
        }

        if (measure_cond) {
            if ((rdtsc() - last_send) > (cycles_between_req * 105) / 100) {
                missed_sends++;
            }
        }

        last_send = now;
        sent++;

        if (!SWEEP && qdepth) {
            send_cond--;
        } else {
            send_cond = ((now - bench_start) / cycles_between_req) >= sent &&
                        sent < (NUM_MEASURE * 3);
        }
        // assert(req->conn); // checkpoint @3.5
    }

    int ret = send_pending_client_reqs(conn);
    // if (ret)
    // 	printf("CPU %d | Sent %d batched requests.\n",
    // percpu_get(cpu_id), ret);
}

static void main_handler(struct ixev_ctx *ctx, unsigned int reason) {
    struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
    int ret;

    if (reason == IXEVOUT) {
        ret = send_pending_client_reqs(conn);
        if (ret)
            printf("IXEVOUT: CPU %d | Sent %d batched requests.\n",
                   percpu_get(cpu_id), ret);
    } else if (reason == IXEVHUP) {
        printf("Connection close 5\n");
        if (conn->alive) {
            ixev_close(&conn->ctx);
            conn->alive = false;
        }
        return;
    }
    receive_req(conn);
}

static void pp_dialed(struct ixev_ctx *ctx, long ret) {
    struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
    unsigned long now = rdtsc();

    ixev_set_handler(&conn->ctx, IXEVIN | IXEVOUT | IXEVHUP, &main_handler);
    conn_opened++;
    printf("Tenant %d is dialed. (conn_opened: %d).\n", tid, conn_opened);

    // FIXME: avoid hardcoded latency SLOs
    register_flow(conn, 500, global_target_IOPS, read_percentage);
    receive_req(conn);

    while (rdtsc() < now + 1000000) {
    }
    return;
}

static void pp_release(struct ixev_ctx *ctx) {
    struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
    conn_opened--;
#if CLI_DEBUG
    if (conn_opened == 0)
        printf(
            "Tid: %lx All connections released handle %lx open conns still "
            "%i\n",
            pthread_self(), conn->ctx.handle, conn_opened);
#endif
    mempool_free(&pp_conn_pool, conn);
    terminate = true;
    running = false;
}

static struct ixev_ctx *pp_accept(struct ip_tuple *id) { return NULL; }

static struct ixev_conn_ops pp_conn_ops = {
    .accept = &pp_accept,
    .release = &pp_release,
    .dialed = &pp_dialed,
};

static void *receive_loop(void *arg) {
    int ret, i;
    int flags;
    int num_tests;
    unsigned long ns_size = CFG.ns_sizes[0];

    printf("ns_size is 0x%lx\n", ns_size);

    tid = *(int *)arg;
    conn_opened = 0;
    ret = ixev_init_thread();
    if (ret) {
        fprintf(stderr, "unable to init IXEV\n");
        return NULL;
    };

    ret = mempool_create(&pp_conn_pool, &pp_conn_datastore,
                         MEMPOOL_SANITY_GLOBAL, 0);
    if (ret) {
        fprintf(stderr, "unable to create mempool\n");
        return NULL;
    }

    ret = mempool_create(&req_pool, &nvme_usr_datastore, MEMPOOL_SANITY_GLOBAL,
                         0);
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

    struct pp_conn *conn = mempool_alloc(&pp_conn_pool);
    if (!conn) {
        printf("PP_CONN: MEMPOOL ALLOC FAILED !\n");
        return NULL;
    }

    for (i = 0; i < MAX_LATENCY; i++) {
        measurements[i] = 0;
    }

    list_head_init(&conn->pending_requests);
    conn->rx_received = 0;
    conn->rx_pending = false;
    conn->tx_sent = 0;  // how many bytes were sent
    conn->tx_pending = false;
    conn->in_flight_pkts = 0x0UL;
    conn->sent_pkts = 0x0UL;
    conn->list_len = 0x0UL;
    // conn->receive_loop = true;
    srand(rdtsc());
    conn->last_count =
        rand() %
        (ns_size >> log_ns_sector_size);  // random start, are they same for
                                          // different threads?
    conn->last_count = conn->last_count & ~7;  // align

    ixev_ctx_init(&conn->ctx);

    conn->nvme_fg_handle = 0;  // set to this for now
    conn->alive = true;

    flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    sleep(tid * 1);  // try to serialize
    ixev_dial(&conn->ctx, ip_tuple[tid]);
    if (preconditioning) SWEEP = 0;
    if (!SWEEP)
        num_tests = 1;
    else
        num_tests = NUM_TESTS;
    
    
    while (!running) ixev_wait();

    for (i = 0; i < num_tests; i++) {
        terminate = false;
        assert(sent == 0);
        assert(measure == 0);
        if (SWEEP) {
            cycles_between_req =
                (((unsigned long)rte_get_timer_hz()) /
                 (sweep[i] / nr_threads));
            NUM_MEASURE = sweep[i] * DURATION / nr_threads;
        } else {
            cycles_between_req =
                ((unsigned long)rte_get_timer_hz() * nr_threads) /
                global_target_IOPS;
            NUM_MEASURE = global_target_IOPS * DURATION / nr_threads;
        }
        assert(NUM_MEASURE <= MAX_NUM_MEASURE);
        if (preconditioning)  // write each lba once
            NUM_MEASURE = (ns_size / ns_sector_size) / req_size;
#ifdef CLI_DEBUG
        if (tid == 0)
            printf(
                "Test round %d: cycles between requests is %lu(%d "
                "us/request, "
                "DDL: %lu), NUM_MEASURE is %d.\n",
                i, cycles_between_req, cycles_between_req / rte_get_timer_hz() * 1E6,
                cycles_between_req + cycles_between_req / 10, NUM_MEASURE);
#endif
        pthread_barrier_wait(&barrier);  // caution

        time(&start_time);
        //---

        if (!SWEEP && qdepth) {
            if (running) send_handler(&conn->ctx, qdepth);
            while (1) {
                ixev_wait();
                if (terminate) break;
            }
        } else {
            while (1) {
                if (running) send_handler(&conn->ctx, 0);
                ixev_wait();
                if (terminate) break;
            }
        }
        //---
    }
    running = true;
    if (conn->alive) {
        printf("[%d] Connection normally closed.\n", percpu_get(cpu_id));
        ixev_close(&conn->ctx);
        conn->alive = false;
    } else {
        running = false;
    }

    while (running) ixev_wait();

    return NULL;
}

int reflex_client_main(int argc, char *argv[]) {
    int ret;
    unsigned int pp_conn_pool_entries;
    int nr_cpu, req_size_bytes;
    pthread_t thread[64];
    int tid[64];  // why
    int i;

    // sleep(10);

    nr_cpu = cpus_active;
    if (nr_cpu < 1) {
        fprintf(stderr, "got invalid cpu count %d\n", nr_cpu);
        exit(-1);
    }

    int opt;

    while ((opt = getopt(argc, argv, "s:p:w:T:i:r:S:R:P:d:t:h")) != -1) {
        switch (opt) {
            case 's':
                ip = malloc(sizeof(char) * strlen(optarg));
                strcpy(ip, optarg);
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'w':
                if (strcmp(optarg, "seq") == 0)
                    sequential = 1;
                else if (strcmp(optarg, "rand") == 0)
                    sequential = 0;
                else {
                    sequential = 0;
                    printf(
                        "WARNING: invalid workload type, use random by "
                        "default\n");
                }
                break;
            case 'T':
                nr_threads = atoi(optarg);
                break;
            case 'i':
                global_target_IOPS = atoi(optarg);
                break;
            case 'r':
                read_percentage = atoi(optarg);
                break;
            case 'S':
                SWEEP = atoi(optarg);
                break;
            case 'R':
                req_size_bytes = atoi(optarg);
                if (req_size_bytes % ns_sector_size != 0) {
                    printf(
                        "WARNING: request size should be multiple of "
                        "sector "
                        "size\n");
                }
                req_size = req_size_bytes / ns_sector_size;
                break;
            case 'P':
                preconditioning = atoi(optarg);
                break;
            case 'd':
                qdepth = atoi(optarg);
                break;
            case 't':
                run_time = atoi(optarg);
                break;
            case 'h':
                fprintf(stderr,
                        "\nUsage: \n"
                        "sudo ./dp/ix\n"
                        "to run ReFlex server, no parameters required\n"
                        "to run ReFlex client, set the following options:\n"
                        "-s  server IP address\n"
                        "-p  server port number\n"
                        "-w  workload type [seq/rand] (default=rand)\n"
                        "-T  number of threads (default=1)\n"
                        "-i  target IOPS for open-loop test (default=50000)\n"
                        "-r  percentage of read requests (default=100)\n"
                        "-S  sweep multiple target IOPS for open-loop test "
                        "(default=1)\n"
                        "-R  request size in bytes (default=1024)\n"
                        "-P  precondition (default=0)\n"
                        "-d  queue depth for closed-loop test (default=0)\n"
                        "-t  execution time in seconds for closed-loop test "
                        "(default=0)\n");
                exit(1);
            default:
                fprintf(stderr, "invalid command option\n");
                exit(1);
        }
    }

    printf(
        "DEBUG: ip=%s, port=%d, seq=%d, nr_threads=%d, global=%d, read=%d, "
        "SWEEP=%d, req_size_bytes=%d, preconditioning=%d, qdepth=%d, "
        "run_time=%d\n",
        ip, port, sequential, nr_threads, global_target_IOPS, read_percentage,
        SWEEP, req_size_bytes, preconditioning, qdepth, run_time);

    if (ip == NULL) {
        fprintf(stderr,
                "missing server IP address, enter -s [ip] to specify\n");
        exit(1);
    }
    if (port == -1) {
        fprintf(stderr, "missing port number, enter -p [port] to specify\n");
        exit(1);
    }
    if (SWEEP && qdepth) {
        fprintf(stderr, "SWEEP and qdepth cannot both be nonzero\n");
        exit(1);
    }

    assert(nr_threads <= nr_cpu);
    pthread_barrier_init(&barrier, NULL, nr_threads);

    for (i = 0; i < nr_threads; i++) {
        ip_tuple[i] = malloc(sizeof(struct ip_tuple[i]));
        if (!ip_tuple[i]) exit(-1);

        if (parse_ip_addr(ip, &ip_tuple[i]->dst_ip)) {
            fprintf(stderr, "Bad IP address '%s'", ip);
            exit(1);
        }

        timer_calibrate_tsc();

        ip_tuple[i]->dst_port = port + i;  // mutli-thread
        ip_tuple[i]->src_port = port + i;
        printf("Connecting to port: %i\n", port + i);
    }

    free(ip);
    cycles_between_req =
        ((unsigned long)rte_get_timer_hz() * nr_threads) /
        global_target_IOPS;
    NUM_MEASURE = global_target_IOPS * DURATION / nr_threads;
    pp_conn_pool_entries = 16 * 4096;
    pp_conn_pool_entries =
        ROUND_UP(pp_conn_pool_entries, MEMPOOL_DEFAULT_CHUNKSIZE);
    ixev_init(&pp_conn_ops);
    ret = mempool_create_datastore(&pp_conn_datastore, pp_conn_pool_entries,
                                   sizeof(struct pp_conn), "pp_conn");
    if (ret) {
        fprintf(stderr, "unable to create mempool\n");
        return ret;
    }

    ret = mempool_create_datastore(&nvme_usr_datastore, outstanding_reqs * 2,
                                   sizeof(struct nvme_req), "nvme_req_1");
    if (ret) {
        fprintf(stderr, "unable to create datastore\n");
        return ret;
    }

    ret = mempool_create_datastore_align(&nvme_req_buf_datastore,
                                         // *2 avoids out-of-mem error
                                         outstanding_reqs * 2, PAGE_SIZE,
                                         "nvme_req_2");
    if (ret) {
        fprintf(stderr, "unable to create datastore\n");
        return ret;
    }

    for (i = 1; i < nr_cpu; i++) {
        // ret = pthread_create(&tid, NULL, start_cpu, (void *)(unsigned
        // long) i);
        log_info("rte_eal_remote_launch...receive_loop\n");
        tid[i] = i;
        // ret = rte_eal_remote_launch(receive_loop, (void *)(unsigned long)
        // i, i);
        ret = rte_eal_remote_launch(receive_loop, &tid[i], i);

        if (ret) {
            log_err("init: unable to start app\n");
            return -EAGAIN;
        }
    }

    printf("Started %i threads\n", nr_cpu);
    tid[0] = 0;

    receive_loop(&tid[0]);
    return 0;
}
