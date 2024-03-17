
from ubuntu:22.04 

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
    ;

# Install dependencies from pip
RUN pip install pyaml requests