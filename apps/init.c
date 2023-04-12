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

/*
 * init.c - initialization
 */

#include <ix/errno.h>
#include <ix/log.h>
#include <ix/stddef.h>
#include <pthread.h>
#include <rte_atomic.h>
#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_version.h>
#include <rte_timer.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
//#include <ix/pci.h>
#include <ix/cfg.h>
#include <ix/control_plane.h>
#include <ix/cpu.h>
#include <ix/dpdk.h>
#include <ix/ethdev.h>
#include <ix/ethqueue.h>
#include <ix/kstats.h>
#include <ix/lock.h>
#include <ix/log.h>
#include <ix/mbuf.h>
#include <ix/mempool.h>
#include <ix/profiler.h>
#include <ix/syscall.h>
#include <ix/timer.h>
#include <lwip/memp.h>
#include <net/ip.h>
#include <reflex.h>

#define MSR_RAPL_POWER_UNIT 1542
#define ENERGY_UNIT_MASK 0x1F00
#define ENERGY_UNIT_OFFSET 0x08

// #define RSS_ENABLE

static int init_parse_cpu(void);
static int init_parse_mem(void);
static int init_cfg(void);
static int init_firstcpu(void);
static int init_hw(void);
static int init_ethdev(void);

extern int init_nvmedev(void);
extern int init_nvmeqp_cpu(void);
extern int init_nvme_request(void);
extern int init_nvme_request_cpu(void);

extern int net_init(void);
extern int tcp_api_init(void);
extern int tcp_api_init_cpu(void);
extern int tcp_api_init_fg(void);
extern int sandbox_init(int argc, char *argv[]);
extern void tcp_init(struct eth_fg *);
extern int cp_init(void);
extern int mempool_init(void);
extern int init_migration_cpu(void);
extern int dpdk_init(void);

struct init_vector_t {
    const char *name;
    int (*f)(void);
    int (*fcpu)(void);
    int (*ffg)(unsigned int);
};

static struct init_vector_t init_tbl[] = {
    {"CPU", cpu_init, NULL, NULL},
    {"cfgcpu", init_parse_cpu, NULL, NULL},  // after cpu
    {"cfgmem", init_parse_mem, NULL, NULL},  // after cpu
    {"dpdk", dpdk_init, NULL, NULL},
    {"timer", timer_init, NULL , NULL},
    {"net", net_init, NULL, NULL},
    {"cfg", init_cfg, NULL, NULL},  // after net
    {"cp", cp_init, NULL, NULL},
    {"firstcpu", init_firstcpu, NULL, NULL},   // after cfg
    {"mbuf", mbuf_init, mbuf_init_cpu, NULL},  // after firstcpu
    {"memp", memp_init, memp_init_cpu, NULL},
    {"tcpapi", tcp_api_init, tcp_api_init_cpu, NULL},
    {"ethdev", init_ethdev, ethdev_init_cpu, NULL},
    {"nvmemem", init_nvme_request, init_nvme_request_cpu, NULL},
    {"migration", NULL, init_migration_cpu, NULL},
    {"nvmedev", init_nvmedev, NULL, NULL},    // before per-cpu init
    {"hw", init_hw, NULL, NULL},              // spaws per-cpu init sequence
    {"nvmeqp", NULL, init_nvmeqp_cpu, NULL},  // after per-cpu init
    {"syscall", NULL, syscall_init_cpu, NULL},
#ifdef ENABLE_KSTATS
    {"kstats", NULL, kstats_init_cpu, NULL},  // after timer
#endif
    {NULL, NULL, NULL, NULL}};

static int init_argc;
static char **init_argv;
static int args_parsed;

volatile int uaccess_fault;

