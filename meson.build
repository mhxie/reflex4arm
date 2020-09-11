project('reflex4arm', 'c')

add_global_arguments('-w', language : 'c') # ignore warning for now

dpdk = dependency('libdpdk')

pci_dma_inc = include_directories('./deps/pcidma')

inc = include_directories('inc')

CFLAGS = ['-fno-dwarf2-cfi-asm', '-fno-asynchronous-unwind-tables', '-O3']
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