"""Independent ZIP/ZIP64 interoperability, using only Python's standard library."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


class SequentialOutput(io.BytesIO):
    def seekable(self):
        return False

    def seek(self, *args):
        raise io.UnsupportedOperation("sequential fixture")


def run(executable):
    with tempfile.TemporaryDirectory(prefix="cceda-interop-") as directory:
        root = Path(directory)
        output = root / "writer.cceda"
        subprocess.run([executable, "--write", str(output)], check=True)
        with zipfile.ZipFile(output) as archive:
            assert archive.testzip() is None
            assert archive.namelist()[-1] == "manifest.json"
            manifest = json.loads(archive.read("manifest.json"))
            data = {name: archive.read(name) for name in archive.namelist()}
            for entry in manifest["entries"]:
                content = data[entry["path"]]
                assert len(content) == entry["uncompressedBytes"]
                assert hashlib.sha256(content).hexdigest() == entry["sha256"]
        assert b"PK\x06\x06" in output.read_bytes()
        for streaming in (False, True):
            for zip64 in (False, True):
                buffer = SequentialOutput() if streaming else io.BytesIO()
                with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                    for name, content in data.items():
                        with archive.open(name, "w", force_zip64=zip64) as entry:
                            entry.write(content)
                fixture = root / f"python-{streaming}-{zip64}.cceda"
                fixture.write_bytes(buffer.getvalue())
                subprocess.run([executable, "--read", str(fixture)], check=True)
        print("Python ZIP32/ZIP64 interoperability passed (seekable and sequential).")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
