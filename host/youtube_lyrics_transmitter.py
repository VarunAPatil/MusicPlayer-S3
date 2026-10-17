#!/usr/bin/env python3
"""Show timed YouTube lyrics on the OLED and send spectrum bars to the matrix."""

import argparse
import asyncio
from bisect import bisect_right
import difflib
import json
import re
import socket
import subprocess
import sys
import time
import unicodedata
from pathlib import Path
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from dbus_next import BusType, Message, MessageType
from dbus_next.aio import MessageBus

from audio_volume import parse_volume, set_default_sink_volume
from spectrum_transmitter import ESP32_IP, UDP_PORT, find_usb_interface, list_sources


MPRIS_PREFIX = "org.mpris.MediaPlayer2."
DBUS_DESTINATION = "org.freedesktop.DBus"
DBUS_PATH = "/org/freedesktop/DBus"
DBUS_INTERFACE = "org.freedesktop.DBus"
PROPERTIES_INTERFACE = "org.freedesktop.DBus.Properties"
MPRIS_PATH = "/org/mpris/MediaPlayer2"
MPRIS_PLAYER = "org.mpris.MediaPlayer2.Player"
LRCLIB_SEARCH = "https://lrclib.net/api/search"
USER_AGENT = "MusicPlayer-S3-OLED/1.0 (personal lyrics display)"
POLL_INTERVAL = 0.10
HEARTBEAT_INTERVAL = 1.0
FETCH_TIMEOUT_SECONDS = 5
MAX_TEXT_BYTES = 63
TIME_TAG = re.compile(r"\[(\d{1,2}):(\d{2})(?:\.(\d{1,3}))?\]")
DECORATION = re.compile(
    r"\s*[\[(](?:official\s*)?(?:music\s*)?(?:video|lyrics?|audio|"
    r"visuali[sz]er|lyric\s*video|hd|4k)(?:\s*video)?[\])]"
    r"|\s*\|\s*(?:lyrics?|official\s*video).*",
    re.IGNORECASE,
)


def ascii_text(value):
    normalized = unicodedata.normalize("NFKD", value)
    return "".join(char for char in normalized if not unicodedata.combining(char)) \
        .encode("ascii", "replace").decode("ascii").strip()


def clean_title(raw_title):
    title = DECORATION.sub("", raw_title)
    title = re.sub(r"\s*[-|]\s*(?:YouTube|Chromium|Firefox).*$", "", title,
                   flags=re.IGNORECASE)
    return ascii_text(title).strip(" -|[]()")


def parse_lrc(content):
    rows = []
    offset_ms = 0
    for raw_line in content.splitlines():
        if raw_line.lower().startswith("[offset:"):
            try:
                offset_ms = int(raw_line[8:].split("]", 1)[0])
            except ValueError:
                pass
            continue
        tags = list(TIME_TAG.finditer(raw_line))
        lyric = TIME_TAG.sub("", raw_line).strip()
        for tag in tags:
            minutes, seconds, fraction = tag.groups()
            fractional_seconds = float("0." + (fraction or "0"))
            timestamp = int(minutes) * 60 + int(seconds) + fractional_seconds
            rows.append((max(0.0, timestamp + offset_ms / 1000.0), ascii_text(lyric)))
    return sorted(rows)


def normalized(value):
    return " ".join(re.findall(r"[a-z0-9]+", ascii_text(value).casefold()))


def unvariant(value):
    return value.value if hasattr(value, "value") else value


def fetch_lyrics(title, artist, duration):
    query = " ".join(part for part in (title, artist) if part).strip()
    url = LRCLIB_SEARCH + "?" + urlencode({"q": query})
    request = Request(url, headers={"User-Agent": USER_AGENT})
    with urlopen(request, timeout=FETCH_TIMEOUT_SECONDS) as response:
        candidates = json.load(response)

    wanted_title = normalized(title)
    wanted_artist = normalized(artist)
    scored = []
    for item in candidates:
        synced = item.get("syncedLyrics")
        if not synced:
            continue
        candidate_title = normalized(item.get("trackName", ""))
        candidate_artist = normalized(item.get("artistName", ""))
        title_score = difflib.SequenceMatcher(None, wanted_title, candidate_title).ratio()
        artist_score = (difflib.SequenceMatcher(None, wanted_artist, candidate_artist).ratio()
                        if wanted_artist else 0.5)
        duration_score = 0.0
        candidate_duration = item.get("duration")
        if duration and candidate_duration:
            duration_score = max(0.0, 1.0 - abs(duration - candidate_duration) / 10.0)
        score = title_score * 0.70 + artist_score * 0.20 + duration_score * 0.10
        scored.append((score, item))

    if not scored:
        return []
    scored.sort(key=lambda pair: pair[0], reverse=True)
    score, match = scored[0]
    if score < 0.55:
        return []
    return parse_lrc(match["syncedLyrics"])


async def call_dbus(bus, destination, path, interface, member,
                    signature="", body=None):
    message = Message(
        destination=destination,
        path=path,
        interface=interface,
        member=member,
        signature=signature,
        body=body or [],
    )
    reply = await asyncio.wait_for(bus.call(message), timeout=2.0)
    if reply.message_type == MessageType.ERROR:
        raise RuntimeError(reply.body[0] if reply.body else "D-Bus method failed")
    return reply