static struct rte_eth_conf default_eth_conf = {
    .rxmode = {
        .max_rx_pkt_len = 9128,
        .offloads = DEV_RX_OFFLOAD_TIMESTAMP |
#if (RTE_VER_YEAR <= 18) && (RTE_VER_MONTH < 11)
                    DEV_RX_OFFLOAD_CRC_STRIP |
#endif
                    DEV_RX_OFFLOAD_CHECKSUM |
                    DEV_RX_OFFLOAD_HEADER_SPLIT |
                    DEV_RX_OFFLOAD_JUMBO_FRAME,
#ifdef RSS_ENABLE
        .mq_mode = ETH_MQ_RX_RSS,  // multiple queue mode: RSS | DCB | VMDQ
#endif
    },
    // .rx_adv_conf = {
    // 	.rss_conf = {
    // 		.rss_hf = ETH_RSS_NONFRAG_IPV4_TCP | ETH_RSS_NONFRAG_IPV4_UDP,
    // 		// .rss_hf = ETH_RSS_IPV4_TCP
    // 		// .rss_key_len = 40,
    // 		.rss_key = {0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
    // 					0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
    // 					0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
    // 					0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
    // 					0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa},
    // 		// .rss_hf = ETH_RSS_PORT
    // 	},
    // },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE, .offloads = DEV_TX_OFFLOAD_IPV4_CKSUM | DEV_TX_OFFLOAD_TCP_CKSUM,
        // DEV_TX_OFFLOAD_TCP_TSO		|
        // DEV_TX_OFFLOAD_VLAN_INSERT |
        // DEV_TX_OFFLOAD_UDP_CKSUM   |
        // DEV_TX_OFFLOAD_SCTP_CKSUM,
    },
#ifndef RSS_ENABLE
    .fdir_conf = {
        .mode = RTE_FDIR_MODE_PERFECT,
        .pballoc = RTE_FDIR_PBALLOC_256K,
        .mask = {
            .vlan_tci_mask = 0x0,
            .ipv4_mask = {
                .src_ip = 0xFFFFFFFF,
                .dst_ip = 0xFFFFFFFF,
            },
            .src_port_mask = 0xFFFF,
            .dst_port_mask = 0xFFFF,
            .mac_addr_byte_mask = 0,
            .tunnel_type_mask = 0,
            .tunnel_id_mask = 0,
        },
        .drop_queue = 127,
        .flex_conf = {
            .nb_payloads = 0,
            .nb_flexmasks = 0,
        },
    },
#endif
};

/**
 * add_fdir_rules
 * Sets up flow director to direct incoming packets.
 */
int add_fdir_rules(uint8_t port_id) {
    log_info("Adding FDIR rules:\n");
    int ret;

    // Check that flow director is supported.
    if (ret = rte_eth_dev_filter_supported(port_id, RTE_ETH_FILTER_FDIR)) {
        log_err("This device does not support Flow Director (Error %d).\n", ret);
        return -ENOTSUP;
    }

    // Flush any existing flow director rules.
    ret = rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_FDIR, RTE_ETH_FILTER_FLUSH, NULL);
    if (ret < 0) {
        log_err("Could not flush FDIR entries.\n");
        return -1;
    }

    // Add flow director rules (currently added from static config file in cfg.c).
    ret = parse_cfg_fdir_rules(port_id);  // FIXME: decouple this function

    return ret;
}

/**
 * init_port
 * Sets up the ethernet port on a given port id.
 */
