#!/usr/bin/env python3
"""Send eight system-audio spectrum values to a Wi-Fi UDP receiver."""

import argparse
import sys

from audio_volume import parse_volume, set_default_sink_volume
from spectrum_transmitter import UDP_PORT, SOURCE, list_sources, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ip", required=True,
                        help="ESP32's Wi-Fi IPv4 address, e.g. 192.168.1.80")
    parser.add_argument("--port", type=int, default=UDP_PORT)
    parser.add_argument("--source", default=SOURCE,
                        help="PulseAudio/PipeWire monitor source")
    parser.add_argument("--volume", type=parse_volume,
                        help="set the default speaker sink first, e.g. 120 or 120%%")
    parser.add_argument("--debug", action="store_true",
                        help="print the eight transmitted values once per second")
    parser.add_argument("--list-sources", action="store_true")
    args = parser.parse_args()

    try:
        if args.list_sources:
            list_sources()
            return 0
        if not 1 <= args.port <= 65535:
            parser.error("--port must be from 1 to 65535")
        set_default_sink_volume(args.volume)
        run(args.ip, args.port, args.source, debug=args.debug, bind_usb=False)
    except KeyboardInterrupt:
        print("\nStopped.")
    except (FileNotFoundError, OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