async def player_snapshot(bus, preferred_player=None):
    reply = await call_dbus(bus, DBUS_DESTINATION, DBUS_PATH, DBUS_INTERFACE, "ListNames")
    names = [name for name in reply.body[0]
             if name.startswith(MPRIS_PREFIX)
             and any(browser in name.casefold() for browser in ("chromium", "chrome", "firefox"))]
    if preferred_player:
        names.sort(key=lambda name: preferred_player.casefold() not in name.casefold())

    for name in names:
        try:
            reply = await call_dbus(
                bus, name, MPRIS_PATH, PROPERTIES_INTERFACE, "GetAll", "s", [MPRIS_PLAYER]
            )
            properties = {key: value.value for key, value in reply.body[0].items()}
            if properties.get("PlaybackStatus") != "Playing":
                continue
            metadata = properties.get("Metadata", {})
            title = unvariant(metadata.get("xesam:title"))
            if not title:
                continue
            artist_value = unvariant(metadata.get("xesam:artist", []))
            artist = ", ".join(artist_value) if isinstance(artist_value, list) else str(artist_value)
            position_reply = await call_dbus(
                bus, name, MPRIS_PATH, PROPERTIES_INTERFACE, "Get", "ss",
                [MPRIS_PLAYER, "Position"],
            )
            position = float(position_reply.body[0].value) / 1_000_000.0
            duration = float(unvariant(metadata.get("mpris:length", 0))) / 1_000_000.0
            return name, clean_title(str(title)), ascii_text(artist), position, duration
        except (asyncio.TimeoutError, KeyError, RuntimeError, TypeError, ValueError):
            continue
    return None


def make_packet(title, line):
    title_bytes = ascii_text(title).encode("ascii", "replace")[:MAX_TEXT_BYTES]
    line_bytes = ascii_text(line).encode("ascii", "replace")[:MAX_TEXT_BYTES]
    return b"L" + bytes((len(title_bytes), len(line_bytes))) + title_bytes + line_bytes


def line_at(lines, position):
    if not lines:
        return ""
    starts = [row[0] for row in lines]
    index = bisect_right(starts, position) - 1
    return lines[index][1] if index >= 0 else ""


async def run(args):
    interface = args.interface or find_usb_interface()
    bus = await MessageBus(bus_type=BusType.SESSION).connect()
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, interface.encode() + b"\0")
    destination = (args.ip, args.port)

    spectrum_process = None
    if not args.no_spectrum:
        script = Path(__file__).with_name("spectrum_transmitter.py")
        command = [sys.executable, str(script), "--ip", args.ip, "--port", str(args.port),
                   "--source", args.source, "--interface", interface]
        spectrum_process = subprocess.Popen(command)

    print(f"Watching YouTube MPRIS; sending lyrics and bars over {interface} to {args.ip}:{args.port}")
    cache = {}
    current_key = None
    lines = []
    display_title = "YOUTUBE"
    display_line = "Waiting for playback"
    last_payload = None
    last_sent = 0.0
    next_poll = time.monotonic()

    try:
        while True:
            snapshot = await player_snapshot(bus, args.player)
            if snapshot is None:
                display_title, display_line = "YOUTUBE", "Waiting for playback"
            else:
                _, title, artist, position, duration = snapshot
                track_key = (title.casefold(), artist.casefold())
                if track_key != current_key:
                    current_key = track_key
                    display_title = title or "YOUTUBE"
                    display_line = "Searching timed lyrics"
                    print(f"Searching timed lyrics: {artist} - {title}")
                    if track_key not in cache:
                        try:
                            cache[track_key] = await asyncio.to_thread(
                                fetch_lyrics, title, artist, duration
                            )
                        except (OSError, ValueError, json.JSONDecodeError) as error:
                            cache[track_key] = []
                            print(f"Lyric lookup failed: {error}", file=sys.stderr)
                    lines = cache[track_key]
                    display_line = "No synced lyrics found" if not lines else ""
                    print(f"Found {len(lines)} timed lines" if lines else "No synced lyrics found")
                if lines:
                    display_line = line_at(lines, position + args.offset)

            now = time.monotonic()
            payload = make_packet(display_title or "YOUTUBE", display_line)
            if payload != last_payload or now - last_sent >= HEARTBEAT_INTERVAL:
                udp.sendto(payload, destination)
                last_payload = payload
                last_sent = now

            next_poll += POLL_INTERVAL
            await asyncio.sleep(max(0.0, next_poll - time.monotonic()))
            if time.monotonic() - next_poll > POLL_INTERVAL:
                next_poll = time.monotonic()
    finally:
        udp.close()
        bus.disconnect()
        if spectrum_process:
            spectrum_process.terminate()
            try:
                spectrum_process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                spectrum_process.kill()
                spectrum_process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ip", default=ESP32_IP)
    parser.add_argument("--port", type=int, default=UDP_PORT)
    parser.add_argument("--interface", help="USB NCM interface; auto-detected by MAC by default")
    parser.add_argument("--player", help="prefer an MPRIS player whose name contains this string")
    parser.add_argument("--source", default="@DEFAULT_MONITOR@",
                        help="PulseAudio/PipeWire monitor used for spectrum bars")
    parser.add_argument("--offset", type=float, default=0.0,
                        help="lyric timing adjustment in seconds; positive moves lyrics later")
    parser.add_argument("--volume", type=parse_volume,
                        help="set default speaker sink volume, e.g. 120 or 120%%")
    parser.add_argument("--no-spectrum", action="store_true",
                        help="send lyrics only; do not launch the audio bar sender")
    parser.add_argument("--list-sources", action="store_true")
    args = parser.parse_args()
    try:
        if args.list_sources:
            list_sources()
            return 0
        if not 1 <= args.port <= 65535:
            parser.error("--port must be from 1 to 65535")
        set_default_sink_volume(args.volume)
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\nStopped.")
    except (FileNotFoundError, OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
