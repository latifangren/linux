#!/usr/bin/env python3

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
    print(f"\n[WAIT] Waiting for: {target}", flush=True)

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
            print(f"\n[FOUND] {target}", flush=True)
            return buf


def send_raw(ser, data):
    ser.write(data)
    ser.flush()


def send_command_wait_prompt(ser, cmd, prompt):
    print(f"\n>>> {cmd}", flush=True)

    ser.write(cmd.encode("utf-8") + ENTER)
    ser.flush()

    wait_for_text(ser, prompt)


def keep_logging(ser):
    print("\n[LOG] Logging mode active. Press Ctrl+C to exit.\n", flush=True)

    while True:
        read_and_print(ser)


def main():
    print(f"[OPEN] Opening serial port {SERIAL_PORT} @ {BAUDRATE}")

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

        print("[READY] Script is running. Now start/reboot your device.")

        wait_for_text(ser, "welcome to lk/MP")

        print("\n[ACTION] Sending Escape")
        send_raw(ser, ESC)

        wait_for_text(ser, "PCPU_FW_START")

        print("\n[ACTION] Sending Enter")
        send_raw(ser, ENTER)

        wait_for_text(ser, PROMPT)

        print("\n[STAGE 1] Sending initial commands one by one")

        for cmd in STAGE1_COMMANDS:
            send_command_wait_prompt(ser, cmd, prompt=PROMPT)

        print("\n>>> chain 0x01500000")
        ser.write(b"chain 0x01500000" + ENTER)
        ser.flush()

        # After chain enters the next U-Boot stage, the prompt changes to RTD1619>
        wait_for_text(ser, CHAIN_PROMPT)

        print("\n[STAGE 2] Sending FDT and boot commands one by one")

        for cmd in STAGE2_COMMANDS:
            if cmd.startswith("booti "):
                print(f"\n>>> {cmd}", flush=True)
                ser.write(cmd.encode("utf-8") + ENTER)
                ser.flush()
            else:
                send_command_wait_prompt(ser, cmd, prompt=CHAIN_PROMPT)

        keep_logging(ser)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[EXIT] Stopped by user.")
    except serial.SerialException as e:
        print(f"\n[ERROR] Serial error: {e}")
        sys.exit(1)