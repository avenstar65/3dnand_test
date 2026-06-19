ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

RUN native_arch="$(dpkg --print-architecture)" \
    && if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then \
        sed -i "/^Types:/i Architectures: ${native_arch}" /etc/apt/sources.list.d/ubuntu.sources; \
    fi \
    && . /etc/os-release \
    && cat > /etc/apt/sources.list.d/ubuntu-amd64.sources <<EOF
Types: deb
Architectures: amd64
URIs: http://archive.ubuntu.com/ubuntu/
Suites: ${VERSION_CODENAME} ${VERSION_CODENAME}-updates ${VERSION_CODENAME}-backports
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
Architectures: amd64
URIs: http://security.ubuntu.com/ubuntu/
Suites: ${VERSION_CODENAME}-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
RUN dpkg --add-architecture amd64 \
    && apt-get update \
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
    && mkdir -p /opt/rootfs-amd64 \
    && cd /tmp \
    && apt-get download busybox-static:amd64 \
    && dpkg-deb -x busybox-static_*_amd64.deb /opt/rootfs-amd64 \
    && rm -f /tmp/busybox-static_*_amd64.deb \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["/bin/bash"]
