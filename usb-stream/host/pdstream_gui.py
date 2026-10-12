#!/usr/bin/env python3
"""PD Audio Monitor Host: pdstream.py in a window, with status, the log, and setup steps.

Built as "PD Audio Monitor Host.app" (`make app`), macOS asks microphone access for the app itself
rather than for a terminal.
"""
import queue
import subprocess
import sys
import threading
import tkinter as tk
import webbrowser
from tkinter import font, scrolledtext, ttk

import pdstream

MICROPHONE_SETTINGS = "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"
SOUND_SETTINGS = "x-apple.systempreferences:com.apple.preference.sound"
BLACKHOLE_URL = "https://existential.audio/blackhole/"
LOG_LINES = 2000

WINDOWS = sys.platform == "win32"
VBCABLE_URL = "https://vb-audio.com/Cable/"

SETUP_WINDOWS = [
    ("Install VB-Cable", "A virtual audio device: what plays into CABLE Input, the host app captures from "
     "CABLE Output. Download it, run the installer as administrator and restart Windows.",
     [("VB-Cable website", lambda: webbrowser.open(VBCABLE_URL))]),
    ("Send audio to CABLE Input", "Pick CABLE Input as Windows' sound output, or as your player's or DAW's "
     "output device. To hear it on the PC too, tick \"Listen to this device\" on CABLE Output's properties "
     "(Recording tab); the Playdate plays a few hundred ms later.",
     [("Sound settings", lambda: subprocess.run(["cmd", "/c", "start", "ms-settings:sound"]))]),
    ("Pick CABLE Output as the Input", "Above, under Input: the host app captures from the device that "
     "receives the audio, CABLE Output (or VoiceMeeter Output). It is picked by default when present; press ↻ "
     "after installing VB-Cable. Never pick your speakers or a microphone.", []),
    ("Start PD Audio Monitor Receiver on the Playdate", "Install PDAudioMonitorReceiver.pdx, start it, "
     "and keep the Playdate connected over USB and unlocked.", []),
]

TROUBLESHOOTING_WINDOWS = [
    ("Wrong input, or \"Error starting stream\"", "Pick CABLE Output or VoiceMeeter Output under Input (↻ refreshes the list). "
     "Inputs that aren't receiving audio, such as a microphone or speakers, stay silent or fail to start.", []),
    ("Peak stays at -99 dBFS", "Nothing reaches CABLE Input: check Windows' (or the player's) output device."),
    ("Waiting for a Playdate", "Plug it in and unlock it. Quit the Playdate Simulator: it also holds the "
     "Playdate's serial port."),
    ("Falling behind, or dropouts", "The USB link is slower than the format needs: pick a lighter one."),
]

SETUP = SETUP_WINDOWS if WINDOWS else [
    ("Install BlackHole 2ch", "A virtual audio device: what plays into it, the host app captures. "
     "Download it, or run `brew install blackhole-2ch`.", [("BlackHole website", lambda: webbrowser.open(BLACKHOLE_URL))]),
    ("Set BlackHole 2ch to 44.1 kHz", "In Audio MIDI Setup. To hear the Mac too, create a Multi-Output Device "
     "there with BlackHole and your headphones; the Playdate plays a few hundred ms later.",
     [("Audio MIDI Setup", lambda: subprocess.run(["open", "-a", "Audio MIDI Setup"]))]),
    ("Send audio to BlackHole", "Pick BlackHole (or the Multi-Output Device) as the Mac's sound output, "
     "or as your DAW's output device.", [("Sound settings", lambda: subprocess.run(["open", SOUND_SETTINGS]))]),
    ("Pick BlackHole 2ch as the Input", "Above, under Input: the host app captures from BlackHole 2ch. It is "
     "picked by default when present; press ↻ after installing it. Never pick a microphone: "
     "that records the room instead.", []),
    ("Start PD Audio Monitor Receiver on the Playdate", "Install PDAudioMonitorReceiver.pdx, start it, "
     "and keep the Playdate connected over USB and unlocked.", []),
    ("Allow microphone access", "macOS asks when the host app first captures: that is how it lets it hear "
     "BlackHole. Nothing is recorded from a microphone.",
     [("Microphone settings", lambda: subprocess.run(["open", MICROPHONE_SETTINGS]))]),
]

