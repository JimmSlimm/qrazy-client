"""Read the original grapple PCM from the browser's shared sound pack."""
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import wave
import zipfile


def grapple_input(pack, cue):
    entry = f"sound/weapons/grapple/{cue}.wav"
    with wave.open(io.BytesIO(pack.read(entry)), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
            raise ValueError(f"{entry} must be mono 16-bit PCM")
        frames = wav.getnframes()
        pcm = struct.unpack(f"<{frames}h", wav.readframes(frames))
        return {"rate": wav.getframerate(), "pcm": pcm}


if __name__ == "__main__":
    node, game, pack_path, output = sys.argv[1:]
    with zipfile.ZipFile(pack_path) as pack:
        inputs = {cue: grapple_input(pack, cue) for cue in ("grapfire", "grappull")}
    script = Path(__file__).with_name("gen-weapon-audio.cjs")
    subprocess.run([node, script, game, output], input=json.dumps(inputs), text=True, check=True)
