"""Keep one byte-identical copy of shared Mesa objects; retain whole RADV.

EGL, Gallium and RADV release archives each bundle Mesa's common utilities.
Removing their identical copies preserves strict duplicate-symbol errors for
every other object instead of asking the linker to ignore conflicting code.
The original SDK archives are never edited.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def members(path):
    raw = path.read_bytes()
    if not raw.startswith(b"!<arch>\n"):
        raise ValueError(f"Not a regular archive: {path}")
    result, names = {}, b""
    pos = 8
    while pos < len(raw):
        header = raw[pos:pos + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise ValueError(f"Malformed archive at {pos}: {path}")
        size = int(header[48:58])
        body = raw[pos + 60:pos + 60 + size]
        if len(body) != size:
            raise ValueError(f"Truncated archive: {path}")
        name = header[:16].decode("ascii").strip()
        pos += 60 + size + (size % 2)
        if name == "//":
            names = body
            continue
        if name in ("/", "/SYM64/"):
            continue
        if name.startswith("#1/"):
            length = int(name[3:])
            name = body[:length].rstrip(b"\0").decode()
            body = body[length:]
        elif name.startswith("/") and name[1:].isdigit():
            offset = int(name[1:])
            name = names[offset:].split(b"/\n", 1)[0].decode()
        else:
            name = name.rstrip("/")
        if name in result:
            raise ValueError(f"Repeated member name {name}: {path}")
        result[name] = body
    return result


def prepare(radv, gallium, egl, output, ar):
    canonical = members(radv)
    canonical_hashes = {hashlib.sha256(body).digest(): name for name, body in canonical.items()}
    output.mkdir(parents=True, exist_ok=True)
    report = {"canonical_radv_sha256": hashlib.sha256(radv.read_bytes()).hexdigest(), "archives": []}
    for source, filename in ((gallium, "libGallium.rpcs3.a"), (egl, "libEGL.rpcs3.a")):
        objects = members(source)
        duplicates = []
        for name, body in objects.items():
            digest = hashlib.sha256(body).digest()
            if digest in canonical_hashes:
                duplicates.append(name)
            elif name in canonical:
                if body != canonical[name]:
                    raise ValueError(f"Conflicting Mesa member {name}: {source}")
                duplicates.append(name)
            else:
                canonical[name] = body
                canonical_hashes[digest] = name
        destination = output / filename
        shutil.copyfile(source, destination)
        # Keep command lines comfortably below the Windows limit.
        for start in range(0, len(duplicates), 80):
            subprocess.run([ar, "d", str(destination), *duplicates[start:start + 80]], check=True)
        subprocess.run([ar, "s", str(destination)], check=True)
        retained = members(destination)
        expected = {name: body for name, body in objects.items() if name not in duplicates}
        if retained != expected:
            raise ValueError(f"Prepared archive differs from its retained input members: {destination}")
        report["archives"].append({"source": str(source), "output": str(destination),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "output_sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
            "removed_identical_members": duplicates, "retained_member_count": len(retained)})
    (output / "mesa-gl-archives.json").write_text(json.dumps(report, indent=2))
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for arg in ("radv", "gallium", "egl", "output", "ar"):
        parser.add_argument("--" + arg, required=True)
    args = parser.parse_args()
    report = prepare(Path(args.radv), Path(args.gallium), Path(args.egl), Path(args.output), args.ar)
    for archive in report["archives"]:
        print(f"{archive['output']}: {archive['retained_member_count']} retained, "
              f"{len(archive['removed_identical_members'])} byte-identical shared members removed")
