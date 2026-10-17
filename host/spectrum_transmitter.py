#!/usr/bin/env python3
"""Send eight system-audio spectrum heights over a USB NCM UDP link.

Linux capture uses the PulseAudio monitor exposed by PulseAudio or pipewire-pulse.
No microphone and no raw audio are sent over the network.
"""

import argparse
import math
from pathlib import Path
import socket
import subprocess
import sys
import time

import numpy as np
from audio_volume import parse_volume, set_default_sink_volume


# Edit these defaults, or override IP/port/source on the command line.
ESP32_IP = "192.168.77.1"
UDP_PORT = 4210
USB_MAC = "02:71:19:08:08:01"  # NCM MAC configured in the ESP firmware
SOURCE = "@DEFAULT_MONITOR@"
SAMPLE_RATE = 48_000
FFT_SIZE = 2048
UPDATE_RATE = 50
ATTACK = 0.025                 # Seconds to rise toward a new bar height
DECAY = 0.10                   # Seconds to fall toward a new bar height
MIN_FREQ = 40.0
MAX_FREQ = 11_000.0
DYNAMIC_RANGE_DB = 30.0       # Smaller range gives the bars more contrast
HEADROOM_DB = 8.0             # Normal peaks leave room for musical accents
AGC_ATTACK = 0.35             # Seconds for the level reference to rise
AGC_DECAY = 2.0               # Seconds for the level reference to fall
BAND_GAIN_DB = (5, 4, 4, 4, 2, 0, -3, -6)  # Balance low/mid against bright treble
TONAL_PEAK_WEIGHT = 0.55      # Preserve notes in wide high-frequency bands
MIN_REFERENCE = 0.003         # Stops silence/noise from being amplified endlessly
SILENCE_RMS = 0.0005          # Captured audio below this level counts as silence


def read_exact(stream, count):
    data = bytearray()
    while len(data) < count:
        chunk = stream.read(count - len(data))
        if not chunk:
            raise EOFError("parec stopped producing audio")
        data.extend(chunk)
    return data


class Spectrum:
    def __init__(self):
        if not (FFT_SIZE > 0 and FFT_SIZE & (FFT_SIZE - 1) == 0):
            raise ValueError("FFT_SIZE must be a power of two")
        if not (0 < MIN_FREQ < MAX_FREQ < SAMPLE_RATE / 2):
            raise ValueError("Frequency limits must lie below Nyquist")
        self.hop = round(SAMPLE_RATE / UPDATE_RATE)
        if not 0 < self.hop <= FFT_SIZE:
            raise ValueError("UPDATE_RATE produces an invalid chunk size")

        self.frame = np.zeros((FFT_SIZE, 2), dtype=np.float32)
        self.window = np.hanning(FFT_SIZE).astype(np.float32)
        frequencies = np.fft.rfftfreq(FFT_SIZE, 1 / SAMPLE_RATE)
        edges = np.geomspace(MIN_FREQ, MAX_FREQ, 9)
        self.band_bins = [
            np.flatnonzero((frequencies >= edges[i]) & (frequencies < edges[i + 1]))
            for i in range(8)
        ]
        if any(len(indices) == 0 for indices in self.band_bins):
            raise ValueError("A frequency band has no FFT bins; increase FFT_SIZE")

        self.band_gain = 10.0 ** (np.asarray(BAND_GAIN_DB, dtype=np.float32) / 20.0)
        self.reference = MIN_REFERENCE
        self.smoothed = np.zeros(8, dtype=np.float32)
        self.dt = self.hop / SAMPLE_RATE

    def process(self, stereo):
        # Analyze each channel before combining, so opposite-phase stereo cannot cancel.
        self.frame[:-self.hop] = self.frame[self.hop:]
        self.frame[-self.hop:] = stereo

        if np.sqrt(np.mean(self.frame * self.frame)) < SILENCE_RMS:
            levels = np.zeros(8, dtype=np.float32)
        else:
            channel_spectra = np.fft.rfft(self.frame * self.window[:, None], axis=0)
            magnitude = np.sqrt(np.mean(np.abs(channel_spectra) ** 2, axis=1))
            magnitude *= 2.0 / self.window.sum()
            levels = np.array([
                max(
                    np.sqrt(np.mean(magnitude[indices] ** 2)),
                    TONAL_PEAK_WEIGHT * np.max(magnitude[indices]),
                )
                for indices in self.band_bins
            ], dtype=np.float32)

        balanced = levels * self.band_gain
        peak = float(balanced.max())
        agc_time = AGC_ATTACK if peak > self.reference else AGC_DECAY
        agc_alpha = 1.0 - math.exp(-self.dt / agc_time)
        self.reference = max(
            MIN_REFERENCE, self.reference + agc_alpha * (peak - self.reference)
        )

        # A slow reference and headroom let beats rise without holding every bar at 8.
        relative_db = 20.0 * np.log10(np.maximum(balanced, 1e-12) / self.reference)
        target = np.clip(
            8.0 * (relative_db + DYNAMIC_RANGE_DB - HEADROOM_DB) / DYNAMIC_RANGE_DB,
            0.0, 8.0,
        )
        rising = target > self.smoothed
        attack_alpha = 1.0 - math.exp(-self.dt / ATTACK)
        decay_alpha = 1.0 - math.exp(-self.dt / DECAY)
        self.smoothed += np.where(rising, attack_alpha, decay_alpha) * (
            target - self.smoothed
        )
        return np.rint(self.smoothed).astype(np.uint8)


