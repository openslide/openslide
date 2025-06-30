#!/bin/bash

set -e

# Always use the upstream cache, even in forks
GH_USERNAME=openslide
FEED_URL="https://nuget.pkg.github.com/${GH_USERNAME}/index.json"

export VCPKG_BINARY_SOURCES="clear;nuget,${FEED_URL},readwrite"
export X_VCPKG_NUGET_ID_PREFIX="vcpkg-cache"

if [ -z "${GITHUB_TOKEN}" ]; then
    echo "No GITHUB_TOKEN."
    exit 1
fi

nuget sources add -Source "${FEED_URL}" -StorePasswordInClearText \
    -Name GitHubPackages -UserName "${GH_USERNAME}" -Password "${GITHUB_TOKEN}"
nuget setapikey "${GITHUB_TOKEN}" -Source "${FEED_URL}"

vcpkg install \
    pkgconf \
    cairo \
    glib \
    libdicom \
    libjpeg-turbo \
    libpng \
    libxml2 \
    openjpeg \
    sqlite3 \
    tiff \
    zlib \
    zstd

if [ -n "${GITHUB_ENV}" ]; then
    pfx="$VCPKG_INSTALLATION_ROOT\\installed\\x64-windows"
    origpath="$(echo $PATH | sed -e 's|:|;|g' -e 's|/c/|C:\\|g' -e 's|/|\\|g')"
    echo "PKG_CONFIG=${pfx}\\tools\\pkgconf\\pkgconf.exe" >> $GITHUB_ENV
    echo "PKG_CONFIG_PATH=${pfx}\\lib\\pkgconfig;${pfx}\\share\\pkgconfig;${PKG_CONFIG_PATH}" >> $GITHUB_ENV
    echo "PATH=${pfx}\\bin;${pfx}\\tools\\glib;${origpath}" >> $GITHUB_ENV
fi
