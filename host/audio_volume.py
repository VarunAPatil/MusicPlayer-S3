"""Optional PulseAudio/PipeWire sink-volume control shared by host tools."""

import subprocess


def parse_volume(value):
    text = str(value).strip()
    if text.endswith("%"):
        text = text[:-1].strip()
    try:
        percent = float(text)
    except ValueError as error:
        raise ValueError("volume must be a percentage such as 120 or 120%") from error
    if not 0 <= percent <= 200:
        raise ValueError("volume must be between 0% and 200%")
    return percent


def set_default_sink_volume(percent):
    if percent is None:
        return
    value = parse_volume(percent)
    if value.is_integer():
        value = int(value)
    subprocess.run(
        ["pactl", "set-sink-volume", "@DEFAULT_SINK@", f"{value}%"],
        check=True,
    )
