FROM emscripten/emsdk:3.1.61

WORKDIR /build
COPY . /build
RUN rm -fr builddir build

RUN apt-get update \
  && apt-get install -qqy \
  build-essential \
  prelink \
  autoconf \
  libtool \
  texinfo \
  pkgconf \
  # needed for Meson
  ninja-build \
  python3-pip \
  python2-dev \
  python3-dev \
  libxml2-utils \
  shared-mime-info \
  libglib2.0-dev-bin \
  python3-docutils \
  gettext && pip3 install meson lxml

RUN meson setup builddir --cross-file emscripten-cross.txt --default-library=static
RUN meson compile -C builddir 
