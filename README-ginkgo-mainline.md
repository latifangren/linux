# Redmi Note 8 (ginkgo) Mainline Extended Device Tree
This directory contains experimental device tree setup for porting advanced hardware functions of **Xiaomi Redmi Note 8 (ginkgo)** device tree to the Linux mainline kernel (v7.1.3).

## Files
- Main DTS: `arch/arm64/boot/dts/qcom/ginkgo-mainline.dts`
- Porting Plan / Roadmap: `.hermes/plans/2026-07-10_000000-ginkgo-mainline-porting.md`

## Summary of Ported Hardware Configurations

| Component | Mainline Compatible | Downstream Mapping (Android) | Mainline Mapping / Registers | Notes |
|---|---|---|---|---|
| **ADSP** | `qcom,sm6115-adsp-pas` | `qcom,lpass@ab00000` | `@ab00000` (length `0x4040`) | Managed via POSIX SMP2P and remoteproc drivers. |
| **CDSP** | `qcom,sm6115-cdsp-pas` | `qcom,turing@b300000` | `@b300000` (length `0x4040`) | Managed via PIL cdsp remoteproc. |
| **MPSS (Modem)**| `qcom,sm6115-mpss-pas` | `qcom,mss@6080000` | `@6080000` (length `0x4040`) | Modem remoteproc interface. |
| **IPA v4.2** | `qcom,sc7180-ipa` | `qcom,ipa@5800000` | Split: `0x5800000`, `0x5830000`, `0x5804000` | Registers mapped separate from standard GSI spacing. |
| **WiFi** | `qcom,wcn3990-wifi` | `qcom,icnss@C800000` | `@c800000` (length `0x800000`) | SMMU SID mapping at `0x80`. |
| **Bluetooth** | `qcom,wcn3990-bt` | `bt_wcn3990` | Attach to `uart9` (`@4c90000`) | Mapped on QUP Wrapper 1 Engine 4 using GPIO pin functions `qup14`. |

## Drivers / SoC Modifications

Untuk mendukung `ginkgo` (SM6125), beberapa driver di `drivers/soc/qcom/` telah disesuaikan agar mengenali hardware platform ini secara native:
- **PD Mapper (`drivers/soc/qcom/qcom_pd_mapper.c`):**
  Menambahkan target `.compatible = "qcom,sm6125"` yang dipetakan ke `sm6115_domains`. Hal ini wajib agar user-space daemon (`pd-mapper`) dan firmware subsystem GLink dapat memetakan domain audio, sensor, cdsp, dan wlan secara dinamis saat subsystem remoteproc di-boot.

- **Remoteproc PAS (`drivers/remoteproc/qcom_q6v5_pas.c`):**
  Menambahkan kompatibilitas remoteproc secure-boot untuk SM6125 (ADSP, CDSP, MPSS) yang dipetakan ke data inisialisasi yang sekeluarga dengan SM6115.

- **IPA Driver (`drivers/net/ipa/ipa_main.c`):**
  Menambahkan compatible `"qcom,sm6125-ipa"` yang dipetakan ke data versi `ipa_data_v4_2` untuk inisialisasi native.

## Status Battery & Charging (Roadmap Masa Depan)

Pada *dump* Android DTS, sistem pengisian daya dan pemantauan baterai Ginkgo dikelola oleh:
1. **Fuel Gauge (Pengukur Baterai):** `qcom,qpnp-qg` (QG gauge)
2. **Main Charger (Pengisi Daya Utama):** `qcom,qpnp-smb5` (charger PMI632)
3. **Companion Charger (Pengisi Daya Tambahan):** `qcom,smb1355`

**Status di Mainline 7.1.3:**
Ketiga driver ini (`qpnp-qg`, `qpnp-smb5`, dan `smb1355`) **belum di-porting atau belum tersedia** di direktori driver mainline kernel (`drivers/power/supply/`).
Oleh karena itu, fungsi monitor persentase baterai dan kontrol pengisian daya belum dapat aktif di mainline secara default tanpa adanya upaya memporting kode C driver tersebut dari kernel lama (LineageOS/Qualcomm vendor kernel) sebagai modul out-of-tree atau patch tambahan.



## Compile Instruction
This SoC device tree can be compiled with:
```bash
make O=.output ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j2 dtbs
```
This generates the binary DTB file under:
`.output/arch/arm64/boot/dts/qcom/ginkgo-mainline.dtb`
