#!/bin/sh

LIBJXR_PC='
prefix=/usr
libdir=${prefix}/lib/x86_64-linux-gnu
includedir=${prefix}/include

Name: libjxr
Description: A library for reading JPEG XR images.

Version: 1.1
Libs: -L${libdir} -ljpegxr -ljxrglue
Libs.private: -lm
Cflags: -I${includedir}/jxrlib -D__ANSI__ -DDISABLE_PERF_MEASUREMENT
'

. /etc/os-release
CENTOS7_LIB='libdir=${prefix}\/lib64'
VID=$(echo $VERSION_ID | cut -d'.' -f1)

#CentOS7, Debian < Bookworm, and Ubuntu < 22.04LTS are missing libjxr.pc
if [ "$ID" = 'centos' ]; then
    echo "$LIBJXR_PC" | sed "3s/.*/${CENTOS7_LIB}/" > libjxr.pc
elif [ "$ID" = 'debian' ]  && [ "$VID" -lt 12 ]; then
    echo "$LIBJXR_PC" > libjxr.pc
elif [ "$ID" = 'ubuntu' ]  && [ "$VID" -lt 22 ]; then
    echo "$LIBJXR_PC" > libjxr.pc
else
    echo "do nothing"
fi

autoreconf -i
