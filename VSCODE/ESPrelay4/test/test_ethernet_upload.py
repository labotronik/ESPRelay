"""Run with python3 -m unittest discover -s test -p 'test_ethernet_upload.py'."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit

spec = importlib.util.spec_from_file_location(
    "ethernet_upload", Path(__file__).resolve().parents[1] / "scripts/ethernet_upload.py")
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


class Env(dict):
    def GetProjectOption(self, key, default):
        return self.get(key, default)

    def subst(self, value):
        return {"$PROJECT_DATA_DIR": self["data"], "$PROJECT_DIR": self["project"],
                "$PIOENV": "ethernet"}.get(value, value)

    def PioPlatform(self):
        return self

    def get_package_dir(self, name):
        return self["tool"]


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.env = Env(UPLOAD_PORT="192.0.2.1", custom_ota_password="test",
                       data=str(self.root / "data"), project=str(self.root), tool=str(self.root))
        (self.root / "data").mkdir()
        (self.root / "data/index.html").write_text("new interface")
        self.settings = {key: {"value": key} for key in ("rules", "net", "mqtt", "wifi", "ble")}

    def image(self, name):
        image = self.root / name
        image.write_bytes(b"image")
        return image

    def test_firmware_and_filesystem_routes(self):
        with patch.object(ota.http.client, "HTTPConnection") as factory, \
                patch.object(ota, "filesystem_payload", return_value=b"preserved image"):
            response = factory.return_value.getresponse.return_value
            response.status = 200
            response.read.return_value = b'{"ok":true}'
            for name, endpoint in (("firmware.bin", "/api/ota"), ("littlefs.bin", "/api/otafs")):
                self.assertEqual(ota.upload_firmware([self.image(name)], [], self.env), 0)
                self.assertEqual(factory.return_value.request.call_args.args, ("POST", endpoint))
            response.status = 401
            self.assertEqual(ota.upload_firmware([self.image("firmware.bin")], [], self.env), 1)

    def test_incomplete_backup_prevents_filesystem_post(self):
        with patch.object(ota.http.client, "HTTPConnection") as factory:
            response = factory.return_value.getresponse.return_value
            response.status = 200
            response.read.return_value = b'{"rules":{},"net":{},"mqtt":{}}'
            self.assertEqual(ota.upload_firmware([self.image("littlefs.bin")], [], self.env), 1)
            self.assertEqual(factory.return_value.request.call_args.args, ("GET", "/api/backup"))

    def test_staged_image_contains_settings_without_changing_project(self):
        def build(args, check):
            staged = Path(args[args.index("-c") + 1])
            for name in ("rules", "net", "mqtt", "wifi", "ble"):
                self.assertEqual(json.loads((staged / (name + ".json")).read_text()), self.settings[name])
            self.assertEqual(json.loads((staged / "auth.json").read_text()),
                             {"user": "admin", "pass": "test"})
            self.assertEqual((staged / "index.html").read_text(), "new interface")
            Path(args[-1]).write_bytes(b"new preserved image")

        with patch.object(ota, "read_settings", return_value=self.settings), \
                patch.object(ota.subprocess, "run", side_effect=build):
            payload = ota.filesystem_payload(self.image("littlefs.bin"), self.env,
                                             urlsplit("http://192.0.2.1"), "token", "admin", "test")
        self.assertEqual(payload, b"new preserved image")
        self.assertEqual([p.name for p in (self.root / "data").iterdir()], ["index.html"])

    def test_automatic_web_upload_after_reboot(self):
        with patch.object(ota, "read_settings", return_value=self.settings), \
                patch.object(ota.time, "sleep"), patch.object(ota.subprocess, "call", return_value=0) as call:
            self.assertEqual(ota.upload_web_after_firmware([], [], self.env), 0)
            self.assertIn("uploadfs", call.call_args.args[0])
            self.env["custom_upload_web"] = "no"
            call.reset_mock()
            self.assertEqual(ota.upload_web_after_firmware([], [], self.env), 0)
            call.assert_not_called()


if __name__ == "__main__":
    unittest.main()
