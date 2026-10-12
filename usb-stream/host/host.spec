# PyInstaller spec for "PD Audio Monitor Host": the .app on macOS (`make app`), a single windowed .exe on Windows.
# On macOS the usage description is what lets macOS ask microphone access for the app itself; without it,
# macOS ends the app on its first capture.
import glob
import os
import sys

name = "PD Audio Monitor Host"
bundle_id = "nl.ninovanhooff.pdstream"
here = SPECPATH
library = "libpdstream.dll" if sys.platform == "win32" else "libpdstream.dylib"
# The receiver's pdxinfo is the one place the version is set.
pdxinfo = os.path.join(here, "..", "receiver", "Source", "pdxinfo")
with open(pdxinfo) as info:
    version = next(line.split("=", 1)[1].strip() for line in info if line.startswith("version="))
analysis = Analysis([os.path.join(here, "pdstream_gui.py")], datas=[(pdxinfo, ".")],
                    binaries=[(path, ".") for path in glob.glob(os.path.join(here, library))])
if sys.platform == "win32":
    exe = EXE(PYZ(analysis.pure), analysis.scripts, analysis.binaries, analysis.datas, name=name, console=False)
else:
    exe = EXE(PYZ(analysis.pure), analysis.scripts, exclude_binaries=True, name=name, console=False)
    app = BUNDLE(COLLECT(exe, analysis.binaries, analysis.datas, name="host-app"), name=name + ".app",
                 bundle_identifier=bundle_id,
                 info_plist={
                     "CFBundleName": name,
                     "CFBundleDisplayName": name,
                     "CFBundleShortVersionString": version,
                     "NSMicrophoneUsageDescription":
                         "PD Audio Monitor Host captures BlackHole, the virtual audio device your Mac plays into, "
                         "to stream it to the Playdate. macOS treats every audio input as a microphone.",
                     "NSHighResolutionCapable": True,
                 })