static void init_port(uint8_t port_id, struct eth_addr *mac_addr) {
    int ret;

    uint16_t nb_rx_q = CFG.num_cpus;
    uint16_t nb_tx_q = CFG.num_cpus;
    struct rte_eth_conf *dev_conf;
    dev_conf = &default_eth_conf;
    struct rte_eth_dev_info dev_info;

    memset(&dev_info, 0, sizeof(dev_info));
    rte_eth_dev_info_get(port_id, &dev_info);
#if (RTE_VER_RELEASE > 90)
    dev_conf->txmode.offloads &= dev_info.tx_offload_capa;
    dev_conf->rxmode.offloads &= dev_info.rx_offload_capa;
#endif

    uint16_t mtu;

    if (dev_conf->txmode.offloads & DEV_TX_OFFLOAD_TCP_CKSUM) {
        printf("TX TCP checksum offloading is enabled\n");
    } else {
        printf("WARNING: TX TCP offloading not supported\n");
    }

    if (dev_conf->rxmode.offloads & DEV_RX_OFFLOAD_TIMESTAMP) {
        printf("RX Timestamp offloading is enabled\n");
    } else {
        printf("WARNING: RX Timestamp offloading not supported\n");
    }

    printf("nb_tx_queues is %d\n", dev_info.nb_tx_queues);
    printf("nb_rx_queues is %d\n", dev_info.nb_rx_queues);

    /* Configure the Ethernet device. */
    ret = rte_eth_dev_configure(port_id, nb_rx_q, nb_tx_q, dev_conf);
    if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eth_dev_configure:err=%d, port=%u\n", ret, (unsigned)port_id);

    // print_rss_conf(port_id);

    if (dev_conf->rxmode.offloads & DEV_RX_OFFLOAD_JUMBO_FRAME) {
        rte_eth_dev_set_mtu(port_id, 9000);
        rte_eth_dev_get_mtu(port_id, &mtu);
        printf("Enable jumbo frames. MTU size is %d\n", mtu);
        //FIXME: rx_qinfo crashes with ixgbe DPDK driver (but works fine on AWS)
        //struct rte_eth_rxq_info rx_qinfo;
        //rte_eth_rx_queue_info_get(port_id, 0, &rx_qinfo);
        //rx_qinfo.scattered_rx = 1;

        //txconf->txq_flags = 0;
    } else {
        rte_eth_dev_get_mtu(port_id, &mtu);
        printf("Disable jumbo frames. MTU size is %d\n", mtu);
    }

    init_queues(port_id, &dev_info);

    // print_initial_reta_table();

    ret = rte_eth_dev_start(port_id);     // this will alter the reta table
    rte_eth_promiscuous_enable(port_id);  // Should be here to avoid driver problem but do we really need this?

    // print_rss_conf(port_id);

    if (ret < 0) {
        printf("ERROR starting device at port %d\n", port_id);
    } else {
        printf("started device at port %d\n", port_id);
    }

    // print_initial_reta_table();

    rte_eth_dev_info_get(port_id, &dev_info);  // get new number
    printf("nb_tx_queues is %d\n", dev_info.nb_tx_queues);
    printf("nb_rx_queues is %d\n", dev_info.nb_rx_queues);

#ifdef MQ_DEBUG
// rss_reta_setup(port_id, &dev_info);
#endif

    // print_initial_reta_table();

    // /* PMD link up */
    // ret = rte_eth_dev_set_link_up(port_id);
    // /* Do not panic if PMD does not provide link up functionality */
    // if (ret < 0 && ret != -ENOTSUP)
    // 	rte_panic("Port %d: PMD set link up error %d", port_id, ret);

    // expansion of assert_link_status();
    struct rte_eth_link link;
    memset(&link, 0, sizeof(link));
    rte_eth_link_get(port_id, &link);

    // print_initial_reta_table();

    if (!link.link_status) {
        log_warn("eth:\tlink appears to be down, check connection.\n");
    } else {
        printf("eth:\tlink up - speed %u Mbps, %s\n",
               (uint32_t)link.link_speed,
               (link.link_duplex == ETH_LINK_FULL_DUPLEX) ? ("full-duplex") : ("half-duplex"));
        rte_eth_macaddr_get(port_id, mac_addr);
        g_active_eth_port = port_id;
    }
}

inline void print_rss_conf(port_id) {
    struct rte_eth_rss_conf rss_conf;
    int ret = rte_eth_dev_rss_hash_conf_get(port_id, &rss_conf);
    if (ret < 0) {
        printf("Unable to get the rss configuration.\n");
    } else {
        printf("The rss_key_len is %d, now printing\n", rss_conf.rss_key_len);
        printf("The rss_key is at %p\n", rss_conf.rss_key);
        if (rss_conf.rss_key) {
            int i;
            for (i = 0; i < rss_conf.rss_key_len; i++) {
                printf("[%d]: %#1x\n", i, rss_conf.rss_key[i]);
            }
        }
        printf("The rss_hf is %lx, which was set as %lx\n", rss_conf.rss_hf, ETH_RSS_NONFRAG_IPV4_TCP | ETH_RSS_NONFRAG_IPV4_UDP);
    }
}

