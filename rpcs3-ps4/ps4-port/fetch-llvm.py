"""Fetch only the LLVM revision pinned by this RPCS3 tree."""
from pathlib import Path
import hashlib
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent
COMMIT = "ca7933e47d3a3451d81e72ac174dcb5aa28b59d1"
SHA256 = "9dd0aba32a0c2b9e8e808e9b67502cc977f22220f67268bbfd937fa07e5ee6ce"
tree = subprocess.check_output(["git", "ls-tree", "HEAD", "3rdparty/llvm/llvm"], cwd=ROOT.parent, text=True)
if tree.split()[2] != COMMIT:
    raise RuntimeError("RPCS3's LLVM submodule changed; update the pin and verify its archive first")
deps = ROOT / "deps"
deps.mkdir(exist_ok=True)
name = "llvm-project-" + COMMIT
archive = deps / (name + ".tar.gz")
if not archive.exists():
    partial = archive.with_suffix(archive.suffix + ".part")
    request = urllib.request.Request("https://codeload.github.com/llvm/llvm-project/tar.gz/" + COMMIT,
                                     headers={"User-Agent": "RPCS3-PS4-source-build"})
    with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as output:
        while block := response.read(1024 * 1024):
            output.write(block)
    with partial.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != SHA256:
            raise RuntimeError("Downloaded LLVM archive hash mismatch; partial retained for inspection")
    partial.replace(archive)
with archive.open("rb") as stream:
    if hashlib.file_digest(stream, "sha256").hexdigest() != SHA256:
        raise RuntimeError("Cached LLVM archive hash mismatch")
source = deps / name
# Repeat extraction when the completion marker is absent, including recovery
# from an interrupted extraction. Never merge an unverified archive.
marker = source / ".rpcs3-extraction-complete"
if not marker.exists() or marker.read_text().strip() != SHA256:
    with tarfile.open(archive, "r:gz") as tf:
        members = [member for member in tf.getmembers()
                   if member.name.startswith(tuple(name + "/" + directory + "/"
                                                    for directory in ("llvm", "cmake", "third-party")))]
        for member in members:
            if not (deps / member.name).resolve().is_relative_to(deps.resolve()):
                raise RuntimeError("LLVM archive path escapes dependency directory")
        tf.extractall(deps, members=members, filter="data")
    marker.write_text(SHA256 + "\n")
print("Verified pinned LLVM source:", source)
