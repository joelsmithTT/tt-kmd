#!/bin/bash

# SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

# Configuration
PACKAGE_NAME="tenstorrent-dkms"
MODULE_NAME="tenstorrent"
MAINTAINER="Tenstorrent <releases@tenstorrent.com>"
HOMEPAGE="https://github.com/tenstorrent/tt-kmd"

# Determine project root (parent of tools/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Parse version from module.h or use argument
get_version() {
    if [[ $# -gt 0 ]]; then
        echo "$1"
        return
    fi
    
    local module_h="${PROJECT_ROOT}/module.h"
    if [[ ! -f "${module_h}" ]]; then
        echo "Error: module.h not found at ${module_h}" >&2
        exit 1
    fi
    
    local major minor patch suffix
    major=$(grep -E '^\s*#define\s+TENSTORRENT_DRIVER_VERSION_MAJOR\s+' "${module_h}" | awk '{print $3}')
    minor=$(grep -E '^\s*#define\s+TENSTORRENT_DRIVER_VERSION_MINOR\s+' "${module_h}" | awk '{print $3}')
    patch=$(grep -E '^\s*#define\s+TENSTORRENT_DRIVER_VERSION_PATCH\s+' "${module_h}" | awk '{print $3}')
    suffix=$(grep -E '^\s*#define\s+TENSTORRENT_DRIVER_VERSION_SUFFIX\s+' "${module_h}" | sed -E 's/.*"(.*)".*/\1/')
    
    if [[ -z "${major}" || -z "${minor}" || -z "${patch}" ]]; then
        echo "Error: Could not parse version from module.h" >&2
        exit 1
    fi
    
    echo "${major}.${minor}.${patch}${suffix}"
}

# Get version
VERSION=$(get_version "$@")
echo "Building ${PACKAGE_NAME} version ${VERSION}"

# Create temporary build directory
BUILD_DIR=$(mktemp -d -t tenstorrent-deb-build.XXXXXX)
trap 'rm -rf "${BUILD_DIR}"' EXIT

PACKAGE_DIR="${BUILD_DIR}/${PACKAGE_NAME}_${VERSION}_all"
DEBIAN_DIR="${PACKAGE_DIR}/DEBIAN"
SRC_DIR="${PACKAGE_DIR}/usr/src/${MODULE_NAME}-${VERSION}"

mkdir -p "${DEBIAN_DIR}"
mkdir -p "${SRC_DIR}"

# Copy source files to package
echo "Copying source files..."
cd "${PROJECT_ROOT}"

# Copy all C source and header files, excluding build artifacts
for file in *.c *.h; do
    # Skip build artifacts
    if [[ "${file}" != "*.c" ]] && [[ "${file}" != "*.h" ]] && [[ "${file}" != "tenstorrent.mod.c" ]]; then
        cp -v "${file}" "${SRC_DIR}/" 2>/dev/null || true
    fi
done

# Copy essential files
for file in Makefile dkms.conf dkms-post-install AKMBUILD \
            LICENSE LICENSE_understanding.txt README.md SUMMARY.md \
            modprobe.d-tenstorrent.conf udev-50-tenstorrent.rules; do
    if [[ -f "${file}" ]]; then
        cp -v "${file}" "${SRC_DIR}/"
    fi
done

# Copy directories
for dir in contrib docs tools test; do
    if [[ -d "${dir}" ]]; then
        cp -rv "${dir}" "${SRC_DIR}/"
    fi
done

# Make dkms-post-install executable
chmod 755 "${SRC_DIR}/dkms-post-install"

# Update dkms.conf with correct version
sed -i "s/PACKAGE_VERSION=\".*\"/PACKAGE_VERSION=\"${VERSION}\"/" "${SRC_DIR}/dkms.conf"

# Create control file
cat > "${DEBIAN_DIR}/control" << EOF
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Section: misc
Priority: optional
Architecture: all
Depends: dkms, linux-headers-generic | linux-headers
Maintainer: ${MAINTAINER}
Homepage: ${HOMEPAGE}
Description: Tenstorrent kernel mode driver (DKMS)
 This package provides the Tenstorrent kernel module as DKMS source.
 The module will be built automatically by DKMS when installed.
EOF

# Create postinst script
cat > "${DEBIAN_DIR}/postinst" << 'EOF'
#!/bin/sh
set -e

# Extract version from control file or dkms.conf
VERSION="__VERSION__"
MODULE_NAME="tenstorrent"

case "$1" in
    configure)
        # Add module to DKMS
        if dkms status -m "${MODULE_NAME}" -v "${VERSION}" | grep -q "${MODULE_NAME}"; then
            echo "DKMS module ${MODULE_NAME}/${VERSION} already added"
        else
            echo "Adding ${MODULE_NAME}/${VERSION} to DKMS"
            dkms add -m "${MODULE_NAME}" -v "${VERSION}" || true
        fi
        
        # Build and install for current kernel
        RUNNING_KERNEL=$(uname -r)
        echo "Building ${MODULE_NAME}/${VERSION} for kernel ${RUNNING_KERNEL}"
        dkms build -m "${MODULE_NAME}" -v "${VERSION}" -k "${RUNNING_KERNEL}" || true
        dkms install -m "${MODULE_NAME}" -v "${VERSION}" -k "${RUNNING_KERNEL}" || true
        
        # Try to load the module if it was built successfully
        if [ -f "/lib/modules/${RUNNING_KERNEL}/updates/dkms/${MODULE_NAME}.ko" ] || \
           [ -f "/lib/modules/${RUNNING_KERNEL}/kernel/extra/${MODULE_NAME}.ko" ] || \
           [ -f "/lib/modules/${RUNNING_KERNEL}/extra/${MODULE_NAME}.ko" ]; then
            echo "Loading ${MODULE_NAME} module"
            modprobe "${MODULE_NAME}" 2>/dev/null || true
        fi
        ;;
esac

#DEBHELPER#

exit 0
EOF

# Replace version placeholder
sed -i "s/__VERSION__/${VERSION}/g" "${DEBIAN_DIR}/postinst"
chmod 755 "${DEBIAN_DIR}/postinst"

# Create prerm script
cat > "${DEBIAN_DIR}/prerm" << 'EOF'
#!/bin/sh
set -e

VERSION="__VERSION__"
MODULE_NAME="tenstorrent"

case "$1" in
    remove|upgrade|deconfigure)
        # Unload module if loaded
        if lsmod | grep -q "^${MODULE_NAME} "; then
            echo "Unloading ${MODULE_NAME} module"
            modprobe -r "${MODULE_NAME}" 2>/dev/null || true
        fi
        
        # Remove from DKMS on package removal (not on upgrade)
        if [ "$1" = "remove" ]; then
            if dkms status -m "${MODULE_NAME}" -v "${VERSION}" | grep -q "${MODULE_NAME}"; then
                echo "Removing ${MODULE_NAME}/${VERSION} from DKMS"
                dkms remove -m "${MODULE_NAME}" -v "${VERSION}" --all || true
            fi
        fi
        ;;
esac

#DEBHELPER#

exit 0
EOF

# Replace version placeholder
sed -i "s/__VERSION__/${VERSION}/g" "${DEBIAN_DIR}/prerm"
chmod 755 "${DEBIAN_DIR}/prerm"

# Build the .deb package
echo "Building .deb package..."
dpkg-deb --build "${PACKAGE_DIR}"

# Move .deb to project root or specified output directory
OUTPUT_DIR="${PROJECT_ROOT}"
if [[ -n "${DEB_OUTPUT_DIR:-}" ]]; then
    OUTPUT_DIR="${DEB_OUTPUT_DIR}"
fi

DEB_FILE="${PACKAGE_NAME}_${VERSION}_all.deb"
mv "${BUILD_DIR}/${DEB_FILE}" "${OUTPUT_DIR}/"

echo ""
echo "Successfully built: ${OUTPUT_DIR}/${DEB_FILE}"
echo ""
echo "Package details:"
dpkg-deb --info "${OUTPUT_DIR}/${DEB_FILE}"
echo ""

#echo "Package contents:"
#dpkg-deb --contents "${OUTPUT_DIR}/${DEB_FILE}"

exit 0