/* initialize one queue per cpu */
inline void init_queues(uint8_t port_id, struct rte_eth_dev_info *dev_info) {
    int ret;
    uint16_t nb_tx_desc = ETH_DEV_TX_QUEUE_SZ;  //1024
    uint16_t nb_rx_desc = ETH_DEV_RX_QUEUE_SZ;  //512
    int nb_queues = CFG.num_cpus;

    // struct rte_eth_txconf* txconf;
    // txconf = &dev_info.default_txconf;  //FIXME: this should go here but causes TCP rx bug

    printf("max_tx_queues is %d\n", dev_info->max_tx_queues);
    printf("max_rx_queues is %d\n", dev_info->max_rx_queues);

    if (unlikely(nb_queues > dev_info->max_tx_queues || nb_queues > dev_info->max_rx_queues)) {  // Assume #rx queues = #tx queues
        log_info("Too many cores, only the first of %d will work\n", dev_info->max_tx_queues);
        nb_queues = dev_info->max_tx_queues;
    }

    unsigned lcore_id = 0;
    // RTE_LCORE_FOREACH(lcore_id) {
    for (lcore_id = 0; lcore_id < nb_queues; lcore_id++) {
        log_info("Setting up TX queues for core %d... at socket %d\n", lcore_id, rte_eth_dev_socket_id(port_id));

        ret = rte_eth_tx_queue_setup(port_id, lcore_id, nb_tx_desc, rte_eth_dev_socket_id(port_id), NULL /*txconf*/);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "tx queue setup: err=%d, port=%u\n", ret, (unsigned)port_id);
    }

    for (lcore_id = 0; lcore_id < nb_queues; lcore_id++) {
        log_info("Setting up RX queues for core %d... at socket %d\n", lcore_id, rte_eth_dev_socket_id(port_id));
        ret = rte_eth_rx_queue_setup(port_id, lcore_id, nb_rx_desc, rte_eth_dev_socket_id(port_id), NULL, dpdk_pool);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rx queue setup: err=%d, port=%u\n", ret, (unsigned)port_id);
    }

    // rte_eth_dev_info_get(port_id, &dev_info); // get new number
    // printf("nb_tx_queues is %d\n", dev_info->nb_tx_queues);
    // printf("nb_rx_queues is %d\n", dev_info->nb_rx_queues);
}

inline void rss_reta_setup(uint8_t port_id, struct rte_eth_dev_info *dev_info) {
    struct rte_eth_rss_reta_entry64 reta_conf[ETH_RSS_RETA_SIZE_128 / RTE_RETA_GROUP_SIZE];
    uint32_t i;
    int ret;

    /* Get RETA size */
    if (dev_info->reta_size == 0)
        rte_panic("Port %u: RSS setup error (null RETA size)\n", port_id);

    if (dev_info->reta_size > ETH_RSS_RETA_SIZE_512)
        rte_panic("Port %u: RSS setup error (RETA size too big)\n", port_id);

    /* Setup RETA contents */
    memset(&reta_conf, 0, sizeof(reta_conf));

    for (i = 0; i < dev_info->reta_size; i++)
        reta_conf[i / RTE_RETA_GROUP_SIZE].mask = 0x3;

    for (i = 0; i < dev_info->reta_size; i++) {
        uint32_t reta_id = i / RTE_RETA_GROUP_SIZE;
        uint32_t reta_pos = i % RTE_RETA_GROUP_SIZE;

        reta_conf[reta_id].reta[reta_pos] = 1;  // set all to queue 0, like 0x1
    }

    /* RETA update */
    ret = rte_eth_dev_rss_reta_update(port_id, &reta_conf, dev_info->reta_size);
    if (ret != 0)
        rte_panic("Port %u: RSS setup error (RETA update failed)\n", port_id);
}

inline void print_initial_reta_table() {
    log_info("Now printing the initial reta table:\n");
    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(0, &dev_info);
    struct rte_eth_rss_reta_entry64 reta_conf[ETH_RSS_RETA_SIZE_128 / RTE_RETA_GROUP_SIZE];

    //struct rte_eth_rss_reta_entry64 reta_conf;
    int ret = rte_eth_dev_rss_reta_query(0, &reta_conf, dev_info.reta_size);
    if (ret < 0)
        printf("Unable to get reta table\n");
    else {
        printf("Printing reta table:\n");
        int i = 0;
        for (; i < RTE_RETA_GROUP_SIZE; i++) {
            printf("%d: %d\n", i, reta_conf[0].reta[i]);
        }
        for (; i < ETH_RSS_RETA_SIZE_128; i++) {
            printf("%d: %d\n", i, reta_conf[1].reta[i]);
        }
    }
}

