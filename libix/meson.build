ix_sources = [
    'main.c', 'ixev.c', 'ixev_timer.c'
]
ix_inc = include_directories('.')

ix_lib = static_library('ix',
                    ix_sources,
                    c_args : ['-fno-omit-frame-pointer', '-O3', '-march=native'],
                    include_directories : [inc, ix_inc],
                    link_with : core_lib,
                    dependencies : dpdk)