import base64
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock
from urllib.error import HTTPError

from npm_package import NAME, pack, registry_status, verify


class NpmPackageTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.exe = Path(directory.name, "release-setup.exe")
        self.exe.write_bytes(b"MZ-synthetic, never executed")
        self.archive = Path(directory.name, "package.tgz")
        self.version = "1.2.3"
        pack(self.version, self.exe, self.archive)

    def test_pairs_and_reproducibility(self):
        verify(self.version, self.exe, self.archive)
        previous = self.archive.read_bytes()
        os.utime(self.exe, (123456, 123456))
        pack(self.version, self.exe, self.archive)
        self.assertEqual(previous, self.archive.read_bytes())
        with self.assertRaises(ValueError):
            verify("1.2.4", self.exe, self.archive)
        self.exe.write_bytes(b"MZ-another installer content")
        with self.assertRaises(ValueError):
            verify(self.version, self.exe, self.archive)

    def test_beta_package(self):
        pack("1.2.3-beta.10", self.exe, self.archive)
        verify("1.2.3-beta.10", self.exe, self.archive)
        with self.assertRaises(ValueError):
            verify("1.2.3", self.exe, self.archive)
        with self.assertRaises(ValueError):
            pack("1.2.3-beta.01", self.exe, self.archive)

    def registry_record(self):
        archive = self.archive.read_bytes()
        digest = "sha512-" + base64.b64encode(hashlib.sha512(archive).digest()).decode()
        return {"name": NAME, "version": self.version, "dist": {
            "integrity": digest, "tarball": "https://registry.npmjs.org/archive.tgz"}}

    def test_new_metadata_can_precede_tarball_propagation(self):
        record = self.registry_record()
        responses = [
            HTTPError("version", 404, "Not Found", {}, None),
            io.BytesIO(json.dumps(record).encode()), HTTPError(record["dist"]["tarball"], 404, "Not Found", {}, None),
            io.BytesIO(json.dumps(record).encode()), io.BytesIO(self.archive.read_bytes()),
        ]
        fetch = Mock(side_effect=responses)
        for expected in ("absent", "pending", "identical"):
            self.assertEqual(registry_status(self.version, self.archive, fetch), expected)

    def test_registry_errors_are_not_pending(self):
        record = self.registry_record()
        for responses in (
            [HTTPError("version", 503, "Unavailable", {}, None)],
            [io.BytesIO(json.dumps(record).encode()), HTTPError(record["dist"]["tarball"], 503, "Unavailable", {}, None)],
        ):
            with self.assertRaises(HTTPError):
                registry_status(self.version, self.archive, Mock(side_effect=responses))
        with self.assertRaisesRegex(ValueError, "tarball differs"):
            registry_status(self.version, self.archive, Mock(side_effect=[
                io.BytesIO(json.dumps(record).encode()), io.BytesIO(b"different")]))
        for field, wrong in (("name", "@other/package"), ("version", "2.0.0"),
                             ("integrity", "sha512-incorrect"), ("tarball", "https://example.org/archive.tgz")):
            with self.subTest(field=field):
                record = self.registry_record()
                (record if field in ("name", "version") else record["dist"])[field] = wrong
                fetch = Mock(side_effect=[io.BytesIO(json.dumps(record).encode())])
                with self.assertRaises(ValueError):
                    registry_status(self.version, self.archive, fetch)
                fetch.assert_called_once()


if __name__ == "__main__":
    unittest.main()
