# Change to the folder mapped into the container
cd /build

# Clone or copy the openslide version to be built
git clone https://github.com/sderaedt/openslide.git
cd openslide

# Generate the configuration file and run configure
autoreconf -i && ./configure && make distcheck -j8

# Copy compiled file to release folder mapped in docker image
cp openslide-*.tar.gz ../release/