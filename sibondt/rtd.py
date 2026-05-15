import serial
import time
import sys

# --- KONFIGURASI ---
SERIAL_PORT = '/dev/ttyUSB0' 
BAUD_RATE = 115200

def send_cmd_and_wait(ser, cmd, prompt="Realtek>", timeout=15):
    """Kirim perintah dan tunggu sampai prompt U-Boot (Realtek>) muncul lagi"""
    ser.flushInput() 
    print(f"\n[KIRIM] {cmd}")
    ser.write((cmd + '\r\n').encode())
    
    start_time = time.time()
    buffer = ""
    
    while time.time() - start_time < timeout:
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            text = data.decode('utf-8', errors='ignore')
            
            sys.stdout.write(text)
            sys.stdout.flush()
            buffer += text
            
            if prompt in buffer:
                time.sleep(0.2) 
                return True
        time.sleep(0.01)
        
    print(f"\n[!] Waktu habis (Timeout) saat menunggu '{cmd}' selesai.")
    return False


def run_automation():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)
        print(f"[*] Terhubung ke {SERIAL_PORT}. Menunggu trigger...")

        injected = False
        trigger_buffer = ""

        while True:
            # --- FASE 1: MONITORING TRIGGER ---
            if not injected:
                if ser.in_waiting > 0:
                    data = ser.read(ser.in_waiting)
                    text = data.decode('utf-8', errors='ignore')
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    
                    trigger_buffer += text
                    if len(trigger_buffer) > 500:
                        trigger_buffer = trigger_buffer[-500:]

                    if "PCPU_FW_START" in trigger_buffer or "r8168 link up" in trigger_buffer:
                        injected = True
                        print("\n\n[!] TRIGGER DETECTED! Memulai urutan booting aman...")
                        
                        # Pancing prompt dengan Enter agar buffer bersih
                        ser.write(b'\r\n')
                        time.sleep(0.5)

                        # --- FASE 2: LOADING FILE ---
                        send_cmd_and_wait(ser, "usb start", timeout=15)
                        send_cmd_and_wait(ser, "fatload usb 0:1 0x04000000 uInitrd", timeout=10)
                        send_cmd_and_wait(ser, "fatload usb 0:1 0x07f00000 rtd1619-x1-prime-c.dtb", timeout=5)
                        send_cmd_and_wait(ser, "fatload usb 0:1 0x08000000 Image", timeout=25) 
                        send_cmd_and_wait(ser, "fatload usb 0:1 0x05000000 u-boot.bin", timeout=10)

                        # --- FASE 3: CHAIN & INTERRUPT ESC ---
                        print("\n[KIRIM] chain 0x05000000")
                        ser.write(b"chain 0x05000000\r\n")
                        
                        time.sleep(0.3) 
                        
                        print("[*] Mencecar tombol ESC untuk interrupt U-Boot baru...")
                        for _ in range(40):
                            ser.write(b'\x1b')
                            time.sleep(0.05)
                        
                        print("[*] Menunggu prompt U-Boot baru...")
                        wait_start = time.time()
                        wait_buf = ""
                        while time.time() - wait_start < 10:
                            if ser.in_waiting > 0:
                                t = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                                sys.stdout.write(t)
                                sys.stdout.flush()
                                wait_buf += t
                                if "Realtek>" in wait_buf:
                                    break
                            time.sleep(0.01)

                        # --- FASE 4: SETENV & BOOT ---
                        # Kirim perintah bootargs 2 kali sesuai kebutuhan (karakter sering terpotong di awal)
                        bootargs_cmd = "setenv bootargs earlycon=uart8250,mmio32,0x98007800 console=ttyS0,115200 keep_bootcon root=LABEL=rootfs rootwait rw ignore_loglevel loglevel=8"
                        
                        print("\n[*] Menjalankan setenv bootargs (Percobaan 1 - Biasanya terpotong)...")
                        send_cmd_and_wait(ser, bootargs_cmd, timeout=5)
                        
                        print("\n[*] Menjalankan setenv bootargs (Percobaan 2 - Memastikan sukses)...")
                        send_cmd_and_wait(ser, bootargs_cmd, timeout=5)

                        send_cmd_and_wait(ser, "setenv initrd_high 0xffffffffffffffff", timeout=5)
                        send_cmd_and_wait(ser, "fdt addr 0x07f00000", timeout=5)
                        send_cmd_and_wait(ser, "fdt resize 0x1000", timeout=5)
                        
                        # Booting
                        print("\n[KIRIM] booti 0x08000000 0x04000000:1b4061 0x07f00000")
                        ser.write(b"booti 0x08000000 0x04000000:1b4061 0x07f00000\r\n")
                        
                        print("\n[+] Perintah booting terkirim. Anda sekarang berada di terminal Kernel!\n")

            # --- FASE 5: TERMINAL KERNEL ---
            else:
                if ser.in_waiting > 0:
                    sys.stdout.write(ser.read(ser.in_waiting).decode('utf-8', errors='ignore'))
                    sys.stdout.flush()
                time.sleep(0.01)

    except KeyboardInterrupt:
        print("\n[!] Dihentikan.")
    finally:
        if 'ser' in locals():
            ser.close()

if __name__ == "__main__":
    run_automation()