/**
 * init_ethdev - initializes an ethernet device
 *
 * Returns 0 if successful, otherwise fail.
 */
static int init_ethdev(void) {
    int ret;

    // DPDK init for pci ethdev already done in dpdk_init()
    uint8_t port_id;
    uint8_t nb_ports;
    struct rte_ether_addr mac_addr;

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0) rte_exit(EXIT_FAILURE, "No Ethernet ports - exiting\n");
    if (nb_ports > 1) printf("WARNING: only 1 ethernet port is used\n");
    // if (nb_ports > RTE_MAX_ETHPORTS) nb_ports = RTE_MAX_ETHPORTS; // too many ports, comment for now

    // for (port_id = nb_ports-1; port_id < nb_ports; port_id++) { // too many ports, only initialize the one used
    printf("We have %d nb_ports now.\n", nb_ports);
    for (port_id = 0; port_id < nb_ports; port_id++) {
        init_port(port_id, &mac_addr);
        // print_rss_conf(port_id); FIXME: this will cause segfault at spdk_nvme_probe
        log_info("Ethdev on port %d initialised.\n", port_id);

#ifndef RSS_ENABLE
        ret = add_fdir_rules(port_id);
        if (ret) {
            log_err("Adding FDIR rules failed. (Error %d)\n", ret);
        } else {
            log_info("All FDIR rules added.\n");
        }
#endif
    }

    struct eth_addr *macaddr = &mac_addr;
    CFG.mac = *macaddr;  // Always get the last port
    // percpu_get(eth_num_queues) = CFG.num_cpus; //NOTE: assume num tx queues == num rx queues
    percpu_get(eth_num_queues) = nb_ports;

    return 0;
}

/**
 * init_create_cpu - initializes a CPU
 * @cpu: the CPU number
 * @eth: the ethernet device to assign to this CPU
 *
 * Returns 0 if successful, otherwise fail.
 */
static int init_create_cpu(unsigned int cpu, int first) {
    int ret = 0, i;

    if (!first)
        ret = cpu_init_one(cpu);

    if (ret) {
        log_err("init: unable to initialize CPU %d\n", cpu);
        return ret;
    }

    log_info("init: percpu phase %d\n", cpu);
    for (i = 0; init_tbl[i].name; i++)
        if (init_tbl[i].fcpu) {
            ret = init_tbl[i].fcpu();
            log_info("init: module %-10s on %d: %s \n", init_tbl[i].name, RTE_PER_LCORE(cpu_id), (ret ? "FAILURE" : "SUCCESS"));
            if (ret)
                panic("could not initialize IX\n");
        }

    log_info("init: CPU %d ready\n", cpu);
    printf("init:CPU %d ready\n", cpu);
    return 0;
}

static pthread_mutex_t spawn_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t spawn_cond = PTHREAD_COND_INITIALIZER;

struct spawn_req {
    void *arg;
    struct spawn_req *next;
};

static struct spawn_req *spawn_reqs;
extern void *pthread_entry(void *arg);

static void wait_for_spawn(void) {
    struct spawn_req *req;
    void *arg;

    pthread_mutex_lock(&spawn_mutex);
    while (!spawn_reqs)
        pthread_cond_wait(&spawn_cond, &spawn_mutex);
    req = spawn_reqs;
    spawn_reqs = spawn_reqs->next;
    pthread_mutex_unlock(&spawn_mutex);

    arg = req->arg;
    free(req);

    log_info("init: user spawned cpu %d\n", RTE_PER_LCORE(cpu_id));
    //pthread_entry(arg);
    //pp_main(NULL);
}

