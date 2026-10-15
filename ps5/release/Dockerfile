# Ubuntu's timestamped archive pins all apt packages, including transitive tools.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55
ARG DEBIAN_FRONTEND=noninteractive
# The minimal base has no CA store; bootstrap this fixed Noble package over
# Ubuntu's signed apt indexes before using the HTTPS-only snapshot service.
RUN apt-get update \
    && cd /tmp && apt-get download ca-certificates=20240203 \
    && dpkg-deb --extract ca-certificates_20240203_all.deb /tmp/ca-bootstrap \
    && cat /tmp/ca-bootstrap/usr/share/ca-certificates/mozilla/*.crt > /tmp/ca-bootstrap.pem \
    && rm -rf /var/lib/apt/lists/* /tmp/ca-bootstrap /tmp/ca-certificates_20240203_all.deb
RUN printf '%s\n' \
      'Types: deb' \
      'URIs: https://snapshot.ubuntu.com/ubuntu/20261001T000000Z/' \
      'Suites: noble noble-updates noble-security' \
      'Components: main universe' \
      'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
      'Check-Valid-Until: no' > /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get -o Acquire::https::CaInfo=/tmp/ca-bootstrap.pem update \
    && apt-get -o Acquire::https::CaInfo=/tmp/ca-bootstrap.pem install -y --no-install-recommends \
      autoconf automake bison build-essential ca-certificates cmake curl file flex \
      gettext git libtool libtool-bin pkg-config rsync unzip wget xz-utils \
      clang-18 clang-19 lld-18 lld-19 llvm-18-dev llvm-19-dev \
      libclang-19-dev libclang-cpp19-dev libclang-rt-19-dev \
      libclc-19 libclc-19-dev libllvmspirvlib-19-dev \
      glslang-tools spirv-tools libdrm-dev libelf-dev libexpat1-dev libzstd-dev \
      libfreetype-dev python3 python3-venv python3-pip zlib1g-dev \
    && rm -rf /var/lib/apt/lists/* /tmp/ca-bootstrap.pem
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir \
      meson==1.9.1 ninja==1.13.0 pillow==11.3.0 pefile==2024.8.26 \
      Mako==1.3.10 MarkupSafe==3.0.3 PyYAML==6.0.2 packaging==25.0 ply==3.11
RUN curl --fail --location \
      https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz \
      --output /tmp/llvm-mingw.tar.xz \
    && echo 'bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21  /tmp/llvm-mingw.tar.xz' | sha256sum -c - \
    && mkdir /opt/llvm-mingw \
    && tar -xJf /tmp/llvm-mingw.tar.xz --strip-components=1 -C /opt/llvm-mingw \
    && rm /tmp/llvm-mingw.tar.xz
ENV PATH=/opt/venv/bin:/usr/lib/llvm-19/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    LLVM_MINGW=/opt/llvm-mingw \
    LLVM_CONFIG=/usr/bin/llvm-config-19 \
    LLVM_BIN=/usr/lib/llvm-19/bin \
    PS5_CLANG=/usr/bin/clang-19 \
    BISON_BIN=/usr/bin \
    SOURCE_DATE_EPOCH=0 \
    LC_ALL=C.UTF-8 TZ=UTC
WORKDIR /sources/ps5-proton/ps5-wow
