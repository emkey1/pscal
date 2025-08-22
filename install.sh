#!/bin/sh
set -eu

# Ensure we can write to /usr/local
if [ "$(id -u)" -ne 0 ] && [ ! -w /usr/local ]; then
    echo "Error: must run as root or have write permission to /usr/local" >&2
    exit 1
fi

# Ensure /usr/local/bin exists
mkdir -p /usr/local/bin

# Copy binaries to /usr/local/bin
if [ -d "build/bin" ]; then
    cp -r build/bin/* /usr/local/bin/
else
    echo "Warning: build/bin directory not found; skipping binary installation" >&2
fi

# Install Pascal library
mkdir -p /usr/local/pscal/pascal/lib
cp -r lib/pascal/* /usr/local/pscal/pascal/lib/

# Install C-like library
mkdir -p /usr/local/pscal/clike/lib
cp -r lib/clike/* /usr/local/pscal/clike/lib/

# Install fonts
mkdir -p /usr/local/pscal/fonts
cp -r fonts/* /usr/local/pscal/fonts

# Install etc
mkdir -p /usr/local/pscal/etc
cp -r etc/* /usr/local/pscal/etc
