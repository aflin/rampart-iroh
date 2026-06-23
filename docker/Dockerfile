# rampart-iroh "oven" -- builds rampart-iroh.so for the portable
# centos7-x86_64 target (glibc 2.17).  The module is a Rust cdylib/staticlib
# (iroh + tokio + rustls) wrapped by a small C shim; building on CentOS 7 drops
# the glibc floor from whatever the host has (e.g. 2.34) down to 2.17, so the
# module runs on old and new Linux alike.  Nothing is bundled: the finished .so
# needs only glibc + libgcc_s (Rust statically links its std + every crate).
#
# Base = manylinux2014 (glibc 2.17) + devtoolset-11 (gcc 11 for the C shim and
# for the C in ring/aws-lc-sys) + a Rust toolchain (manylinux ships none).
# cmake comes with the manylinux base (aws-lc-sys needs it); perl is for the
# ring/aws-lc assembly codegen.
#
# Self-contained on purpose (its own image, persisted/reused): an ordinary iroh
# change never reinstalls the toolchain.  rampart headers are NOT baked in --
# docker/build.sh bind-mounts /usr/local/rampart and the build reads
# $RAMPART_INCLUDE from `rampart -c process.installPath`.
#
# Built automatically by docker/build.sh; or: docker build -t <tag> docker/
# Target arch: x86_64 (default) or aarch64.  build.sh passes --build-arg ARCH=$(uname -m).
ARG ARCH=x86_64
FROM quay.io/pypa/manylinux2014_${ARCH}

# devtoolset's enable script is a bash-ism; make every RUN a bash shell.
SHELL ["/bin/bash", "-c"]

# gcc 11 (C shim + the C compiled by ring/aws-lc-sys via cc-rs) + perl for their
# assembly codegen.  glibc stays 2.17 (devtoolset links the base glibc).  cmake
# (needed by aws-lc-sys) already ships in the manylinux base image.
ARG ARCH=x86_64
RUN if [ "$ARCH" = "x86_64" ]; then \
        yum install -y devtoolset-11-gcc devtoolset-11-gcc-c++ \
            devtoolset-11-binutils perl-core && yum clean all; \
    else \
        yum install -y perl-core && yum clean all; \
        echo "==> $ARCH: using the image's bundled devtoolset-10 (no devtoolset-11 on CentOS 7 arm)"; \
    fi

# Rust toolchain.  Pinned under /opt/rust so this layer is cached; CARGO_HOME is
# overridden at build time (build-in-oven.sh) to a mounted, persistent cache so
# crate downloads survive across runs.  Uses stable -- pin a version here if you
# ever need a specific rustc.
ENV RUSTUP_HOME=/opt/rust/rustup \
    CARGO_HOME=/opt/rust/cargo \
    PATH=/opt/rust/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --profile minimal --default-toolchain stable && \
    rustc --version && cargo --version

# No ENTRYPOINT: docker/build.sh invokes docker/build-in-oven.sh explicitly.
