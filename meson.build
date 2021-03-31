project('reflex4arm', 'c')

add_global_arguments('-w', language : 'c') # ignore warning for now

# dpdk = dependency('libdpdk')

SPDK_ROOT = 'spdk/build'
DPDK_ROOT = 'spdk/dpdk/build'

pci_dma_inc = include_directories('./pcidma')
inc = include_directories('inc')
SPDK_INC = include_directories(join_paths('spdk', 'include'))
DPDK_INC = include_directories(join_paths(DPDK_ROOT, 'include'))

CFLAGS = [
    '-fno-dwarf2-cfi-asm',
    '-fno-asynchronous-unwind-tables',
    '-O3',
    '-march=native'
]

if get_option('DEBUG')
    CFLAGS += ['-DDEBUG']
endif

if get_option('MQ_DEBUG')
    CFLAGS += ['-DMQ_DEBUG']
endif

if get_option('CLI_DEBUG')
    CFLAGS += ['-DCLI_DEBUG']
endif

if get_option('ENABLE_KSTATS')
    CFLAGS += ['-DENABLE_KSTATS']
endif

core_sources = []
deps = []
subdir('lwip')
subdir('net')
subdir('nvme')
subdir('core')
subdir('libix')
subdir('apps')

# libdpdk = dependency('libdpdk', method : 'pkg-config')

incs = [inc, pci_dma_inc, ix_inc, SPDK_INC, DPDK_INC]
deps += [core_deps, spdk_deps, dpdk_deps]


executable('dp',
            app_sources+core_sources,
            c_args : CFLAGS,
            # link_args : LDFLAGS,
            include_directories : incs,
            link_with: [ix_lib],
            objects: [SPDK_LIBS, DPDK_LIBS],
            dependencies : deps,
            install : true)