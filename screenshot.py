r"""
T-Deck-Ai-Terminal screenshot tool — captures the device screen over USB-CDC.

No WiFi credentials needed — just plug in the T-Deck via USB.

Double-click to run interactively, or use the command line:
  python screenshot.py                  # auto-detect COM port, save screenshot.png
  python screenshot.py COM11            # specify COM port
  python screenshot.py COM11 home.png   # specify port and output filename

Requires: pip install pyserial
"""

import os
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial not installed. Run: python -m pip install pyserial")
    sys.exit(1)

BAUD = 115200
W, H = 320, 240
BASE_DIR = os.path.dirname(os.path.abspath(__file__))


def find_tdeck_port():
    """Scan COM ports for a T-Deck device (ESP32-S3 USB-CDC)."""
    import serial.tools.list_ports
    ports = serial.tools.list_ports.comports()
    for p in ports:
        # ESP32-S3 USB-CDC typically shows as "USB Serial Device" or similar
        vid = getattr(p, 'vid', None)
        pid = getattr(p, 'pid', None)
        if vid == 0x303A and pid == 0x1001:  # ESP32-S3 USB-CDC
            return p.device
        if "USB" in (p.description or "") and "Serial" in (p.description or ""):
            return p.device
    # Fallback: return the last USB serial port found
    for p in reversed(ports):
        if "USB" in str(p.description or "").upper():
            return p.device
    return None


def open_serial_no_reset(port):
    """Open serial port without triggering ESP32 bootloader reset."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.5
    ser.write_timeout = 5
    ser.rtscts = False
    ser.dsrdtr = False
    # Hold DTR/RTS deasserted to avoid triggering ESP32 bootloader
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.setDTR(False)
    ser.setRTS(False)
    return ser


def read_exact(ser, n, timeout=45):
    """Read exactly n bytes, with progress indicator."""
    data = b""
    deadline = time.time() + timeout
    while len(data) < n and time.time() < deadline:
        chunk = ser.read(min(4096, n - len(data)))
        if chunk:
            data += chunk
            pct = len(data) * 100 // n
            print(f"  {pct}%", end="\r")
    return data


def wait_for_marker(ser, markers, timeout=10):
    """Read until one of the marker byte-strings is found."""
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = ser.read(1)
        if not c:
            continue
        buf += c
        for marker in markers:
            if buf.endswith(marker):
                return marker, buf
        if len(buf) > 512:
            buf = buf[-512:]
    return None, buf


def write_png(filename, pixels_rgb332):
    """Convert raw RGB332 (320x240) to 24-bit BMP, then optionally PNG."""
    if not os.path.isabs(filename):
        filename = os.path.join(BASE_DIR, filename)
    folder = os.path.dirname(filename)
    if folder:
        os.makedirs(folder, exist_ok=True)

    # RGB332 → 24-bit BGR (BMP pixel order)
    bmp_pixels = bytearray(W * H * 3)
    for i, c in enumerate(pixels_rgb332):
        r3 = (c >> 5) & 0x07
        g3 = (c >> 2) & 0x07
        b2 = c & 0x03
        r8 = (r3 << 5) | (r3 << 2) | (r3 >> 1)
        g8 = (g3 << 5) | (g3 << 2) | (g3 >> 1)
        b8 = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2
        bmp_pixels[i * 3 + 0] = b8  # B
        bmp_pixels[i * 3 + 1] = g8  # G
        bmp_pixels[i * 3 + 2] = r8  # R

    # Write 24-bit BMP
    filesize = 54 + W * H * 3
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    hdr[2:6] = struct.pack("<I", filesize)
    hdr[10] = 54
    hdr[14] = 40
    hdr[18:22] = struct.pack("<I", W)
    hdr[22:26] = struct.pack("<i", -H)  # top-down
    hdr[26:28] = struct.pack("<H", 1)   # planes
    hdr[28:30] = struct.pack("<H", 24)  # bpp

    name, ext = os.path.splitext(filename)
    ext = ext.lower()

    if ext == ".bmp":
        with open(filename, "wb") as f:
            f.write(hdr)
            f.write(bmp_pixels)
        print(f"Saved {filename}  ({W}x{H} BMP)")
        return

    # Save as BMP first, then convert to PNG with pillow if available
    tmp_bmp = name + ".tmp.bmp"
    with open(tmp_bmp, "wb") as f:
        f.write(hdr)
        f.write(bmp_pixels)

    try:
        from PIL import Image
        img = Image.open(tmp_bmp)
        img.save(filename, "PNG")
        os.remove(tmp_bmp)
        print(f"Saved {filename}  ({W}x{H} PNG)")
    except ImportError:
        os.rename(tmp_bmp, filename if ext == ".bmp" else name + ".bmp")
        print(f"Saved {filename}  ({W}x{H} BMP — pip install pillow for PNG)")


def capture(port, outfile):
    print(f"Opening {port} without reset...")
    ser = open_serial_no_reset(port)

    try:
        time.sleep(0.3)
        ser.reset_input_buffer()

        # Check device is ready
        print("Checking device...")
        ser.write(b"R")
        ser.flush()
        marker, _ = wait_for_marker(ser, [b"READY"], timeout=3)
        if marker:
            print("  Device ready.")
        else:
            print("  No READY reply; continuing anyway...")

        ser.reset_input_buffer()

        # Request screenshot
        print("Capturing screen...")
        ser.write(b"S")
        ser.flush()

        marker, _ = wait_for_marker(ser, [b"RGB332:", b"OOM:"], timeout=30)
        if marker is None:
            raise RuntimeError("No screenshot response from device.")
        if marker == b"OOM:":
            info = ser.readline().decode("ascii", errors="replace").strip()
            raise RuntimeError(f"Device out of RAM: {info}")

        total = W * H
        start = time.time()
        data = read_exact(ser, total)
        elapsed = time.time() - start

        if len(data) < total:
            raise RuntimeError(f"Transfer stalled at {len(data)}/{total} bytes.")
        print(f"  Done in {elapsed:.1f}s          ")
    finally:
        ser.close()

    write_png(outfile, data)


def main():
    args = sys.argv[1:]
    port = None
    outfile = None

    for a in args:
        if a.upper().startswith("COM"):
            port = a.upper()
        else:
            outfile = a

    if port is None:
        print("Looking for T-Deck...")
        port = find_tdeck_port()
        if port is None:
            print("No T-Deck found. Specify COM port:")
            print("  python screenshot.py COM11")
            sys.exit(1)
        print(f"  → {port}")

    if outfile is None:
        outfile = os.path.join(BASE_DIR, "screenshot.png")

    try:
        capture(port, outfile)
    except Exception as exc:
        print(f"Error: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
