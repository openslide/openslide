#!/usr/bin/env bash

# Fetch the archive from github
wget https://github.com/sderaedt/openslide/archive/v3.4.3.tar.gz

# Uncompress archive
tar -zxvf v3.4.3.tar.gz 

# Clone debian package
git clone https://salsa.debian.org/med-team/openslide.git

# Copy debian package into source code directory
cp -r openslide/debian/ openslide-3.4.3/
cd openslide-3.4.3/

# Change the debhelper version
echo 9 > debian/compat 
sed -i "s/Build-Depends: debhelper (>= .*),/Build-Depends: debhelper (>= 9~),/g" debian/control

# Add the changelog
vi debian/changelog

# Run configure to generate the necessary files 
autoreconf -i
./configure 
make

# Create the source archive
tar -zcvf ../openslide_3.4.3.orig.tar.gz ../openslide-3.4.3/

# Build package
debuild -us -uc

# Copy to release folder
cp ../*openslide*.deb ../release/
