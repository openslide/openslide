#Build Stage
FROM --platform=linux/amd64 ubuntu:20.04 as builder

##Install Build Dependencies
RUN apt-get update && \
        DEBIAN_FRONTEND=noninteractive apt-get install -y git clang zlib1g-dev libpng-dev libjpeg-dev libtiff-dev libgdk-pixbuf2.0-dev libxml2-dev sqlite3 libcairo2-dev libglib2.0-dev autoconf automake libtool pkg-config make cmake liblcms2-dev libz-dev libzstd-dev libwebp-dev libsqlite3-dev libopenjp2-tools libopenjp2-7-dev

# Install openjpeg
WORKDIR /
RUN git clone https://github.com/uclouvain/openjpeg.git
WORKDIR /openjpeg/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release
RUN make -j$(nproc)
RUN make install && make clean

##ADD source code to the build stage
WORKDIR /
ADD https://api.github.com/repos/ennamarie19/openslide/git/refs/heads/mayhem version.json
RUN git clone -b mayhem https://github.com/ennamarie19/openslide.git
WORKDIR /openslide


##Build
RUN autoreconf -i
RUN ./configure CC="clang" CXX="clang++" BUILD_FUZZER=1
RUN make -j$(nproc)
RUN make install && ldconfig

##Prepare all library dependencies for copy
RUN mkdir /deps
RUN cp `ldd ./test/.libs/fuzz | grep so | sed -e '/^[^\t]/ d' | sed -e 's/\t//' | sed -e 's/.*=..//' | sed -e 's/ (0.*)//' | sort | uniq` /deps 2>/dev/null || :
#
FROM --platform=linux/amd64 ubuntu:20.04
COPY --from=builder /openslide/test/.libs/fuzz /fuzz
COPY --from=builder /deps /usr/lib

CMD /fuzz

