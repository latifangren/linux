#!/usr/bin/env bash
set -euo pipefail

ARCH="arm64"
CROSS_COMPILE="aarch64-linux-gnu-"
kversion=$(make kernelversion)
BUILDER="/home/sibondt/Linux/Builder/amlogic-s9xxx-openwrt"
TARGET_DIR="${BUILDER}/make-openwrt/kernel/stable/${kversion}"
DTBS="realtek/rtd1619-x1-prime-c.dtb"

mkdir -p output

# 1. Kernel Configuration
if [ ! -f "output/.config" ]; then
    echo ".config file not found, running realtek_defconfig..."
    make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" realtek_defconfig
    make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" menuconfig
else
    echo ".config file already exists."
    read -p "Do you want to run menuconfig to modify the configuration? (y/N) " -n 1 -r
    echo
    if [[ "${REPLY:-N}" =~ ^[Yy]$ ]]; then
        echo "Running menuconfig..."
        make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" menuconfig
    else
        echo "Skipping menuconfig (default N)."
    fi
fi

# 2. Build Kernel
echo "Starting the kernel build process..."
make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)"

# Ensure the main target directory exists
mkdir -p "${TARGET_DIR}"

# 3. Module Installation
echo "Kernel build finished."
read -p "Do you want to install modules to INSTALL_MOD_PATH? (y/N) " -n 1 -r
echo
if [[ "${REPLY:-N}" =~ ^[Yy]$ ]]; then
    echo "Running modules_install..."
    rm -f "${TARGET_DIR}/lib" >/dev/null 2>&1 || echo "No lib dir..."
    make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)" modules_install \
        INSTALL_MOD_PATH="${TARGET_DIR}" \
        INSTALL_MOD_STRIP=1
else
    echo "Skipping module installation."
fi

# 4. Deploy (Copy Build Files)
read -p "Do you want to copy the build files (Image, dtb, .config, System.map) to the target directory? (y/N) " -n 1 -r
echo
if [[ "${REPLY:-N}" =~ ^[Yy]$ ]]; then
    echo "Copying files to ${TARGET_DIR} ..."
    cp output/System.map "${TARGET_DIR}/System.map-${kversion}"
    cp output/.config "${TARGET_DIR}/config-${kversion}"
    cp output/arch/arm64/boot/Image "${TARGET_DIR}/vmlinuz-${kversion}"
    
    # Check if dtb file was successfully created before copying to avoid 'set -e' crashes
    if [ -f "output/arch/arm64/boot/dts/${DTBS}" ]; then
        cp output/arch/arm64/boot/dts/${DTBS} "${TARGET_DIR}/"
    else
        echo "Warning: dtb file not found in the output directory!"
    fi
    echo "Deployment finished."
else
    echo "Skipping deployment (default N)."
fi

# 5. Clean up Directories & Symlinks
# Using 'rm -rf' and '|| true' so the script doesn't die if the folder doesn't exist
rm -rf "${TARGET_DIR}/lib/modules/${kversion}/lib" >/dev/null 2>&1 || true
# (Optional but recommended) Remove 'build' and 'source' symlinks that bloat the compressed archive
rm -f "${TARGET_DIR}/lib/modules/${kversion}/build" >/dev/null 2>&1 || true
rm -f "${TARGET_DIR}/lib/modules/${kversion}/source" >/dev/null 2>&1 || true



# 6. Create Tarball Archives
echo "Creating tarball archives..."
cd "${TARGET_DIR}"

cp -f ../uInitrd-arm64 uInitrd-${kversion}

# Module Archive (Navigate into the modules folder so paths inside the tarball are clean)
if [ -d "lib/modules/${kversion}" ]; then
    cd lib/modules
    tar czf "../../modules-${kversion}.tar.gz" "${kversion}"
    cd ../..
else
    echo "Warning: Module directory not found, skipping module archive."
fi

# Boot & Config Archive (All files ending in -kversion)
tar czf "boot-${kversion}.tar.gz" *"-${kversion}" 2>/dev/null || echo "No boot files to archive."

# DTB Archive
tar czf "dtb-realtek-${kversion}.tar.gz" *.dtb 2>/dev/null || echo "No dtb files to archive."

# remove files
rm -r lib/ 2>/dev/null || echo "No lib folder"
rm *"-${kversion}" || echo "No trash"
rm *.dtb 2>/dev/null || echo "No dtb files"


echo "✅ All processes have been executed successfully!"