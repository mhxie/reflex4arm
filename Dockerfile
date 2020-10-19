FROM ubuntu:16.04
COPY . /root/reflex4arm

WORKDIR /root/reflex4arm

RUN apt-get update && apt-get install -y \
  build-essential automake python-pip libcap-ng-dev gawk pciutils net-tools

RUN apt-get update && apt-get install -y \
  libconfig-dev libnuma-dev libpciaccess-dev pciutils uuid-dev

RUN apt-get install -y \
  libconfig-dev libnuma-dev libpciaccess-dev libaio-dev libevent-dev g++-multilib

RUN apt-get install -y kmod

RUN bash scripts/configure.sh
# RUN bash scripts/precondition.sh
CMD bash ./scripts/run_reflex_server.sh