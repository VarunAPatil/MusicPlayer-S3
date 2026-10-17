#!/usr/bin/env python3
"""Capture the visible YouTube video and send monochrome OLED frames over USB UDP."""

import argparse
import socket
import struct
import sys
import time

import numpy as np
from mss import MSS
from PIL import Image, ImageEnhance, ImageOps

from audio_volume import parse_volume, set_default_sink_volume
from spectrum_transmitter import ESP32_IP, UDP_PORT, find_usb_interface


OLED_WIDTH = 128
OLED_HEIGHT = 64
VIDEO_FPS = 20
CONTRAST = 1.25
BRIGHTNESS = 1.10
DITHER = True
FRAME_MAGIC = b"V"
FRAME_HEADER = struct.Struct("!cI")


def parse_region(text):
    try:
        left, top, width, height = (int(value.strip()) for value in text.split(","))
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError("region must be LEFT,TOP,WIDTH,HEIGHT") from error
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("region width and height must be positive")
    return {"left": left, "top": top, "width": width, "height": height}


def frame_to_ssd1306(rgb_bytes, size, contrast=CONTRAST, brightness=BRIGHTNESS,
                     dither=DITHER):
    image = Image.frombytes("RGB", size, rgb_bytes)
    fitted = ImageOps.contain(image, (OLED_WIDTH, OLED_HEIGHT),
                              method=Image.Resampling.LANCZOS)
    canvas = Image.new("L", (OLED_WIDTH, OLED_HEIGHT), 0)
    canvas.paste(fitted.convert("L"),
                 ((OLED_WIDTH - fitted.width) // 2,
                  (OLED_HEIGHT - fitted.height) // 2))
    canvas = ImageEnhance.Contrast(canvas).enhance(contrast)
    canvas = ImageEnhance.Brightness(canvas).enhance(brightness)
    if dither:
        monochrome = canvas.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    else:
        monochrome = canvas.point(lambda value: 255 if value >= 128 else 0, mode="1")

    bits = np.asarray(monochrome, dtype=np.uint8)
    pages = bits.reshape(8, 8, OLED_WIDTH).transpose(0, 2, 1)
    return np.packbits(pages, axis=2, bitorder="little").tobytes()


def run(args):
    interface = args.interface or find_usb_interface()
    if args.region is not None:
        capture_region = args.region
    else:
        with MSS() as screen:
            if not 1 <= args.monitor < len(screen.monitors):
                raise ValueError(f"monitor {args.monitor} not found; detected {len(screen.monitors) - 1}")
            capture_region = screen.monitors[args.monitor]

    print(f"Capturing screen region {capture_region}; OLED {OLED_WIDTH}x{OLED_HEIGHT} at {args.fps} fps")
    print(f"Sending frames over {interface} to {args.ip}:{args.port}")
    frame_period = 1.0 / args.fps
    sequence = 0
    next_frame = time.monotonic()

    with MSS() as screen, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE,
                       interface.encode() + b"\0")
        while True:
            screenshot = screen.grab(capture_region)
            frame = frame_to_ssd1306(
                screenshot.rgb, screenshot.size, args.contrast,
                args.brightness, not args.no_dither,
            )
            packet = FRAME_HEADER.pack(FRAME_MAGIC, sequence) + frame
            udp.sendto(packet, (args.ip, args.port))
            sequence = (sequence + 1) & 0xFFFFFFFF

            next_frame += frame_period
            delay = next_frame - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                next_frame = time.monotonic()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ip", default=ESP32_IP)
    parser.add_argument("--port", type=int, default=UDP_PORT)
    parser.add_argument("--monitor", type=int, default=1,
                        help="mss monitor index (1 is the first physical monitor)")
    parser.add_argument("--region", type=parse_region,
                        help="capture only LEFT,TOP,WIDTH,HEIGHT, for example 300,150,900,500")
    parser.add_argument("--interface", help="USB network interface; auto-detected by MAC")
    parser.add_argument("--fps", type=float, default=VIDEO_FPS)
    parser.add_argument("--contrast", type=float, default=CONTRAST)
    parser.add_argument("--brightness", type=float, default=BRIGHTNESS)
    parser.add_argument("--no-dither", action="store_true",
                        help="use a hard black/white threshold instead of grayscale dithering")
    parser.add_argument("--volume", type=parse_volume,
                        help="set the default speaker sink first, e.g. 120 or 120%%")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be from 1 to 65535")
    if not 1 <= args.fps <= 30:
        parser.error("--fps must be from 1 to 30")
    try:
        set_default_sink_volume(args.volume)
        run(args)
    except KeyboardInterrupt:
        print("\nStopped.")
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
