core_sources += ['cfg.c',
    'mbuf.c', 'mempool.c',
    'pci.c','cpu.c',
    'ethfg.c', 'ethqueue.c',
    'tailqueue.c',
    'utimer.c', 'timer.c',
    'control_plane.c', 'log.c',
    'dpdk.c', 'syscall.c'  
]

if get_option('ENABLE_KSTATS')
    core_sources += ['kstats.c']
endif


DPDK_ROOT = '../deps/dpdk/install/usr/local'
DPDK_INC = include_directories(join_paths(DPDK_ROOT, 'include'))
DPDK_LIB_NAMES = [
    # 'librte_pmd_bnxt.a',
    'librte_mempool_ring.a', 'librte_mempool.a', 'librte_mbuf.a',
    'librte_ethdev.a', 'librte_hash.a', 'librte_ring.a', 'librte_eal.a', 'librte_timer.a', 'librte_telemetry.a',
    'librte_bus_pci.a', 'librte_pci.a', 'librte_kvargs.a'
]
DPDK_LIBS = []
foreach NAME : DPDK_LIB_NAMES
    DPDK_LIBS += join_paths(DPDK_ROOT, 'lib', NAME)
endforeach


DPDK_MACHINE_FLAGS = [
    '-DRTE_MACHINE_CPUFLAG_SSE',
    '-DRTE_MACHINE_CPUFLAG_SSE2',
    '-DRTE_MACHINE_CPUFLAG_SSE3',
    '-DRTE_MACHINE_CPUFLAG_SSSE3',
    '-DRTE_MACHINE_CPUFLAG_SSE4_1',
    '-DRTE_MACHINE_CPUFLAG_SSE4_2',
    '-DRTE_MACHINE_CPUFLAG_AES',
    '-DRTE_MACHINE_CPUFLAG_PCLMULQDQ',
    '-DRTE_MACHINE_CPUFLAG_AVX',
    '-DRTE_MACHINE_CPUFLAG_RDRAND',
    '-DRTE_MACHINE_CPUFLAG_FSGSBASE',
    '-DRTE_MACHINE_CPUFLAG_F16C',
    '-DRTE_MACHINE_CPUFLAG_AVX2'
]

SPDK_INC = include_directories(join_paths('../deps', 'spdk', 'include'))
SPDK_ROOT = '../deps/spdk/build/lib'
SPDK_ENV_ROOT = '../deps/dpdk/install/usr/local/lib/'
SPDK_LIBS = [
    join_paths(SPDK_ROOT, 'libspdk_nvme.a'),
    join_paths(SPDK_ROOT, 'libspdk_util.a'),
    join_paths(SPDK_ROOT, 'libspdk_env_dpdk.a'),
    join_paths(SPDK_ROOT, 'libspdk_log.a'),
    join_paths(SPDK_ROOT, 'libspdk_sock.a'),
    # join_paths(SPDK_ENV_ROOT, 'librte_bus_pci.a'),
    # join_paths(SPDK_ENV_ROOT, 'librte_pci.a'),
    # join_paths(SPDK_ENV_ROOT, 'librte_kvargs.a')
]
SPDK_FLAGS = ['-DSPDK_STRING_H']

# core_deps = [dpdk]
core_deps = []
core_deps += dependency('threads')
core_deps += dependency('pciaccess')
core_deps += dependency('uuid')

cc = meson.get_compiler('c')
LDLIBS = ['m', 'rt', 'numa', 'dl', 'config']
foreach LIB : LDLIBS
    dep = cc.find_library(LIB, required : false)
    core_deps += dep
endforeach

core_lib = library('core',
            core_sources,
            c_args : CFLAGS + SPDK_FLAGS,
            include_directories : [inc, pci_dma_inc, DPDK_INC, SPDK_INC],
            objects : [SPDK_LIBS, DPDK_LIBS], # must with this order
            dependencies : core_deps)