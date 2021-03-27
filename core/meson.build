# core_sources += ['cfg.c',
#     'mbuf.c', 'mempool.c',
#     'pci.c','cpu.c',
#     'ethfg.c', 'ethqueue.c',
#     'tailqueue.c',
#     'utimer.c', 'timer.c',
#     'control_plane.c', 'log.c',
#     'dpdk.c', 'syscall.c'  
# ]
core_sources += files('cfg.c',
    'mbuf.c', 'mempool.c',
    'pci.c','cpu.c',
    'ethfg.c', 'ethqueue.c',
    'tailqueue.c',
    'utimer.c', 'timer.c',
    'control_plane.c', 'log.c',
    'dpdk.c', 'syscall.c'  
)

if get_option('ENABLE_KSTATS')
    core_sources += ['kstats.c']
endif




DPDK_LIB_NAMES = [
    # 'librte_pmd_bnxt.a',
    # 'librte_pmd_ixgbe.a',
    # 'librte_pmd_ena.a',
    # 'librte_mempool_ring.a', 'librte_mempool.a',
    'librte_mbuf.a',
    'librte_ethdev.a', 'librte_hash.a', 'librte_ring.a', 'librte_eal.a',
    'librte_telemetry.a',
    'librte_bus_pci.a', 'librte_pci.a', 'librte_kvargs.a'
]
DPDK_LIBS = []
foreach NAME : DPDK_LIB_NAMES
    DPDK_LIBS += join_paths(DPDK_ROOT, 'lib', NAME)
endforeach

dpdk_whole_libs = ['librte_mempool_ring.a', 'librte_mempool.a']
whole_deps = []
foreach NAME : dpdk_whole_libs
    DPDK_WHOLE_LIB = join_paths('..', DPDK_ROOT, 'lib', NAME)
    whole_deps += declare_dependency(link_args : 
                [ '-Wl,--whole-archive'
                , DPDK_WHOLE_LIB
                , '-Wl,--no-whole-archive'
                ])
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

SPDK_LIB_NAMES = [
    'libspdk_nvme.a',
    'libspdk_util.a',
    'libspdk_env_dpdk.a',
    'libspdk_log.a',
    'libspdk_sock.a',
]
SPDK_LIBS = []
foreach NAME : SPDK_LIB_NAMES
    SPDK_LIBS += join_paths(SPDK_ROOT, 'lib', NAME)
endforeach
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

# core_lib = library('core',
#             core_sources,
#             c_args : CFLAGS + SPDK_FLAGS,
#             include_directories : [inc, pci_dma_inc, DPDK_INC, SPDK_INC],
#             objects : [SPDK_LIBS, DPDK_LIBS], # must with this order
#             dependencies : core_deps)