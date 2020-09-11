app_sources = ['init.c', 'reflex_server.c', 'reflex_ix_client.c']

executable('dp',
            app_sources,
            c_args : CFLAGS,
            include_directories : [inc, pci_dma_inc, ix_inc, DPDK_INC],
            link_with: [core_lib, ix_lib],
            objects: DPDK_LIBS,
            dependencies : core_deps,
            install : true)