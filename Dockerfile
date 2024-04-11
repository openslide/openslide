
# this old openslide has old dependencies, thus the old OS.
from ubuntu:18.04 

ARG DEBIAN_FRONTEND=noninteractive

# Switch to 'root' user
USER root:root

# Update system and install dependencies from apt
RUN apt-get update -y && \
    apt-get upgrade -y && \
    apt-get install -y --allow-downgrades --fix-missing \
    meson \
    libcairo2-dev \
    libgdk-pixbuf2.0-dev \
    glib-2.0-dev \
    libjbig-dev \
    libjpeg-dev \
    libpng-dev \
    libtiff-tools \
    libtiff5-dev \
    libxml2-dev \
    libopenjp2-7-dev \
    libsqlite3-dev \
    zlib1g-dev \
    git \
    python3.10 \
    python3-pip \
    cmake \
    xdelta3 \
    libjpeg-progs \
    autoconf \
    libtool \
    liblcms2-dev \
    gdb \
    ;

# Install dependencies from pip
RUN pip3 install pyaml requests pillow flask openslide-python xdelta3

# Install openslide-python to get sample dzi web server
RUN mkdir /app_py
RUN git clone https://github.com/openslide/openslide-python.git /app_py
RUN cd /app_py && git checkout v1.2.0

# fix python issues for running openslide test suite
RUN apt-get install -y locales && locale-gen en_US.UTF-8
