ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        bc \
        binutils \
        bison \
        busybox-static \
        build-essential \
        ca-certificates \
        cpio \
        curl \
        dwarves \
        file \
        flex \
        gdb \
        gdb-multiarch \
        gcc-x86-64-linux-gnu \
        g++-x86-64-linux-gnu \
        git \
        kmod \
        libelf-dev \
        libncurses-dev \
        libssl-dev \
        mtd-utils \
        openssl \
        perl \
        python3 \
        qemu-system-x86 \
        rsync \
        strace \
        tar \
        xz-utils \
        zstd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["/bin/bash"]