int init_do_spawn(void *arg) {
    struct spawn_req *req;

    pthread_mutex_lock(&spawn_mutex);
    req = malloc(sizeof(struct spawn_req));
    if (!req) {
        pthread_mutex_unlock(&spawn_mutex);
        return -ENOMEM;
    }

    req->next = spawn_reqs;
    req->arg = arg;
    spawn_reqs = req;
    pthread_cond_broadcast(&spawn_cond);
    pthread_mutex_unlock(&spawn_mutex);

    return 0;
}

static int init_fg_cpu(void) {
    int fg_id, ret;
    int start;
    DEFINE_BITMAP(fg_bitmap, ETH_MAX_TOTAL_FG);

    start = RTE_PER_LCORE(cpu_nr);

    bitmap_init(fg_bitmap, ETH_MAX_TOTAL_FG, 0);
    for (fg_id = start; fg_id < nr_flow_groups; fg_id += CFG.num_cpus)
        bitmap_set(fg_bitmap, fg_id);

    eth_fg_assign_to_cpu(fg_bitmap, RTE_PER_LCORE(cpu_nr));

    for (fg_id = start; fg_id < nr_flow_groups; fg_id += CFG.num_cpus) {
        eth_fg_set_current(fgs[fg_id]);

        assert(fgs[fg_id]->cur_cpu == RTE_PER_LCORE(cpu_id));

        tcp_init(fgs[fg_id]);
        ret = tcp_api_init_fg();
        if (ret) {
            log_err("init: failed to initialize tcp_api \n");
            return ret;
        }

	// timer_init_fg();
    }

    // unset_current_fg();

    //FIXME: figure out flow group stuff, this is temp fix for fg_id == cpu_id (no migration)
    fg_id = percpu_get(cpu_id);
    //fg_id = outbound_fg_idx();
    fgs[fg_id] = malloc(sizeof(struct eth_fg));
    fgs[fg_id] = malloc(sizeof(struct eth_fg));
    memset(fgs[fg_id], 0, sizeof(struct eth_fg));
    eth_fg_init(fgs[fg_id], fg_id);
    eth_fg_init_cpu(fgs[fg_id]);
    fgs[fg_id]->cur_cpu = RTE_PER_LCORE(cpu_id);
    fgs[fg_id]->fg_id = fg_id;
    //fgs[fg_id]->eth = percpu_get(eth_rxqs[0])->dev;
    tcp_init(fgs[fg_id]);

    return 0;
}

static pthread_barrier_t start_barrier;
static volatile int started_cpus;

void *start_cpu(void *arg) {
    log_info("start_cpu\n");
    int ret;
    unsigned int cpu_nr_ = (unsigned int)(unsigned long)arg;
    unsigned int cpu = CFG.cpu[cpu_nr_];

    ret = init_create_cpu(cpu, 0);
    if (ret) {
        log_err("init: failed to initialize CPU %d\n", cpu);
        exit(ret);
    }

    started_cpus++;

    /* percpu_get(cp_cmd) of the first CPU is initialized in init_hw. */

    RTE_PER_LCORE(cpu_nr) = cpu_nr_;
    percpu_get(cp_cmd) = &cp_shmem->command[started_cpus];
    percpu_get(cp_cmd)->cpu_state = CP_CPU_STATE_RUNNING;

    //pthread_barrier_wait(&start_barrier);
    rte_smp_mb();

    //log_info("skipping fg init for now.....\n");

    ret = init_fg_cpu();
    if (ret) {
        log_err("init: failed to initialize flow groups\n");
        exit(ret);
    }

    return NULL;
}

