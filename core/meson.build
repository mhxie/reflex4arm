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

core_deps = [dpdk]
core_deps += dependency('threads')
core_deps += dependency('pciaccess')
core_deps += dependency('uuid')

cc = meson.get_compiler('c')
LDLIBS = ['m', 'rt', 'numa', 'dl', 'config']
foreach LIB : LDLIBS
    dep = cc.find_library(LIB, required : false)
    core_deps += dep
endforeach

SPDK_INC = include_directories(join_paths('../deps', 'spdk', 'include'))
SPDK_ROOT = '../deps/spdk/build/lib'
SPDK_ENV_ROOT = '../deps/spdk/dpdk/build/lib'
SPDK_LIBS = [
    join_paths(SPDK_ROOT, 'libspdk_nvme.a'),
    join_paths(SPDK_ROOT, 'libspdk_util.a'),
    join_paths(SPDK_ROOT, 'libspdk_env_dpdk.a'),
    join_paths(SPDK_ROOT, 'libspdk_log.a'),
    join_paths(SPDK_ROOT, 'libspdk_sock.a'),
    join_paths(SPDK_ENV_ROOT, 'librte_bus_pci.a'),
    join_paths(SPDK_ENV_ROOT, 'librte_pci.a'),
    join_paths(SPDK_ENV_ROOT, 'librte_kvargs.a')
]
SPDK_FLAGS = ['-DSPDK_STRING_H']

core_lib = library('core',
            core_sources,
            c_args : CFLAGS + DPDK_MACHINE_FLAGS + SPDK_FLAGS,
            include_directories : [inc, pci_dma_inc, SPDK_INC],
            objects : [SPDK_LIBS],
            dependencies : core_deps)