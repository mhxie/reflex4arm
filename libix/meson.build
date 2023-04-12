ix_sources = [
    'main.c', 'ixev.c' # , 'ixev_timer.c'
]
ix_inc = include_directories('.')

EAL_INC = include_directories(join_paths('..', DPDK_ROOT, 'include'))
EAL_LIBS = join_paths('..', DPDK_ROOT, 'lib', 'librte_eal.a')

ix_lib = static_library('ix',
                    ix_sources,
                    c_args : ['-fno-omit-frame-pointer', '-O3', '-march=native'],
                    include_directories : [inc, ix_inc, EAL_INC],
                    objects: EAL_LIBS,)
