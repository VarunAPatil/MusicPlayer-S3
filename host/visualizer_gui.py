#!/usr/bin/env python3
"""Tkinter front end for the existing MusicPlayer-S3 host transmitters."""

import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import messagebox, ttk


ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "host"
DEFAULT_IP = "192.168.77.1"
DEFAULT_PORT = "4210"
DEFAULT_SOURCE = "@DEFAULT_MONITOR@"

MODES = {
    "Wired video + bars (USB NCM)": "video",
    "Wired lyrics + bars (USB NCM)": "lyrics",
    "Wired bars only (USB NCM)": "wired-bars",
    "Wired bars only (ESP32 DSP over USB NCM)": "esp-dsp",
    "Wireless bars only (Wi-Fi UDP)": "wireless-bars",
}


class VisualizerGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("MusicPlayer-S3 Visualizer")
        self.root.geometry("760x650")
        self.root.minsize(680, 560)
        self.processes = []
        self.log_queue = queue.Queue()

        self.mode = tk.StringVar(value=next(iter(MODES)))
        self.ip = tk.StringVar(value=DEFAULT_IP)
        self.port = tk.StringVar(value=DEFAULT_PORT)
        self.source = tk.StringVar(value=DEFAULT_SOURCE)
        self.volume = tk.StringVar()
        self.monitor = tk.StringVar(value="1")
        self.fps = tk.StringVar(value="20")
        self.region = tk.StringVar()
        self.player = tk.StringVar()
        self.offset = tk.StringVar(value="0")
        self.debug = tk.BooleanVar(value=False)
        self.status = tk.StringVar(value="Idle")

        self._build()
        self.refresh_sources(silent=True)
        self.root.after(100, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _build(self):
        outer = ttk.Frame(self.root, padding=14)
        outer.pack(fill="both", expand=True)
        ttk.Label(outer, text="MusicPlayer-S3 Visualizer", font=("TkDefaultFont", 16, "bold")).pack(anchor="w")
        ttk.Label(outer, text="Choose a mode, set the audio monitor and target, then start the existing host scripts.").pack(anchor="w", pady=(2, 12))

        form = ttk.Frame(outer)
        form.pack(fill="x")
        form.columnconfigure(1, weight=1)
        row = 0
        self._label(form, "Mode", row)
        mode_box = ttk.Combobox(form, textvariable=self.mode, values=list(MODES), state="readonly")
        mode_box.grid(row=row, column=1, columnspan=2, sticky="ew", pady=3)
        mode_box.bind("<<ComboboxSelected>>", lambda _event: self._update_mode_help())
        row += 1

        self._label(form, "ESP32 IP", row)
        ttk.Entry(form, textvariable=self.ip).grid(row=row, column=1, sticky="ew", pady=3)
        self._label(form, "UDP port", row, column=2)
        ttk.Entry(form, textvariable=self.port, width=8).grid(row=row, column=3, sticky="w", pady=3)
        row += 1

        self._label(form, "Audio monitor source", row)
        self.source_box = ttk.Combobox(form, textvariable=self.source, values=[DEFAULT_SOURCE])
        self.source_box.grid(row=row, column=1, columnspan=2, sticky="ew", pady=3)
        ttk.Button(form, text="Refresh", command=self.refresh_sources).grid(row=row, column=3, sticky="ew", padx=(6, 0))
        row += 1

        self._label(form, "Set speaker volume", row)
        ttk.Entry(form, textvariable=self.volume, width=12).grid(row=row, column=1, sticky="w", pady=3)
        ttk.Label(form, text="Optional, 0–200%; blank keeps current volume").grid(row=row, column=2, columnspan=2, sticky="w", padx=(8, 0))
        row += 1

        self._label(form, "Video monitor", row)
        ttk.Entry(form, textvariable=self.monitor, width=8).grid(row=row, column=1, sticky="w", pady=3)
        self._label(form, "Video FPS", row, column=2)
        ttk.Entry(form, textvariable=self.fps, width=8).grid(row=row, column=3, sticky="w", pady=3)
        row += 1

        self._label(form, "Video crop", row)
        ttk.Entry(form, textvariable=self.region).grid(row=row, column=1, columnspan=3, sticky="ew", pady=3)
        row += 1
        ttk.Label(form, text="Optional LEFT,TOP,WIDTH,HEIGHT in screen pixels; blank captures the selected monitor.").grid(row=row, column=1, columnspan=3, sticky="w", pady=(0, 4))
        row += 1

        self._label(form, "MPRIS player filter", row)
        ttk.Entry(form, textvariable=self.player).grid(row=row, column=1, sticky="ew", pady=3)
        self._label(form, "Lyrics offset (s)", row, column=2)
        ttk.Entry(form, textvariable=self.offset, width=8).grid(row=row, column=3, sticky="w", pady=3)
        row += 1

        ttk.Checkbutton(form, text="Print bar values to the log (debug)", variable=self.debug).grid(row=row, column=1, columnspan=3, sticky="w", pady=(5, 8))

        self.mode_help = ttk.Label(outer, wraplength=710, foreground="#334155")
        self.mode_help.pack(fill="x", pady=(8, 8))
        self._update_mode_help()

        controls = ttk.Frame(outer)
        controls.pack(fill="x", pady=(0, 8))
        self.start_button = ttk.Button(controls, text="Start", command=self.start)
        self.start_button.pack(side="left")
        self.stop_button = ttk.Button(controls, text="Stop", command=self.stop, state="disabled")
        self.stop_button.pack(side="left", padx=6)
        ttk.Label(controls, textvariable=self.status).pack(side="right")

        self.log = tk.Text(outer, height=16, wrap="word", state="disabled")
        self.log.pack(fill="both", expand=True)
        scrollbar = ttk.Scrollbar(self.log, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")

    @staticmethod
    def _label(parent, value, row, column=0):
        ttk.Label(parent, text=value).grid(row=row, column=column, sticky="w", padx=(0, 8), pady=3)

    def _update_mode_help(self):
        kind = MODES[self.mode.get()]
        help_text = {
            "video": "USB NCM must be connected and the OLED video visible on the selected screen. The GUI starts the existing audio-spectrum and video transmitters together.",
            "lyrics": "USB NCM plus a browser exposing YouTube playback through MPRIS. Synced lyrics are looked up online; the spectrum sender is started by the existing lyrics script.",
            "wired-bars": "USB NCM sends only the eight audio bar heights to the MAX7219 matrix.",
            "esp-dsp": "Flash firmware/usb-ncm-dsp first. USB NCM sends small mono PCM chunks; the ESP32 runs the FFT, eight-band analysis, normalization, and smoothing locally.",
            "wireless-bars": "Wi-Fi mode sends only the eight audio bar heights. Set ESP32 IP to the address printed by its firmware; this firmware does not drive the OLED.",
        }[kind]
        self.mode_help.configure(text=help_text)

    def _append_log(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def refresh_sources(self, silent=False):
        try:
            result = subprocess.run(["pactl", "list", "short", "sources"], check=True,
                                    capture_output=True, text=True, timeout=4)
            sources = [line.split()[1] for line in result.stdout.splitlines()
                       if len(line.split()) >= 2 and line.split()[1].endswith(".monitor")]
            values = [DEFAULT_SOURCE] + sources
            self.source_box.configure(values=list(dict.fromkeys(values)))
            if not silent:
                self._append_log(f"Found {len(sources)} monitor source(s). Choose the one for the speaker playing the song.")
        except (OSError, subprocess.SubprocessError) as error:
            if not silent:
                self._append_log(f"Could not list PulseAudio/PipeWire sources: {error}. You can type a source name manually.")

    def _validate(self):
        try:
            port = int(self.port.get())
            if not 1 <= port <= 65535:
                raise ValueError("UDP port must be 1–65535")
            if not self.ip.get().strip():
                raise ValueError("ESP32 IP address is required")
            if self.volume.get().strip():
                volume = float(self.volume.get().strip().rstrip("%"))
                if not 0 <= volume <= 200:
                    raise ValueError("Volume must be between 0% and 200%")
            kind = MODES[self.mode.get()]
            if kind == "video":
                monitor = int(self.monitor.get())
                fps = float(self.fps.get())
                if monitor < 1 or not 1 <= fps <= 30:
                    raise ValueError("Video monitor must be >= 1 and FPS must be 1–30")
                if self.region.get().strip():
                    parts = [int(value.strip()) for value in self.region.get().split(",")]
                    if len(parts) != 4 or parts[2] <= 0 or parts[3] <= 0:
                        raise ValueError("Video crop must be LEFT,TOP,WIDTH,HEIGHT with positive size")
            if kind == "lyrics":
                float(self.offset.get())
            return kind, port
        except ValueError as error:
            messagebox.showerror("Check settings", str(error))
            return None

    def _common(self, script, ip, port, source=True, volume=True):
        command = [sys.executable, "-u", str(HOST / script), "--ip", ip, "--port", str(port)]
        if source:
            command += ["--source", self.source.get().strip() or DEFAULT_SOURCE]
        if volume and self.volume.get().strip():
            command += ["--volume", self.volume.get().strip()]
        return command

    def _commands(self, kind, port):
        ip = self.ip.get().strip()
        if kind == "wireless-bars":
            command = self._common("wireless_spectrum_transmitter.py", ip, port)
            if self.debug.get():
                command.append("--debug")
            return [command]
        if kind == "wired-bars":
            command = self._common("spectrum_transmitter.py", ip, port)
            if self.debug.get():
                command.append("--debug")
            return [command]
        if kind == "esp-dsp":
            command = self._common("audio_pcm_streamer.py", ip, port)
            if self.debug.get():
                command.append("--debug")
            return [command]
        if kind == "lyrics":
            command = self._common("youtube_lyrics_transmitter.py", ip, port)
            if self.player.get().strip():
                command += ["--player", self.player.get().strip()]
            if float(self.offset.get()) != 0:
                command += ["--offset", self.offset.get().strip()]
            return [command]

        audio = self._common("spectrum_transmitter.py", ip, port)
        if self.debug.get():
            audio.append("--debug")
        video = self._common("video_transmitter.py", ip, port, source=False, volume=False)
        video += ["--monitor", self.monitor.get().strip(), "--fps", self.fps.get().strip()]
        if self.region.get().strip():
            video += ["--region", self.region.get().strip()]
        return [audio, video]

    def start(self):
        if self.processes:
            return
        validated = self._validate()
        if not validated:
            return
        kind, port = validated
        commands = self._commands(kind, port)
        started = []
        try:
            for command in commands:
                self._append_log("$ " + " ".join(command))
                process = subprocess.Popen(
                    command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1, start_new_session=True,
                )
                started.append(process)
                threading.Thread(target=self._read_output, args=(process,), daemon=True).start()
            self.processes = started
            self.status.set("Running")
            self.start_button.configure(state="disabled")
            self.stop_button.configure(state="normal")
        except OSError as error:
            self._append_log(f"Could not start sender: {error}")
            for process in started:
                self._terminate(process)

    def _read_output(self, process):
        if process.stdout is None:
            return
        for line in process.stdout:
            self.log_queue.put(line.rstrip())

    @staticmethod
    def _terminate(process):
        if process.poll() is not None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=3)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()

    def stop(self):
        for process in self.processes:
            self._terminate(process)
        if self.processes:
            self._append_log("Stopped sender process(es).")
        self.processes.clear()
        self.status.set("Idle")
        self.start_button.configure(state="normal")
        self.stop_button.configure(state="disabled")

    def _poll(self):
        while True:
            try:
                self._append_log(self.log_queue.get_nowait())
            except queue.Empty:
                break
        if self.processes:
            ended = [process for process in self.processes if process.poll() is not None]
            if ended:
                codes = ", ".join(str(process.returncode) for process in ended)
                self._append_log(f"Sender process exited (code {codes}).")
                self.stop()
        self.root.after(100, self._poll)

    def close(self):
        self.stop()
        self.root.destroy()


def main():
    root = tk.Tk()
    VisualizerGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