TROUBLESHOOTING = TROUBLESHOOTING_WINDOWS if WINDOWS else [
    ("Wrong input", "Pick BlackHole 2ch under Input (↻ refreshes the list). A microphone records the room, "
     "and other inputs stay silent.", []),
    ("Peak stays at -99 dBFS", "Nothing reaches BlackHole: check the Mac's (or the DAW's) output device. "
     "If the microphone row isn't \"Authorized\", allow PD Audio Monitor Host in Microphone settings, then "
     "press Reopen: macOS applies the change only to a new launch."),
    ("Microphone access lost after rebuilding", "macOS ties the permission to the app's signature, which "
     "changes with each build. Remove PD Audio Monitor Host from the Microphone list (or run "
     "`tccutil reset Microphone nl.ninovanhooff.pdstream`), then reopen it and allow access again."),
    ("Waiting for a Playdate", "Plug it in and unlock it. Quit the Playdate Simulator: it also holds the "
     "Playdate's serial port."),
    ("Falling behind, or dropouts", "The USB link is slower than the format needs: pick a lighter one."),
    ("\"PD Audio Monitor Host can't be opened\"", "macOS blocks unsigned apps that were downloaded: "
     "right-click it, choose Open, or allow it under System Settings → Privacy & Security."),
]


class GuiConsole:
    """pdstream's output, handed to the window through a queue (Tk only works on its own thread)."""

    def __init__(self):
        self.events = queue.Queue()

    def log(self, text):
        self.events.put(("log", text))

    def status(self, text):
        self.events.put(("status", text.strip()))

    def finish(self):
        pass


