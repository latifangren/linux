#!/usr/bin/env bash
set -euo pipefail

ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
kversion=$(make kernelversion)
mkdir -p output

if [ ! -f "output/.config" ]; then
    echo "File .config tidak ditemukan, menjalankan realtek_defconfig..."
    make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" realtek_defconfig
    make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" menuconfig
else
    echo "File .config sudah ada."
    read -p "Apakah Anda ingin menjalankan menuconfig untuk mengubah konfigurasi? (y/N) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Menjalankan menuconfig..."
        make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" menuconfig
    else
        echo "Melewati menuconfig (default N)."
    fi
fi

# Build kernel
make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" -j2

# Setelah build berhasil, tawarkan modules_install
echo "Build kernel selesai."
read -p "Apakah Anda ingin menginstal modul ke INSTALL_MOD_PATH? (y/N) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Menjalankan modules_install..."
    make O=output ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" -j2 modules_install \
        INSTALL_MOD_PATH=/home/sibondt/Linux/Builder/amlogic-s9xxx-openwrt/make-openwrt/kernel/stable/"$kversion" \
        INSTALL_MOD_STRIP=1
else
    echo "Melewati instalasi modul."
fi

# Tawarkan deploy (copy file hasil build)
read -p "Apakah Anda ingin menyalin file hasil build (Image, dtb, .config, System.map) ke direktori target? (y/N) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Menyalin file ke /home/sibondt/Linux/Builder/amlogic-s9xxx-openwrt/make-openwrt/kernel/stable/"$kversion" ..."
    cp output/System.map /home/sibondt/Linux/Builder/amlogic-s9xxx-openwrt/make-openwrt/kernel/stable/"$kversion"/System.map-"$kversion"
    cp output/.config /home/sibondt/Linux/Builder/amlogic-s9xxx-openwrt/make-openwrt/kernel/stable/"$kversion"/config-"$kversion"
    cp output/arch/arm64/boot/Image /home/sibondt/Linux/Builder/amlogic-s9xxx-openwrt/make-openwrt/kernel/stable/"$kversion"/vmlinuz-"$kversion"
    cp output/arch/arm64/boot/dts/realtek/rtd1619-x1-prime-c.dtb /home/sibondt/Linux/Builder/amlogic-s9xxx-openwrt/make-openwrt/kernel/stable/"$kversion"
    echo "Deploy selesai."
else
    echo "Melewati deploy (default N)."
fi