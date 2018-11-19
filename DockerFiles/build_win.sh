# Change to the folder mapped into the container
cd /build

# Clone the winbuild script and change to the directory
git clone https://github.com/openslide/openslide-winbuild.git
cd openslide-winbuild

# Create the override directory to build a custom openslide version
# Otherwise the last release from the official openslide github will be built
mkdir override
cd override

# Clone or copy the openslide version to be built
git clone https://github.com/sderaedt/openslide.git
cd openslide

# Generate the configuration file and run configure
autoreconf -i && ./configure && make distcheck

# Change back to the root directory and run build.sh bdist
cd ../../
bash build.sh -m64 -j16 bdist

# -m64 specifies to build a 64bit version (-m32 for 32bit)
# -j16 parallel build with 16 threads

# Copy compiled file to release folder mapped in docker image
cp openslide-win64-*.zip ../release/