static int init_hw(void) {
    // If we are not on the master lcore, we don't spawn new threads
    int master_id = rte_get_master_lcore();
    int lcore_id = rte_lcore_id();

    if (master_id != lcore_id)
        return 0;

    int i, ret = 0;
    pthread_t tid;
    int j;
    int fg_id;

    // will spawn per-cpu initialization sequence on CPU0
    ret = init_create_cpu(CFG.cpu[0], 1);
    if (ret) {
        log_err("init: failed to create CPU 0\n");
        return ret;
    }

    log_info("created cpu\n");

    RTE_PER_LCORE(cpu_nr) = 0;
    percpu_get(cp_cmd) = &cp_shmem->command[0];
    percpu_get(cp_cmd)->cpu_state = CP_CPU_STATE_RUNNING;

    for (i = 1; i < CFG.num_cpus; i++) {
        //ret = pthread_create(&tid, NULL, start_cpu, (void *)(unsigned long) i);
        log_info("rte_eal_remote_launch...start_cpu\n");
        ret = rte_eal_remote_launch(start_cpu, (void *)(unsigned long)i, i);

        if (ret) {
            log_err("init: unable to create lthread\n");
            return -EAGAIN;
        }
        while (started_cpus != i)
            usleep(100);
    }

    /*
	fg_id = 0;
	for (i = 0; i < CFG.num_ethdev; i++) {
		struct ix_rte_eth_dev *eth = eth_dev[i];

		if (!eth->data->nb_rx_queues)
			continue;

		ret = eth_dev_start(eth); 
		if (ret) {
			log_err("init: failed to start eth%d\n", i);
			return ret;
		}

		for (j = 0; j < eth->data->nb_rx_fgs; j++) {
			eth_fg_init_cpu(&eth->data->rx_fgs[j]);
			fgs[fg_id] = &eth->data->rx_fgs[j];
			fgs[fg_id]->dev_idx = i;
			fgs[fg_id]->fg_id = fg_id;
			fg_id++;
		}
	}
	*/

    nr_flow_groups = fg_id;
    cp_shmem->nr_flow_groups = nr_flow_groups;

    mempool_init();

    if (CFG.num_cpus > 1) {
        //pthread_barrier_wait(&start_barrier);
        //rte_smp_mb();
        rte_eal_mp_wait_lcore();
    }

    log_info("skipping init_fg_cpu for now...\n");

    init_fg_cpu();
    if (ret) {
        log_err("init: failed to initialize flow groups\n");
        exit(ret);
    }

    log_info("init: barrier after al CPU initialization\n");

    return 0;
}

static int init_cfg(void) {
    return cfg_init(init_argc, init_argv, &args_parsed);
}

static int init_parse_cpu(void) {
    return cfg_parse_cpu(init_argc, init_argv, &args_parsed);
}

static int init_parse_mem(void) {
    return cfg_parse_mem(init_argc, init_argv, &args_parsed);
}

static int init_firstcpu(void) {
    int ret;
    unsigned long msr_val;
    unsigned int value;
    int i;

    cpus_active = CFG.num_cpus;
    cp_shmem->nr_cpus = CFG.num_cpus;
    if (CFG.num_cpus > 1) {
        pthread_barrier_init(&start_barrier, NULL, CFG.num_cpus);
    }

    for (i = 0; i < CFG.num_cpus; i++)
        cp_shmem->cpu[i] = CFG.cpu[i];

    ret = cpu_init_one(CFG.cpu[0]);
    if (ret) {
        log_err("init: failed to initialize CPU 0\n");
        return ret;
    }
    // TODO: Need to figure out how to replace this calculation
    /*
	msr_val = rdmsr(MSR_RAPL_POWER_UNIT);
	
	value = (msr_val & ENERGY_UNIT_MASK) >> ENERGY_UNIT_OFFSET;
	energy_unit = 1.0 / (1 << value);
	*/

    return ret;
}

int main(int argc, char *argv[]) {
    int ret, i;

    init_argc = argc;
    init_argv = argv;

    log_info("init: starting IX\n");

    log_info("init: cpu phase\n");
    for (i = 0; init_tbl[i].name; i++)
        if (init_tbl[i].f) {
            ret = init_tbl[i].f();
            log_info("init: module %-10s %s\n", init_tbl[i].name, (ret ? "FAILURE" : "SUCCESS"));
            if (ret)
                panic("could not initialize IX\n");
        }

    // ret = echoserver_main(argc - args_parsed, &argv[args_parsed]);
    if (argc > 1)
        ret = reflex_client_main(argc - args_parsed, &argv[args_parsed]);
    else
        ret = reflex_server_main(argc - args_parsed, &argv[args_parsed]);

    if (ret) {
        log_err("init: failed to start echoserver\n");
        // log_err("init: failed to start reflex server\n");
        return ret;
    }

    log_info("init done\n");
    return 0;
}