class App:
    def __init__(self, root):
        self.root = root
        self.console = pdstream.console = GuiConsole()
        self.lib = pdstream.load_encoder()
        self.formats = [self.lib.pdstream_format_name(i).decode() for i in range(self.lib.pdstream_format_count())]
        self.worker = self.stop = self.sink = self.codec = None
        self.restart = False
        self.settings = pdstream.load_settings()

        root.title(f"{pdstream.APP_NAME} v{pdstream.app_version()}")
        self.console.log(f"{pdstream.APP_NAME} v{pdstream.app_version()}")
        root.geometry("820x600")
        root.minsize(700, 420)
        main = ttk.Frame(root, padding=12)
        main.pack(fill="both", expand=True)

        status = ttk.LabelFrame(main, text="Status", padding=8)
        status.pack(fill="x")
        status.columnconfigure(1, weight=1)
        self.rows = {}
        for row, name in enumerate(("Playdate", "Microphone", "Stream")):
            if name == "Microphone" and sys.platform != "darwin":
                continue
            ttk.Label(status, text=name).grid(row=row, column=0, sticky="w", padx=(0, 12), pady=2)
            self.rows[name] = ttk.Label(status, text="-")
            self.rows[name].grid(row=row, column=1, sticky="w")
        self.reopen = None
        if sys.platform == "darwin":
            ttk.Button(status, text="Microphone settings",
                       command=lambda: subprocess.run(["open", MICROPHONE_SETTINGS])).grid(row=1, column=2, sticky="e")
            # macOS applies access changed in Settings only to a new process.
            self.reopen = ttk.Button(status, text="Reopen", command=self.reopen_app)
            self.reopen.grid(row=1, column=3, sticky="e", padx=(4, 0))

        controls = ttk.Frame(main, padding=(0, 10))
        controls.pack(fill="x")
        ttk.Label(controls, text="Input").pack(side="left")
        self.device = ttk.Combobox(controls, state="readonly", width=24)
        self.device.pack(side="left", padx=(4, 12))
        self.device.bind("<<ComboboxSelected>>", lambda event: (self.remember(device=self.device.get()), self.restart_stream()))
        ttk.Label(controls, text="Format").pack(side="left")
        self.format = ttk.Combobox(controls, state="readonly", width=30,
                                   values=[f"{i}  {name}" for i, name in enumerate(self.formats)])
        saved = self.settings.get("format")
        self.format.current(saved if isinstance(saved, int) and 0 <= saved < len(self.formats)
                            else self.lib.pdstream_default_format())
        self.format.pack(side="left", padx=(4, 12))
        self.format.bind("<<ComboboxSelected>>", lambda event: self.pick_format())
        self.adpcm_xq = tk.BooleanVar(value=self.settings.get("adpcm_xq") is True)
        self.xq_toggle = ttk.Checkbutton(controls, text="ADPCM-XQ", variable=self.adpcm_xq, command=self.pick_adpcm_xq)
        self.xq_toggle.pack(side="left")
        self.update_xq_toggle()
        self.toggle = ttk.Button(controls, text="Stop", command=self.toggle_stream)
        self.toggle.pack(side="right")
        ttk.Button(controls, text="↻", width=2, command=lambda: (self.list_devices(), self.restart_stream())).pack(side="right", padx=4)

        tabs = ttk.Notebook(main)
        tabs.pack(fill="both", expand=True)
        self.log = scrolledtext.ScrolledText(tabs, height=10, state="disabled", wrap="word",
                                             font=("Consolas" if WINDOWS else "Menlo", 11), borderwidth=0)
        tabs.add(self.log, text="Log")
        tabs.add(self.guide(SETUP, numbered=True), text="Setup")
        tabs.add(self.guide(TROUBLESHOOTING), text="Troubleshooting")

        self.list_devices()
        self.start_stream()
        self.poll()

    def guide(self, items, numbered=False):
        heading_font = font.nametofont("TkDefaultFont").copy()
        heading_font.configure(weight="bold")
        frame = ttk.Frame(self.root, padding=12)
        frame.columnconfigure(0, weight=1)
        for row, item in enumerate(items):
            heading, text, buttons = item if len(item) == 3 else (*item, [])
            ttk.Label(frame, text=f"{row + 1}. {heading}" if numbered else heading,
                      font=heading_font).grid(row=2 * row, column=0, sticky="w", pady=(8, 0))
            ttk.Label(frame, text=text, wraplength=540, justify="left").grid(row=2 * row + 1, column=0, sticky="w")
            for column, (label, command) in enumerate(buttons, 1):
                ttk.Button(frame, text=label, command=command).grid(row=2 * row, column=column, rowspan=2, sticky="e")
        return frame

    def list_devices(self):
        try:
            names = [d["name"] for d in pdstream.input_devices()]
        except Exception as error:  # PortAudio can fail to list devices; the window must still open.
            names = []
            self.write_log(f"Couldn't list the input devices: {error}")
        self.device["values"] = names
        if self.device.get() in names:
            return
        saved = self.settings.get("device")
        if saved in names:
            self.device.set(saved)
            return
        try:
            virtual = pdstream.preferred_input()
        except Exception:
            virtual = None
        self.device.set(virtual["name"] if virtual else names[0] if names else "")
        if saved:
            self.write_log(f"Input {saved!r} not found: using {self.device.get() or 'no input'}. Pick another under Input, then press ↻.")
        elif not virtual:
            self.write_log(f"{pdstream.PREFERRED_DEVICES[0]} not found: see Setup, then press ↻ and pick it under Input.")

    def remember(self, **values):
        self.settings.update(values)
        pdstream.save_settings(self.settings)

    def write_log(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        lines = int(self.log.index("end-1c").split(".")[0])
        if lines > LOG_LINES:
            self.log.delete("1.0", f"{lines - LOG_LINES}.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def start_stream(self):
        fmt = self.format.current()
        self.stop = threading.Event()
        self.codec = pdstream.Codec(self.lib, fmt, self.adpcm_xq.get())
        self.sink = pdstream.SerialSink(None, verbose=True)
        stats = pdstream.Stats(self.formats[fmt])
        self.worker = threading.Thread(target=self.run, daemon=True,
                                       args=(self.device.get(), self.codec, self.sink, stats, self.stop))
        self.worker.start()
        self.toggle.configure(text="Stop")

    def run(self, device, codec, sink, stats, stop):
        try:
            pdstream.stream_device(device, codec, sink, stats, self.formats, stop)
        except SystemExit as error:
            self.console.log(str(error.code))
        except Exception as error:  # Shown in the log rather than lost with the thread.
            self.console.log(f"Stopped: {error}")
        finally:
            sink.close()
            self.console.events.put(("stopped", None))

    def stop_stream(self):
        if self.stop:
            self.stop.set()

    def restart_stream(self):
        self.write_log(f"Restarting the stream from {self.device.get() or 'no input'}")
        if self.running():
            self.restart = True
            self.stop_stream()
        else:
            self.start_stream()

    def toggle_stream(self):
        if self.running():
            self.restart = False
            self.stop_stream()
        else:
            self.start_stream()

    def running(self):
        return self.worker is not None and self.worker.is_alive() and not self.stop.is_set()

    def reopen_app(self):
        app = sys.executable.split(".app/Contents/")[0] + ".app" if ".app/Contents/" in sys.executable else None
        subprocess.Popen(["open", "-n", app] if app else [sys.executable, *sys.argv])
        self.root.destroy()

    def update_xq_toggle(self):
        # ADPCM-XQ replaces the encoder of the ADPCM formats only.
        adpcm = self.lib.pdstream_is_adpcm(self.format.current())
        self.xq_toggle.state(["!disabled"] if adpcm else ["disabled"])

    def pick_adpcm_xq(self):
        self.remember(adpcm_xq=self.adpcm_xq.get())
        if self.codec:
            self.codec.adpcm_xq = self.adpcm_xq.get()

    def pick_format(self):
        self.remember(format=self.format.current())
        self.update_xq_toggle()
        if self.running():
            self.sink.requested_format = self.format.current()  # as if the Playdate asked: the stream thread switches

    def poll(self):
        while True:
            try:
                kind, text = self.console.events.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                self.write_log(text)
            elif kind == "status":
                self.rows["Stream"].configure(text=text)
            elif kind == "stopped" and not (self.worker and self.worker.is_alive() and not self.stop.is_set()):
                self.rows["Stream"].configure(text="Stopped")
                self.toggle.configure(text="Start")
                if self.restart:
                    self.restart = False
                    self.start_stream()

        if self.reopen:
            access = pdstream.microphone_access()
            self.rows["Microphone"].configure(text={
                None: "-", "authorized": "Authorized",
                "not determined": "Not asked yet: macOS asks when the capture starts",
                "denied": f"Denied: allow {pdstream.microphone_app()} in Microphone settings, then reopen this app",
                "restricted": "Restricted by a profile or parental controls"}[access])
            if access in (None, "authorized"):
                self.reopen.grid_remove()
            else:
                self.reopen.grid()

        port = self.sink.port if self.sink and self.running() else None
        if port and not self.sink.listening() and self.sink.requested_format != -1:
            playdate = f"Connected on {port.port}: start PD Audio Monitor Receiver on the Playdate"
        else:
            playdate = (f"Connected on {port.port}" if port else "-" if not self.running() else
                        "Waiting: plug it in over USB, unlock it, and start PD Audio Monitor Receiver")
        self.rows["Playdate"].configure(text=playdate)
        if self.codec and self.running() and self.format.current() != self.codec.format:
            self.format.current(self.codec.format)
            self.remember(format=self.codec.format)
            self.update_xq_toggle()
        self.root.after(250, self.poll)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
