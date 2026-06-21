#!/usr/bin/env python3

# Realtek tethered Kernel booter (@sib0ndt)
import serial
import sys

SERIAL_PORT = "/dev/ttyUSB0"
BAUDRATE = 115200

ESC = b"\x1b"
ENTER = b"\r"

PROMPT = "Realtek>"
CHAIN_PROMPT = "RTD1619>"

STAGE1_COMMANDS = [
    "usb start",
    "fatload usb 0:1 0x01500000 u-boot.bin",
    "fatload usb 0:1 0x03000000 Image",
    "fatload usb 0:1 0x02100000 dtb/realtek/rtd1619-x1-prime-c.dtb",
    "fatload usb 0:1 0x02200000 uInitrd",
]

STAGE2_COMMANDS = [
    "setenv bootargs 'console=ttyS0,115200 earlycon=uart8250,mmio32,0x98007800 loglevel=8 ignore_loglevel rootwait rw root=LABEL=ROOTFS' nosmp",
    "booti 0x03000000 0x02200000:0x1b8498 0x02100000",
]


def read_and_print(ser):
    data = ser.read(1)
    if not data:
        return ""

    text = data.decode("utf-8", errors="replace")
    sys.stdout.write(text)
    sys.stdout.flush()
    return text


def wait_for_text(ser, target, buffer_limit=32768):
    buf = ""
    target_lower = target.lower()

    while True:
        text = read_and_print(ser)
        if not text:
            continue

        buf += text

        if len(buf) > buffer_limit:
            buf = buf[-buffer_limit:]

        if target_lower in buf.lower():
            return buf


def send_raw(ser, data):
    ser.write(data)
    ser.flush()


def send_command_wait_prompt(ser, cmd, prompt):
    print(f"\n>>> {cmd}", flush=True)

    ser.write(cmd.encode("utf-8") + ENTER)
    ser.flush()

    wait_for_text(ser, prompt)


def keep_logging(ser, target_log=None, max_occurrences=0):
    buf = ""
    target_lower = target_log.lower() if target_log else None
    occurrences = 0

    while True:
        text = read_and_print(ser)
        if not text:
            continue

        if target_log:
            buf += text

            if len(buf) > 4096:
                buf = buf[-1024:]

            if target_lower in buf.lower():
                occurrences += 1
                
                buf = ""

                if occurrences >= max_occurrences:
                    print("\n[EXIT]", flush=True)
                    break


def main():
    print(f"Realtek Kernel Booter by @sib0ndt")

    with serial.Serial(
        port=SERIAL_PORT,
        baudrate=BAUDRATE,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.1,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False,
    ) as ser:

        ser.reset_input_buffer()
        ser.reset_output_buffer()
        wait_for_text(ser, "welcome to lk/MP")
        send_raw(ser, ESC)
        wait_for_text(ser, "PCPU_FW_START")
        send_raw(ser, ENTER)
        wait_for_text(ser, PROMPT)

        for cmd in STAGE1_COMMANDS:
            send_command_wait_prompt(ser, cmd, prompt=PROMPT)

        print("\n>>> chain 0x01500000")
        ser.write(b"chain 0x01500000" + ENTER)
        ser.flush()

        wait_for_text(ser, CHAIN_PROMPT)

        for cmd in STAGE2_COMMANDS:
            if cmd.startswith("booti "):
                print(f"\n>>> {cmd}", flush=True)
                ser.write(cmd.encode("utf-8") + ENTER)
                ser.flush()
            else:
                send_command_wait_prompt(ser, cmd, prompt=CHAIN_PROMPT)

        target_message = "r8169 98016000.gmac eth0: link up"
        keep_logging(ser, target_log=target_message, max_occurrences=3)


if __name__ == "__main__":
    try:
        main()
        sys.exit(0)
    except KeyboardInterrupt:
        print("\n[EXIT] Stopped by user.")
    except serial.SerialException as e:
        print(f"\n[ERROR] Serial error: {e}")
        sys.exit(1)