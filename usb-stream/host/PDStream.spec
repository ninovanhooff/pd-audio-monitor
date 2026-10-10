# PyInstaller spec for PDStream.app (`make app`); its folder is PDStream-app, as macOS file names ignore case. The usage description is what lets macOS ask
# microphone access for the app itself; without it, macOS ends the app on its first capture.
import glob
import os

bundle_id = "nl.ninovanhooff.pdstream"
here = SPECPATH
analysis = Analysis([os.path.join(here, "pdstream_gui.py")],
                    binaries=[(path, ".") for path in glob.glob(os.path.join(here, "libpdstream.dylib"))])
exe = EXE(PYZ(analysis.pure), analysis.scripts, exclude_binaries=True, name="PDStream", console=False)
app = BUNDLE(COLLECT(exe, analysis.binaries, analysis.datas, name="PDStream-app"), name="PDStream.app",
             bundle_identifier=bundle_id,
             info_plist={
                 "CFBundleShortVersionString": "1.0",
                 "NSMicrophoneUsageDescription":
                     "PDStream captures BlackHole, the virtual audio device your Mac plays into, to stream it to "
                     "the Playdate. macOS treats every audio input as a microphone.",
                 "NSHighResolutionCapable": True,
             })
