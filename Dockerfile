FROM ubuntu:20.04 AS prebuild

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y net-tools libpciaccess-dev

FROM prebuild AS build

RUN apt-get install -y git libconfig-dev
COPY . /reflex4arm

WORKDIR /reflex4arm
RUN git submodule update --init --recursive
RUN ./spdk/scripts/pkgdep.sh
RUN sed -i 's|mempool/ring|mempool/ring net/ena|g' spdk/dpdkbuild/Makefile
RUN sed -i 's|false|true|g' spdk/dpdk/lib/librte_timer/meson.build
# RUN cd spdk && ./configure --with-igb-uio-driver && make && cd ..
RUN cd spdk && ./configure && make && cd ..

RUN meson build && meson compile -C build

FROM prebuild

# copy binary
COPY --from=build /reflex4arm/build/dp /home/dp
# copy dependencies
COPY --from=build /reflex4arm/spdk/build/lib /home/spdk/build/lib
COPY --from=build /reflex4arm/spdk/dpdk/build/lib /home/spdk/dpdk/build/lib
COPY --from=build /reflex4arm/spdk/dpdk/build-tmp/kernel/linux/igb_uio/igb_uio.ko /home/reflex4arm/spdk/dpdk
# copy configurations
COPY --from=build /reflex4arm/ix.conf.sample /home
COPY --from=build /reflex4arm/sample-aws-ec2.devmodel /home
# copy startup script
COPY --from=build /reflex4arm/spdk/scripts/setup.sh /home/spdk
COPY --from=build /reflex4arm/spdk/dpdk/usertools/dpdk-devbind.py /home
COPY --from=build /reflex4arm/usertools/conf_setup.sh /home
COPY --from=build /reflex4arm/usertools/start.sh /home

# WORKDIR /home
# CMD ["./start.sh"]