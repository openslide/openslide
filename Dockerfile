FROM ubuntu:18.04
LABEL maintainer="KML VISION, devops@kmlvision.com"

RUN apt-get update -qq && \
    apt-get install -y \
      wget \
      libz-dev \
      libjpeg-dev \
      libopenjp2-7-dev \
      libtiff-dev \
      libglib2.0-dev \
      libcairo-dev \
      libpng-dev \
      libgdk-pixbuf2.0-dev \
      libxml2-dev \
      libsqlite3-dev \
      libzip-dev \
      valgrind \
      autoconf \
      automake \
      libtool \
      pkg-config \
      cmake


# install libzip > 1.1 (required for VMIC support)
RUN cd /tmp && wget https://libzip.org/download/libzip-1.5.1.tar.gz && tar xzf libzip-1.5.1.tar.gz && cd libzip-1.5.1 && mkdir build && cd build && cmake .. && make && make install

RUN mkdir -p /opt/openslide
COPY . /opt/openslide
WORKDIR /opt/openslide

RUN autoreconf -i && ./configure && make && make install && ldconfig
# test if it worked
RUN openslide-show-properties --version

CMD ["/bin/bash"]