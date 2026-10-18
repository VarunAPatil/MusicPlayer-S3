#!/usr/bin/env python3
"""Send small mono PCM audio chunks to the ESP32 over the USB NCM link.

This program only captures and forwards system-output samples. FFT, band energy,
normalization, smoothing, and MAX7219 updates are performed by ESP32 firmware.
"""

import argparse
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

from audio_volume import parse_volume, set_default_sink_volume


ESP32_IP = "192.168.77.1"
UDP_PORT = 4210
USB_MAC = "02:71:19:08:08:01"
SOURCE = "@DEFAULT_MONITOR@"
SAMPLE_RATE = 16_000
HOP_SAMPLES = 256
PACKET_HEADER = struct.Struct("!cI")  # magic byte + network-order sequence number
PCM_BYTES = HOP_SAMPLES * 2            # signed 16-bit mono PCM, little-endian


def read_exact(stream, count):
    data = bytearray()
    while len(data) < count:
        chunk = stream.read(count - len(data))
        if not chunk:
            raise EOFError("parec stopped producing audio")
        data.extend(chunk)
    return bytes(data)


def find_usb_interface():
    for _, name in socket.if_nameindex():
        address_file = Path("/sys/class/net") / name / "address"
        try:
            if address_file.read_text().strip().lower() == USB_MAC:
                return name
        except OSError:
            continue
    raise OSError(f"USB NCM interface with MAC {USB_MAC} is not connected")


def list_sources():
    subprocess.run(["pactl", "list", "short", "sources"], check=True)


def run(ip, port, source, interface=None, debug=False):
    interface = interface or find_usb_interface()
    capture_command = [
        "parec", "--record", "--raw", f"--device={source}",
        "--format=s16le", f"--rate={SAMPLE_RATE}", "--channels=1",
        "--latency-msec=25",
    ]
    print(f"Capturing {source} at {SAMPLE_RATE} Hz mono PCM")
    print(f"Sending {HOP_SAMPLES}-sample packets ({PCM_BYTES + PACKET_HEADER.size} bytes) "
          f"at {SAMPLE_RATE / HOP_SAMPLES:.1f} packets/s over {interface} to {ip}:{port}")

    recorder = subprocess.Popen(capture_command, stdout=subprocess.PIPE)
    sequence = 0
    packets_since_report = 0
    report_at = time.monotonic() + 1.0
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE,
                           interface.encode() + b"\0")
            while True:
                pcm = read_exact(recorder.stdout, PCM_BYTES)
                packet = PACKET_HEADER.pack(b"A", sequence) + pcm
                udp.sendto(packet, (ip, port))
                sequence = (sequence + 1) & 0xFFFFFFFF
                packets_since_report += 1
                now = time.monotonic()
                if debug and now >= report_at:
                    print(f"sent {packets_since_report} PCM packets in the last second")
                    packets_since_report = 0
                    report_at = now + 1.0
    finally:
        recorder.terminate()
        try:
            recorder.wait(timeout=2)
        except subprocess.TimeoutExpired:
            recorder.kill()
            recorder.wait()
        if recorder.stdout:
            recorder.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ip", default=ESP32_IP, help="ESP32 USB NCM IPv4 address")
    parser.add_argument("--port", type=int, default=UDP_PORT, help="ESP32 UDP port")
    parser.add_argument("--source", default=SOURCE, help="PulseAudio/PipeWire output monitor")
    parser.add_argument("--interface", help="USB NCM interface; auto-detected by MAC by default")
    parser.add_argument("--volume", type=parse_volume,
                        help="optionally set default speaker volume, e.g. 120 or 120%%")
    parser.add_argument("--list-sources", action="store_true",
                        help="list PulseAudio/PipeWire capture sources and exit")
    parser.add_argument("--debug", action="store_true", help="report packet rate once per second")
    args = parser.parse_args()

    try:
        if args.list_sources:
            list_sources()
            return 0
        if not 1 <= args.port <= 65535:
            parser.error("--port must be from 1 to 65535")
        set_default_sink_volume(args.volume)
        run(args.ip, args.port, args.source, args.interface, args.debug)
    except KeyboardInterrupt:
        print("\nStopped.")
    except (FileNotFoundError, subprocess.CalledProcessError, EOFError, OSError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
