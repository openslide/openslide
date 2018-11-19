FROM ubuntu:16.04

# Install missing dependencies
RUN apt-get update && apt-get install -qq --no-install-recommends git autoconf automake libtool pkg-config mingw-w64 nasm openjdk-8-jdk ant cmake gettext libglib2.0-0 wget xz-utils build-essential pkg-config python libglib2.0-dev libglib2.0-bin zip autoconf libjpeg-dev libopenjpeg-dev libtiff-dev libglib2.0-dev libcairo-dev libpng-dev libgdk-pixbuf2.0-dev libxml2-dev libsqlite3-dev valgrind libopenjp2-7-dev libgtk2.0-dev doxygen

# Install dependencies for deb package
RUN apt-get update && apt-get install -qq vim dpkg-dev debhelper devscripts curl

# Copy build script to build directory
WORKDIR /build
COPY build_win.sh /build
COPY build_linux.sh /build
COPY build_linux_deb.sh /build

CMD ["bash"]
