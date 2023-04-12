if get_option('ENABLE_KSTATS')
    core_sources += files('kstats.c')
endif

core_sources += files('cfg.c',
    'mbuf.c', 'mempool.c',
    'pci.c','cpu.c',
    'ethfg.c', 'ethqueue.c',
    'tailqueue.c',
    'utimer.c', 'timer.c',
    'control_plane.c',
    'log.c',
    'dpdk.c', 'syscall.c'  
)


DPDK_LIB_NAMES = [
    # 'librte_ethdev.a',
    # 'librte_mbuf.a',
    # 'librte_eal.a',
    # 'librte_telemetry.a',
    'librte_hash.a',
    #'librte_bus_pci.a',
    #'librte_kvargs.a'
]
DPDK_LIBS = []
foreach NAME : DPDK_LIB_NAMES
    DPDK_LIBS += join_paths(DPDK_ROOT, 'lib', NAME)
endforeach

DPDK_WHOLE_LIB_NAMES = [
    'librte_mempool_ring.a',
    'librte_mempool.a',
    # 'librte_pmd_e1000.a',
    'librte_pmd_ena.a',
    # needed by librte_pmd_e1000.a
    'librte_net.a',
    'librte_ethdev.a',
    'librte_mbuf.a',
    #'librte_eal.a', # dulplicate in IX_LIB
    'librte_telemetry.a',
    # needed by ena librte_pmd_ena.a 
    'librte_timer.a',
    # needed by SPDK
    'librte_ring.a',
    'librte_pci.a',
    'librte_bus_pci.a',
    'librte_kvargs.a',
]

dpdk_deps = []
foreach NAME : DPDK_WHOLE_LIB_NAMES
    DPDK_WHOLE_LIB = join_paths('..', DPDK_ROOT, 'lib', NAME)
    dpdk_deps += declare_dependency(link_args : 
                    [   '-Wl,--whole-archive',
                        DPDK_WHOLE_LIB,
                        '-Wl,--no-whole-archive'])
endforeach

SPDK_LIB_NAMES = [
  #'libspdk_env_dpdk.a',
    # 'libspdk_nvme.a',
    # 'libspdk_util.a',
    # 'libspdk_log.a',
    # 'libspdk_sock.a',
    # 'libspdk_json.a',
]
SPDK_LIBS = []
foreach NAME : SPDK_LIB_NAMES
    SPDK_LIBS += join_paths(SPDK_ROOT, 'lib', NAME)
endforeach

SPDK_WHOLE_LIB_NAMES = [
    'libspdk_nvme.a',
    'libspdk_log.a',
    'libspdk_sock.a',
    'libspdk_json.a',
    'libspdk_jsonrpc.a',
    'libspdk_rpc.a',
    'libspdk_util.a',
    'libspdk_env_dpdk.a',
]
spdk_deps = []
foreach NAME : SPDK_WHOLE_LIB_NAMES
    SPDK_WHOLE_LIB = join_paths('..', SPDK_ROOT, 'lib', NAME)
    spdk_deps += declare_dependency(link_args : 
                    [   '-Wl,--whole-archive',
                        SPDK_WHOLE_LIB,
                        '-Wl,--no-whole-archive'])
endforeach

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

output = run_command('nasm', '-v')
nasm_ver = output.stdout().split(' ')[2]

if nasm_ver >= '2.13.03'
    ISAL_DIR = join_paths('..', 'spdk', 'isa-l', '.libs')
    ISAL_INC = include_directories(join_paths(ISAL_DIR, '..'))
    isal_dep = declare_dependency(
        link_args : ['-L' + ISAL_DIR, '-lisal'],
        include_directories : ISAL_INC)
    core_deps += isal_dep
else
    message('ISA-L requires NASM version 2.13.03 or newer, got', nasm_ver)
endif
