import base64
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
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

    def test_registry_retries(self):
        archive = self.archive.read_bytes()
        digest = "sha512-" + base64.b64encode(hashlib.sha512(archive).digest()).decode()
        record = {"name": NAME, "version": self.version, "dist": {
            "integrity": digest, "tarball": "https://registry.npmjs.org/archive.tgz"}}

        def get(tarball):
            def fetch(url, timeout):
                return io.BytesIO(tarball if url.endswith(".tgz") else json.dumps(record).encode())
            return fetch

        self.assertEqual(registry_status(self.version, self.archive, get(archive)), "identical")
        with self.assertRaises(ValueError):
            registry_status(self.version, self.archive, get(b"different"))
        record["dist"]["integrity"] = "sha512-incorrect"
        with self.assertRaises(ValueError):
            registry_status(self.version, self.archive, get(archive))

        def missing(url, timeout):
            raise HTTPError(url, 404, "Not Found", {}, None)

        def unavailable(url, timeout):
            raise HTTPError(url, 503, "Unavailable", {}, None)

        self.assertEqual(registry_status(self.version, self.archive, missing), "absent")
        with self.assertRaises(HTTPError):
            registry_status(self.version, self.archive, unavailable)


if __name__ == "__main__":
    unittest.main()
