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

/*
 * cfg.c - configuration parameters
 *
 * parsing of the configuration file parameters. All existing parameters are
 * defined in struct config_tbl, along with their respective handlers.
 * In order to add a parameter, add a corresponding entry to config_tbl along
 * with an appropriate handler which will store the setting into the global
 * settings data struct.
 */

#include <getopt.h>
#include <inttypes.h>
#include <ix/cfg.h>
#include <ix/cpu.h>
#include <ix/errno.h>
#include <ix/log.h>
#include <ix/types.h>
#include <libconfig.h> /* provides hierarchical config file parsing */
#include <net/ethernet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// #include <net/ip.h>  // temp fix below
#include <ix/byteorder.h>
struct ip_addr {
    uint32_t addr;
} __packed;
#define MAKE_IP_ADDR(a, b, c, d)                                      \
    (((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | \
     (uint32_t)d)

#include <ix/ethdev.h>
#include <limits.h>

#define DEFAULT_CONF_FILE "./ix.conf"

struct cfg_parameters CFG;

extern int net_cfg(void);
extern int arp_insert(struct ip_addr *addr, struct eth_addr *mac);

// defined in the header
unsigned long MAX_DEV_TOKEN_RATE;
int g_nvme_dev_model;
// bool g_nvme_sched_flag;
uint8_t g_nvme_sched_mode;
struct lat_tokenrate_pair g_dev_models[128];
int g_dev_model_size;

int g_cores_active;
int g_dpdk_mem_channel;

static config_t cfg;
static config_t cfg_devmodel;
static char config_file[256];
static char devmodel_file[256];

static int parse_host_addr(void);
static int parse_port(void);
static int parse_gateway_addr(void);
static int parse_arp(void);
static int parse_devices(void);
static int parse_nvme_devices(void);
static int parse_ns_sizes(void);
static int parse_nvme_device_model(void);
static int parse_cpu(void);
// static int parse_mem_channel(void);
static int parse_batch(void);
static int parse_loader_path(void);
static int parse_scheduler_mode(void);

extern int ixgbe_fdir_add_rule(uint32_t dst_addr, uint32_t src_addr,
                               uint16_t dst_port, int queue_id);

#define IPv4(a, b, c, d)                                                     \
    ((uint32_t)(((a)&0xff) << 24) | (((b)&0xff) << 16) | (((c)&0xff) << 8) | \
     ((d)&0xff))

struct config_vector_t {
    const char *name;
    int (*f)(void);
};

static struct config_vector_t config_tbl[] = {
    {"host_addr", parse_host_addr},
    {"port", parse_port},
    {"gateway_addr", parse_gateway_addr},
    {"arp", parse_arp},
    {"devices", parse_devices},
    {"nvme_devices", parse_nvme_devices},
    {"ns_sizes", parse_ns_sizes},
    {"nvme_device_model", parse_nvme_device_model},
    // { "mem_channel", parse_mem_channel},
    {"batch", parse_batch},
    {"loader_path", parse_loader_path},
    {"scheduler", parse_scheduler_mode},
    {NULL, NULL}};

/**
 * IMPORTANT NOTE about the following parsers: libconfig allocates memory on
 * any 'xx_lookup' calls. According to the docs this memory is managed by the
 * lib and freed "when the setting is destroyed or when the setting’s value
 * is changed; the string must not be freed by the caller."
 * FIXME: ensure the above is true.
 */

static int str_to_eth_addr(const char *src, unsigned char *dst) {
    struct eth_addr tmp;

    if (sscanf(src, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &tmp.addr[0], &tmp.addr[1],
               &tmp.addr[2], &tmp.addr[3], &tmp.addr[4], &tmp.addr[5]) != 6)
        return -EINVAL;
    memcpy(dst, &tmp, sizeof(tmp));
    return 0;
}

/**
 * str_to_ip - converts char ip in presentation format to binary format
 * @src: the ip address in presentation format
 * @dst: the buffer to store the 32bit result
 * */
static int str_to_ip_addr(const char *src, unsigned char *dst) {
    uint32_t addr;
    unsigned char a, b, c, d;
    if (sscanf(src, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
        return -EINVAL;
    }
    addr = MAKE_IP_ADDR(a, b, c, d);
    memcpy(dst, &addr, sizeof(addr));
    return 0;
}

uint32_t convert_str_ip_to_uint32(const char *src, uint32_t *out) {
    uint32_t ip;
    unsigned char a, b, c, d;
    if (sscanf(src, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return -EINVAL;
    ip = (a << 24) + (b << 16) + (c << 8) + (d);
    memcpy(out, &ip, sizeof(uint32_t));
    return 0;
}

int parse_cfg_fdir_rules(uint8_t port_id) {
    int ret;
    const config_setting_t *fdir = NULL;
    const config_setting_t *entry = NULL;

    fdir = config_lookup(&cfg, "fdir");
    if (!fdir) {
        printf("No FDIR rules defined in config\n");
        return 0;
    }

    int i;
    for (i = 0; i < config_setting_length(fdir); i++) {
        entry = config_setting_get_elem(fdir, i);

        // Parse destination IP address.
        const char *dst_ip_str = NULL;
        uint32_t dst_ip;
        config_setting_lookup_string(entry, "dst_ip", &dst_ip_str);
        if (!dst_ip_str) return -EINVAL;
        if (convert_str_ip_to_uint32(dst_ip_str, &dst_ip)) return -EINVAL;
        printf("\tdst_ip = %lu, ", dst_ip);

        // Parse source IP address.
        const char *src_ip_str = NULL;
        uint32_t src_ip;
        config_setting_lookup_string(entry, "src_ip", &src_ip_str);
        if (!src_ip_str) return -EINVAL;
        if (convert_str_ip_to_uint32(src_ip_str, &src_ip)) return -EINVAL;
        printf("\tsrc_ip = %lu, ", src_ip);

        // Parse destination port.
        uint16_t dst_port;
        config_setting_lookup_int(entry, "dst_port", &dst_port);
        if (!dst_port) return -EINVAL;
        printf("\tdst_port = %u --> ", dst_port);

        // // Parse source port.
        // uint16_t src_port;
        // config_setting_lookup_int(entry, "src_port", &src_port);
        // if (!src_port) return -EINVAL;
        // printf("\tsrc_port = %u --> ", src_port);

        // Parse queue number.
        uint16_t queue;  // Technically queue should be uint8_t, but calling
        config_setting_lookup_int(
            entry, "queue",
            &queue);  // config_setting_lookup_int on that overwrites stack.
        printf("\tqueue %u\n", queue);
        if (queue >= CFG.num_cpus) {
            printf("Skipping invalid fdir at queue %d\n", queue);
            continue;
        }

        uint8_t drop = 0;  // Currently hardcoded to zero, adding drop
                           // field/rules in cfg would be possible.
        uint8_t soft_id = i;

        // Create filter struct.
        struct rte_eth_fdir_filter filter;
        memset(&filter, 0, sizeof(filter));
        filter.soft_id = soft_id;
        filter.input.flow_type = RTE_ETH_FLOW_NONFRAG_IPV4_TCP;
        filter.input.flow.tcp4_flow.ip.src_ip = hton32(src_ip);
        filter.input.flow.tcp4_flow.ip.dst_ip = hton32(dst_ip);
        filter.input.flow.tcp4_flow.src_port = hton16(0);
        // filter.input.flow.tcp4_flow.src_port = hton16(src_port);
        // filter.input.flow.tcp4_flow.dst_port = hton16(0);
        filter.input.flow.tcp4_flow.dst_port = hton16(dst_port);
        filter.action.rx_queue = queue;
        filter.action.behavior =
            drop ? RTE_ETH_FDIR_REJECT : RTE_ETH_FDIR_ACCEPT;
        filter.action.report_status = RTE_ETH_FDIR_REPORT_ID;

        // Apply filter struct.
        ret = rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_FDIR,
                                      RTE_ETH_FILTER_ADD, (void *)&filter);
        if (ret < 0) return ret;
    }
    return 0;
}

static int parse_arp(void) {
    const config_setting_t *arp = NULL, *entry = NULL;
    int i, ret;

    arp = config_lookup(&cfg, "arp");
    if (!arp) {
        log_info("no static arp entries defined in config\n");
        return 0;
    }
    for (i = 0; i < config_setting_length(arp); ++i) {
        const char *ip = NULL, *mac = NULL;
        struct ip_addr ipaddr;
        struct eth_addr macaddr;
        entry = config_setting_get_elem(arp, i);
        config_setting_lookup_string(entry, "ip", &ip);
        config_setting_lookup_string(entry, "mac", &mac);
        if (!ip || !mac) return -EINVAL;
        if (str_to_ip_addr(ip, (void *)&ipaddr)) return -EINVAL;
        if (str_to_eth_addr(mac, (void *)&macaddr)) return -EINVAL;
        ret = arp_insert(&ipaddr, &macaddr);
        if (ret) {
            log_err("cfg: failed to insert static ARP entry.\n");
            return ret;
        }
    }
    return 0;
}

static int parse_gateway_addr(void) {
    char *parsed = NULL;

    config_lookup_string(&cfg, "gateway_addr", (const char **)&parsed);
    if (!parsed) return -EINVAL;
    if (str_to_ip_addr(parsed, (void *)&CFG.gateway_addr)) return -EINVAL;
    return 0;
}

static int add_port(int port) {
    if (port <= 0 || port > 65534) return -EINVAL;
    CFG.ports[CFG.num_ports] = (uint16_t)port;
    ++CFG.num_ports;
    return 0;
}

static int parse_port(void) {
    const config_setting_t *ports = NULL;
    int port, ret;

    ports = config_lookup(&cfg, "port");
    if (!ports) return -EINVAL;
    port = config_setting_get_int(ports);
    if (port) return add_port(port);
    CFG.num_ports = 0;
    while (CFG.num_ports < CFG_MAX_PORTS &&
           CFG.num_ports < config_setting_length(ports)) {
        port = 0;
        port = config_setting_get_int_elem(ports, CFG.num_ports);
        ret = add_port(port);
        if (ret) return ret;
    }
    return 0;
}

static int parse_host_addr(void) {
    char *parsed = NULL, *ip = NULL, *bitmask = NULL;

    config_lookup_string(&cfg, "host_addr", (const char **)&parsed);
    if (!parsed) return -EINVAL;
    /* IP */
    ip = strtok(parsed, "/");
    if (!ip) return -EINVAL;
    if (str_to_ip_addr(ip, (void *)&CFG.host_addr)) return -EINVAL;
    /* netmask */
    bitmask = strtok(NULL, "\0");
    if (!bitmask || !atoi(bitmask)) return -EINVAL;
    CFG.mask = ~(0xFFFFFFFF >> atoi(bitmask));
    /* broadcast */
    CFG.broadcast_addr.addr = CFG.host_addr.addr | ~CFG.mask;
    return 0;
}

static int add_dev(const char *dev) {
    int ret, i;
    struct pci_addr addr;

    ret = pci_str_to_addr(dev, &addr);
    if (ret) {
        log_err("cfg: invalid device name %s\n", dev);
        return ret;
    }
    for (i = 0; i < CFG.num_ethdev; ++i) {
        if (!memcmp(&CFG.ethdev[i], &addr, sizeof(struct pci_addr))) return 0;
    }
    if (CFG.num_ethdev >= CFG_MAX_ETHDEV) return -E2BIG;
    CFG.ethdev[CFG.num_ethdev++] = addr;
    return 0;
}

static int parse_devices(void) {
    const config_setting_t *devs = NULL;
    const char *dev = NULL;
    int i, ret;

    devs = config_lookup(&cfg, "devices");
    if (!devs) return -EINVAL;
    dev = config_setting_get_string(devs);
    if (dev) return add_dev(dev);
    for (i = 0; i < config_setting_length(devs); ++i) {
        dev = NULL;
        dev = config_setting_get_string_elem(devs, i);
        ret = add_dev(dev);
        if (ret) return ret;
    }
    return 0;
}

static int add_nvme_dev(const char *dev) {
    int ret, i;
    struct pci_addr addr;

    ret = pci_str_to_addr(dev, &addr);
    if (ret) {
        log_err("cfg: invalid device name %s\n", dev);
        return ret;
    }
    for (i = 0; i < CFG.num_nvmedev; ++i) {
        if (!memcmp(&CFG.nvmedev[i], &addr, sizeof(struct pci_addr))) return 0;
    }
    if (CFG.num_nvmedev >= CFG_MAX_NVMEDEV) return -E2BIG;
    log_info("------------1CFG.num_ethdev %d\n", CFG.num_ethdev);
    CFG.nvmedev[CFG.num_nvmedev++] = addr;
    log_info("------------2CFG.num_ethdev %d\n", CFG.num_ethdev);
    return 0;
}

static int parse_nvme_devices(void) {
    const config_setting_t *devs = NULL;
    const char *dev = NULL;
    int i, ret;

    devs = config_lookup(&cfg, "nvme_devices");
    if (!devs) return 0;
    dev = config_setting_get_string(devs);
    if (dev) return add_nvme_dev(dev);
    for (i = 0; i < config_setting_length(devs); ++i) {
        dev = NULL;
        dev = config_setting_get_string_elem(devs, i);
        ret = add_nvme_dev(dev);
        if (ret) return ret;
    }
    return 0;
}

static int add_nvme_size(const char *hex_size) {
    log_info("------------1CFG.num_ethdev %d\n", CFG.num_ethdev);
    CFG.ns_sizes[CFG.num_nvmedev++] = (unsigned long)strtol(hex_size, NULL, 0);
    log_info("------------2CFG.num_ethdev %d\n", CFG.num_ethdev);
    if (CFG.num_nvmedev >= CFG_MAX_NVMEDEV) return -E2BIG;
    return 0;
}

static int parse_ns_sizes(void) {
    const config_setting_t *ns_sizes = NULL;
    const char *hex_size = NULL;
    int i, ret;

    ns_sizes = config_lookup(&cfg, "ns_size");
    if (!ns_sizes) return 0;
    hex_size = config_setting_get_string(ns_sizes);
    if (hex_size) return add_nvme_size(hex_size);
    for (i = 0; i < config_setting_length(ns_sizes); ++i) {
        hex_size = NULL;
        hex_size = config_setting_get_string_elem(ns_sizes, i);
        ret = add_nvme_size(hex_size);
        if (ret) return ret;
    }
    return 0;
}

int compare_lat_tokenrate(const void *a, const void *b) {
    struct lat_tokenrate_pair *a_pair = (struct lat_tokenrate_pair *)a;
    struct lat_tokenrate_pair *b_pair = (struct lat_tokenrate_pair *)b;
    return (a_pair->p95_tail_latency - b_pair->p95_tail_latency);
}

static int parse_nvme_device_model(void) {
    config_setting_t *devs = NULL;
    const char *dev_model_ = NULL;
    config_setting_t *read_cost;
    config_setting_t *write_cost;
    config_setting_t *max_token_rate;
    config_setting_t *token_limits = NULL, *entry = NULL;
    int i;

    devs = config_lookup(&cfg, "nvme_device_model");
    if (!devs) {
        g_nvme_dev_model = DEFAULT_FLASH;
        MAX_DEV_TOKEN_RATE = UINT_MAX;
        return 0;
    }

    dev_model_ = config_setting_get_string(devs);
    if (!strcmp(dev_model_, "fake")) {
        log_info(
            "NVMe device model: FAKE_FLASH (fake I/O completion events)\n");
        g_nvme_dev_model = FAKE_FLASH;
        NVME_READ_COST = 100;    // default read cost
        NVME_WRITE_COST = 2000;  // default write cost
        return 0;
    }
    if (!strcmp(dev_model_, "default")) {
        log_info("NVMe device model: DEFAULT_FLASH (no limit)\n");
        g_nvme_dev_model = DEFAULT_FLASH;
        return 0;
    }

    g_nvme_dev_model = FLASH_DEV_MODEL;

    strncpy(devmodel_file, dev_model_, sizeof(devmodel_file));
    devmodel_file[sizeof(devmodel_file) - 1] = '\0';
    g_dev_model_size = 1;

    config_init(&cfg_devmodel);
    if (!config_read_file(&cfg_devmodel, devmodel_file)) {
        fprintf(stderr, "%s:%d - %s\n", config_error_file(&cfg_devmodel),
                config_error_line(&cfg_devmodel),
                config_error_text(&cfg_devmodel));
        config_destroy(&cfg_devmodel);
        return -EINVAL;
    }

    log_info("NVMe device model: %s\n", devmodel_file);
    // parse device request costs
    read_cost = config_lookup(&cfg_devmodel, "read_cost_4KB");
    write_cost = config_lookup(&cfg_devmodel, "write_cost_4KB");
    max_token_rate = config_lookup(&cfg_devmodel, "max_token_rate");
    if (config_setting_get_int(read_cost)) {
        NVME_READ_COST = config_setting_get_int(read_cost);
    } else {
        NVME_READ_COST = 100;  // default read cost
        log_info("WARNING: no read cost specified. Default is 100 tokens.");
    }
    if (config_setting_get_int(write_cost)) {
        NVME_WRITE_COST = config_setting_get_int(write_cost);
    } else {
        log_info("WARNING: no write cost specified. Default is 2000 tokens.");
        NVME_WRITE_COST = 2000;  // default write cost
    }

    // parse token limits and store in memory for lookup during runtime
    if (config_setting_get_int(max_token_rate)) {
        MAX_DEV_TOKEN_RATE = config_setting_get_int(max_token_rate);
    } else {
        MAX_DEV_TOKEN_RATE = UINT_MAX;  // default max token rate
    }

    token_limits = config_lookup(&cfg_devmodel, "token_limits");
    if (!token_limits) {
        log_info("WARNING: return no token limits specified.\n");
        return 0;
    }

    g_dev_model_size = config_setting_length(token_limits);
    assert(g_dev_model_size <
           128);  // if this fails, increase size of dev_model array in cfg.h

    for (i = 0; i < config_setting_length(token_limits); ++i) {
        int lat = 0;
        long long token_rate_limit, token_rdonly_rate_limit = 0;
        entry = config_setting_get_elem(token_limits, i);
        config_setting_lookup_int(entry, "p95_latency_limit", &lat);
        config_setting_lookup_int64(entry, "max_token_rate", &token_rate_limit);
        config_setting_lookup_int64(entry, "max_rdonly_token_rate",
                                    &token_rdonly_rate_limit);
        if (!lat || !token_rate_limit) return -EINVAL;

        g_dev_models[i].p95_tail_latency = lat;
        g_dev_models[i].token_rate_limit = (unsigned long)token_rate_limit;

        if (!token_rdonly_rate_limit) {
            g_dev_models[i].token_rdonly_rate_limit =
                (unsigned long)token_rate_limit;
        } else {
            g_dev_models[i].token_rdonly_rate_limit =
                (unsigned long)token_rdonly_rate_limit;
        }
    }
    // sort dev_model array for easy lookup during runtime
    qsort(g_dev_models, g_dev_model_size, sizeof(struct lat_tokenrate_pair),
          &compare_lat_tokenrate);

    // log_info("WARNING: only support 1 device type for now\n");
    return 0;
}

static int parse_scheduler_mode(void) {
    const config_setting_t *sched = NULL;
    const char *sched_mode = NULL;

    sched = config_lookup(&cfg, "scheduler");

    if (!sched) {
        g_nvme_sched_mode = NO_SCHED;
        return 0;
    }
    sched_mode = config_setting_get_string(sched);
    if (sched_mode) {
        if (!strcmp(sched_mode, "reflex")) {
            g_nvme_sched_mode = REFLEX;
            log_info("I/O Scheduler: ReFlex\n");
        } else if (!strcmp(sched_mode, "reflexrr")) {
            g_nvme_sched_mode = REFLEX_RR;
            log_info("I/O Scheduler: ReFlex RR\n");
        } else if (!strcmp(sched_mode, "wfq")) {
            g_nvme_sched_mode = WFQ;
            log_info("I/O Scheduler: WFQ\n");
        } else if (!strcmp(sched_mode, "wdrr")) {
            g_nvme_sched_mode = WDRR;
            log_info("I/O Scheduler: WDRR\n");
        } else if (!strcmp(sched_mode, "lessv0")) {
            g_nvme_sched_mode = LESSv0;
            log_info("I/O Scheduler: LESSv0\n");
        } else if (!strcmp(sched_mode, "lessv1")) {
            g_nvme_sched_mode = LESSv1;
            log_info("I/O Scheduler: LESSv1\n");
        } else if (!strcmp(sched_mode, "lessv2")) {
            g_nvme_sched_mode = LESSv2;
            log_info("I/O Scheduler: LESSv2\n");
        } else if (!strcmp(sched_mode, "off")) {
            g_nvme_dev_model = DEFAULT_FLASH;
            g_nvme_sched_mode = NO_SCHED;
            log_info("I/O Scheduler: OFF (and using DEFAULT FLASH)\n");
        } else {
            log_info("Default: LESSv0 scheduler on\n");
            g_nvme_sched_mode = LESSv0;
        }
        return 0;
    }
    log_info("WARNING: only support 1 device type for now\n");
    return 0;
}

static int add_cpu(int cpu) {
    int i;

    if (cpu < 0 || cpu >= cpu_count) {
        log_err("cfg: cpu %d is invalid (min:0 max:%d)\n", cpu, cpu_count);
        return -EINVAL;
    }
    for (i = 0; i < CFG.num_cpus; i++) {
        if (CFG.cpu[i] == cpu) return 0;
    }
    if (CFG.num_cpus >= CFG_MAX_CPU) return -E2BIG;
    CFG.cpu[CFG.num_cpus++] = (uint32_t)cpu;
    return 0;
}

static int parse_cpu(void) {
    int i, ret, cpu = -1;
    config_setting_t *cpus = NULL;

    cpus = config_lookup(&cfg, "cpu");
    if (!cpus) {
        return -EINVAL;
    }
    if (!config_setting_get_elem(cpus, 0)) {
        g_cores_active = 1;
        cpu = config_setting_get_int(cpus);
        return add_cpu(cpu);
    }
    for (i = 0; i < config_setting_length(cpus); ++i) {
        cpu = config_setting_get_int_elem(cpus, i);
        ret = add_cpu(cpu);
        if (ret) {
            return ret;
        }
    }

    g_cores_active = config_setting_length(cpus);

    return 0;
}

int cfg_parse_mem(void) {
    int mem_channel = -1;
    config_lookup_int(&cfg, "mem_channel", &mem_channel);
    if (!mem_channel || mem_channel < -1) {
        return -EINVAL;
    }
    g_dpdk_mem_channel = mem_channel;
    return 0;
}

static int parse_batch(void) {
    int batch = -1;
    config_lookup_int(&cfg, "batch", &batch);
    if (!batch || batch <= 0) {
        return -EINVAL;
    }
    eth_rx_max_batch = batch;
    return 0;
}

static int parse_loader_path(void) {
    char *parsed = NULL;

    config_lookup_string(&cfg, "loader_path", (const char **)&parsed);
    if (!parsed) return -EINVAL;
    strncpy(CFG.loader_path, parsed, sizeof(CFG.loader_path));
    CFG.loader_path[sizeof(CFG.loader_path) - 1] = '\0';
    return 0;
}

static int parse_conf_file(const char *path) {
    int ret, i;

    for (i = 0; config_tbl[i].name; ++i) {
        if (config_tbl[i].f) {
            ret = config_tbl[i].f();
            if (ret) {
                log_err("error parsing parameter '%s'\n", config_tbl[i].name);
                config_destroy(&cfg);
                return ret;
            }
        }
    }
    return 0;
}

static int parse_conf_cpu(const char *path) {
    int ret;

    log_info("using config :'%s'\n", path);
    config_init(&cfg);
    if (!config_read_file(&cfg, path)) {
        fprintf(stderr, "%s:%d - %s\n", config_error_file(&cfg),
                config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return -EINVAL;
    }

    ret = parse_cpu();
    if (ret) {
        log_err("error parsing cpu\n");
        config_destroy(&cfg);
        return ret;
    }

    return 0;
}

static void usage(char *argv[]) {
    fprintf(stderr,
            "Usage : %s [option] -- ...\n"
            "\n"
            "Options\n"
            "--config|-c [CONFIG_FILE]\n"
            "\tUse CONFIG_FILE as default config.\n"
            "--log|-l\n"
            "\tSets log level: 0:EMERG, 1:CRIT, 2:ERR, 3:WARN, 4:INFO, "
            "5:DEBUG. Default: 5\n",
            argv[0]);
}

static int parse_arguments(int argc, char *argv[], int *args_parsed) {
    int c, ret;
    static struct option long_options[] = {
        {"config", required_argument, NULL, 'c'},
        {"log", required_argument, NULL, 'l'},
        {NULL, 0, NULL, 0}};
    static const char *optstring = "c:l:";

    while (true) {
        c = getopt_long(argc, argv, optstring, long_options, NULL);
        if (c == -1) break;

        switch (c) {
            case 'c':
                strncpy(config_file, optarg, sizeof(config_file));
                config_file[sizeof(config_file) - 1] = '\0';
                break;
            case 'l':
                if (atoi(optarg) < 0 || atoi(optarg) > 5) {
                    fprintf(stderr, "cfg: invalid log parameter");
                    ret = -EINVAL;
                    goto fail;
                }
                max_loglevel = atoi(optarg);
                break;
            default:
                fprintf(stderr, "cfg: invalid command option %x\n", c);
                ret = -EINVAL;
                goto fail;
        }
    }
    *args_parsed = optind;
    return 0;

fail:
    usage(argv);
    return ret;
}

/**
 * cfg_init - parses configuration arguments and files
 * @argc: the number of arguments
 * @argv: the argument vector
 * @args_parsed: a pointer to store the number of arguments parsed
 *
 * Returns 0 if successful, otherwise fail.
 */
int cfg_init(int argc, char *argv[], int *args_parsed) {
    int ret;
    sprintf(config_file, DEFAULT_CONF_FILE);

    ret = parse_conf_file(config_file);
    if (ret) return ret;
    ret = net_cfg();
    if (ret) return ret;
    return 0;
}

int cfg_parse_cpu(int argc, char *argv[], int *args_parsed) {
    int ret;
    sprintf(config_file, DEFAULT_CONF_FILE);
    /*
        ret = parse_arguments(argc, argv, args_parsed);
        if (ret)
                return ret;
        */
    ret = parse_conf_cpu(config_file);
    if (ret) return ret;

    return 0;
}
