"""Build and verify the npm mirror of one verified Windows installer."""
import argparse
import base64
import gzip
import hashlib
import hmac
import io
import json
from pathlib import Path
import re
import tarfile
from urllib.error import HTTPError
from urllib.parse import quote, urlsplit
from urllib.request import urlopen

NAME = "@lightsing/livehime-obs-win32-x64"
REGISTRY = "https://registry.npmjs.org"
INSTALLER = "livehime-obs-setup.exe"


def metadata(version):
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*)){2}(?:-beta\.(?:0|[1-9][0-9]*))?", version):
        raise ValueError("Invalid release version")
    return {
        "name": NAME,
        "version": version,
        "description": "Windows x64 installer for the Livehime OBS plugin",
        "license": "GPL-2.0-or-later",
        "repository": {"type": "git", "url": "git+https://github.com/lightsing/livehime-obs.git"},
        "os": ["win32"],
        "cpu": ["x64"],
    }


def pack(version, installer, output):
    manifest = (json.dumps(metadata(version), indent=2) + "\n").encode()
    with output.open("wb") as file, gzip.GzipFile(filename="", mode="wb", fileobj=file, mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode="w|", format=tarfile.USTAR_FORMAT) as archive:
            with installer.open("rb") as exe:
                for name, size, contents in [
                    ("package.json", len(manifest), io.BytesIO(manifest)),
                    (INSTALLER, installer.stat().st_size, exe),
                ]:
                    header = tarfile.TarInfo("package/" + name)
                    header.size = size
                    header.mode = 0o644
                    archive.addfile(header, contents)


def verify(version, installer, archive_path):
    expected_manifest = metadata(version)
    with tarfile.open(archive_path, "r:gz") as archive:
        members = archive.getmembers()
        if [member.name for member in members] != ["package/package.json", "package/" + INSTALLER]:
            raise ValueError("Unexpected npm archive contents")
        manifest, exe = members
        if any(not m.isfile() for m in members) or manifest.size > 4096 or exe.size != installer.stat().st_size:
            raise ValueError("Unexpected npm archive member type or size")
        if json.load(archive.extractfile(manifest)) != expected_manifest:
            raise ValueError("Unexpected npm package metadata")
        with installer.open("rb") as original, archive.extractfile(exe) as packed:
            if hashlib.file_digest(original, "sha256").digest() != hashlib.file_digest(packed, "sha256").digest():
                raise ValueError("The npm installer differs from the verified GitHub installer")


def registry_status(version, archive, fetch=urlopen):
    """Return absent, pending tarball propagation, or identical verified bytes."""
    metadata(version)
    url = f"{REGISTRY}/{quote(NAME, safe='@')}/{version}"
    try:
        response = fetch(url, timeout=30)
    except HTTPError as error:
        if error.code == 404:
            error.close()
            return "absent"
        raise
    with response:
        record = json.load(response)
    dist = record["dist"]
    if record["name"] != NAME or record["version"] != version:
        raise ValueError("npm returned another package or version")
    with archive.open("rb") as contents:
        digest = hashlib.file_digest(contents, "sha512").digest()
    sri = "sha512-" + base64.b64encode(digest).decode("ascii")
    if not hmac.compare_digest(dist["integrity"], sri):
        raise ValueError("Published npm integrity differs from the GitHub Release tarball")
    target = urlsplit(dist["tarball"])
    if target.scheme != "https" or target.netloc != "registry.npmjs.org":
        raise ValueError("Unexpected npm tarball origin")
    try:
        contents = fetch(target.geturl(), timeout=60)
    except HTTPError as error:
        if error.code == 404:
            error.close()
            return "pending"
        raise
    with contents:
        if hashlib.file_digest(contents, "sha512").digest() != digest:
            raise ValueError("Published npm tarball differs from the GitHub Release tarball")
    return "identical"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=["pack", "verify", "registry-status"])
    parser.add_argument("version")
    parser.add_argument("archive", type=Path)
    parser.add_argument("--installer", type=Path)
    args = parser.parse_args()
    if args.operation == "registry-status":
        print(registry_status(args.version, args.archive))
    else:
        if not args.installer:
            parser.error("--installer is required for pack and verify")
        if args.operation == "pack":
            pack(args.version, args.installer, args.archive)
        verify(args.version, args.installer, args.archive)
