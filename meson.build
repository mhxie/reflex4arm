project('reflex4arm', 'c')

add_global_arguments('-w', language : 'c') # ignore warning for now

# dpdk = dependency('libdpdk')

pci_dma_inc = include_directories('./deps/pcidma')

inc = include_directories('inc')
DPDK_ROOT = 'deps/dpdk/install/usr/local'
DPDK_INC = include_directories(join_paths(DPDK_ROOT, 'include'))
SPDK_ROOT = 'deps/spdk/build/lib'
SPDK_INC = include_directories(join_paths('deps', 'spdk', 'include'))


CFLAGS = ['-fno-dwarf2-cfi-asm', '-fno-asynchronous-unwind-tables', '-O3', '-mavx']
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
subdir('lwip')
subdir('net')
subdir('nvme')
subdir('core')
subdir('libix')
subdir('apps')



executable('dp',
            app_sources+core_sources,
            c_args : CFLAGS,
            include_directories : [inc, pci_dma_inc, ix_inc, SPDK_INC, DPDK_INC],
            link_with: [ix_lib],
            objects: [SPDK_LIBS, DPDK_LIBS],
            dependencies : [core_deps],
            install : true)