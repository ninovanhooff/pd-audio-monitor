# PyInstaller spec for "PD Audio Monitor Host.app" (`make app`). The usage description is what lets macOS
# ask microphone access for the app itself; without it, macOS ends the app on its first capture.
import glob
import os

name = "PD Audio Monitor Host"
bundle_id = "nl.ninovanhooff.pdstream"
here = SPECPATH
analysis = Analysis([os.path.join(here, "pdstream_gui.py")],
                    binaries=[(path, ".") for path in glob.glob(os.path.join(here, "libpdstream.dylib"))])
exe = EXE(PYZ(analysis.pure), analysis.scripts, exclude_binaries=True, name=name, console=False)
app = BUNDLE(COLLECT(exe, analysis.binaries, analysis.datas, name="host-app"), name=name + ".app",
             bundle_identifier=bundle_id,
             info_plist={
                 "CFBundleName": name,
                 "CFBundleDisplayName": name,
                 "CFBundleShortVersionString": "1.0",
                 "NSMicrophoneUsageDescription":
                     "PD Audio Monitor Host captures BlackHole, the virtual audio device your Mac plays into, "
                     "to stream it to the Playdate. macOS treats every audio input as a microphone.",
                 "NSHighResolutionCapable": True,
             })