def list_sources():
    subprocess.run(["pactl", "list", "short", "sources"], check=True)


def find_usb_interface():
    for _, name in socket.if_nameindex():
        address_file = Path("/sys/class/net") / name / "address"
        try:
            if address_file.read_text().strip().lower() == USB_MAC:
                return name
        except OSError:
            continue
    raise OSError(f"USB NCM interface with MAC {USB_MAC} is not connected")


def run(ip, port, source, interface=None, debug=False, bind_usb=True):
    spectrum = Spectrum()
    if bind_usb:
        interface = interface or find_usb_interface()
    command = [
        "parec", "--record", "--raw", f"--device={source}",
        "--format=float32le", f"--rate={SAMPLE_RATE}", "--channels=2",
        "--latency-msec=25",
    ]
    transport = f"USB interface {interface}" if interface else "default network route"
    print(f"Capturing {source}; sending {UPDATE_RATE} updates/s over {transport} to {ip}:{port}")
    print("Bands (Hz): " + ", ".join(
        f"{MIN_FREQ * (MAX_FREQ / MIN_FREQ) ** (i / 8):.0f}-"
        f"{MIN_FREQ * (MAX_FREQ / MIN_FREQ) ** ((i + 1) / 8):.0f}"
        for i in range(8)
    ))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        if interface:
            udp.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, interface.encode() + b"\0")
        recorder = subprocess.Popen(command, stdout=subprocess.PIPE)
        try:
            bytes_per_chunk = spectrum.hop * 2 * 4  # stereo float32
            next_debug = time.monotonic()
            while True:
                raw = read_exact(recorder.stdout, bytes_per_chunk)
                stereo = np.frombuffer(raw, dtype="<f4").reshape(-1, 2)
                heights = spectrum.process(stereo)
                packet = ",".join(str(int(value)) for value in heights).encode("ascii")
                udp.sendto(packet, (ip, port))
                if debug and time.monotonic() >= next_debug:
                    print(f"heights: {packet.decode('ascii')}")
                    next_debug = time.monotonic() + 1.0
        finally:
            recorder.terminate()
            try:
                recorder.wait(timeout=2)
            except subprocess.TimeoutExpired:
                recorder.kill()
                recorder.wait()
            recorder.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ip", default=ESP32_IP, help="ESP32 IPv4 address")
    parser.add_argument("--port", type=int, default=UDP_PORT, help="ESP32 UDP port")
    parser.add_argument("--source", default=SOURCE, help="PulseAudio monitor source")
    parser.add_argument("--interface", help="USB network interface; auto-detected by MAC by default")
    parser.add_argument("--volume", type=parse_volume,
                        help="set the default speaker sink first, e.g. 120 or 120%%")
    parser.add_argument("--list-sources", action="store_true", help="Show available sources and exit")
    parser.add_argument("--debug", action="store_true", help="Print one packet per second")
    args = parser.parse_args()
    try:
        if args.list_sources:
            list_sources()
        else:
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
