"""PlatformIO uploader for the board's authenticated HTTP OTA API."""

import base64
import hashlib
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.parse import urlsplit


def read_settings(url, token):
    connection = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=5)
    try:
        connection.request("GET", "/api/backup", headers={"Authorization": "Basic " + token})
        response = connection.getresponse()
        if response.status != 200:
            raise ValueError("sauvegarde HTTP {}".format(response.status))
        settings = json.loads(response.read())
        for name in ("rules", "net", "mqtt", "wifi", "ble"):
            if not isinstance(settings.get(name), dict):
                raise ValueError("sauvegarde incomplète : {} ; mettre à jour le firmware d'abord".format(name))
        return settings
    finally:
        connection.close()


def filesystem_payload(image, env, url, token, user, password):
    settings = read_settings(url, token)
    settings["auth"] = {"user": user, "pass": password}
    tool_dir = env.PioPlatform().get_package_dir("tool-mklittlefs")
    tool = Path(tool_dir) / ("mklittlefs.exe" if os.name == "nt" else "mklittlefs")
    with tempfile.TemporaryDirectory(prefix="esprelay-ota-") as directory:
        staged = Path(directory) / "data"
        shutil.copytree(env.subst("$PROJECT_DATA_DIR"), staged)
        for name in ("rules", "net", "mqtt", "wifi", "ble", "auth"):
            (staged / (name + ".json")).write_text(json.dumps(settings[name]), encoding="utf-8")
        output = Path(directory) / "littlefs.bin"
        subprocess.run([str(tool), "-c", str(staged), "-s", str(image.stat().st_size),
                        "-p", "256", "-b", "4096", str(output)], check=True)
        print("OTA: réglages réseau, MQTT, règles, WiFi, BLE et identifiants conservés.")
        return output.read_bytes()


def upload_web_after_firmware(source, target, env):
    if env.GetProjectOption("custom_upload_web", "yes").lower() not in ("yes", "true", "1"):
        return 0
    destination = str(env.get("UPLOAD_PORT") or "").strip()
    url = urlsplit(destination if "://" in destination else "http://" + destination)
    user = os.environ.get("ESPRELAY_OTA_USER", env.GetProjectOption("custom_ota_user", "admin"))
    password = os.environ.get("ESPRELAY_OTA_PASSWORD", env.GetProjectOption("custom_ota_password", ""))
    token = base64.b64encode((user + ":" + password).encode()).decode()
    print("OTA: attente du redémarrage avant la mise à jour de l'interface web...")
    time.sleep(2)
    deadline = time.monotonic() + 60
    while True:
        try:
            read_settings(url, token)
            break
        except (OSError, ValueError, http.client.HTTPException):
            if time.monotonic() >= deadline:
                print("OTA: firmware envoyé, mais carte indisponible ou sauvegarde incomplète ; interface non mise à jour.")
                return 1
            time.sleep(2)
    return subprocess.call([sys.executable, "-m", "platformio", "run", "-e", env.subst("$PIOENV"),
                            "-t", "uploadfs", "--upload-port", destination], cwd=env.subst("$PROJECT_DIR"))


def upload_firmware(source, target, env):
    image = Path(str(source[0]))
    is_fs = image.name == "littlefs.bin"
    endpoint = "/api/otafs" if is_fs else "/api/ota"
    label = "LittleFS" if is_fs else "firmware"
    destination = str(env.get("UPLOAD_PORT") or "").strip()
    user = os.environ.get("ESPRELAY_OTA_USER", env.GetProjectOption("custom_ota_user", "admin"))
    password = os.environ.get("ESPRELAY_OTA_PASSWORD", env.GetProjectOption("custom_ota_password", ""))
    if not destination or not password:
        print("OTA: renseignez upload_port et custom_ota_password dans platformio.local.ini.")
        return 1
    url = urlsplit(destination if "://" in destination else "http://" + destination)
    if url.scheme != "http" or not url.hostname or url.username or url.password or url.path not in ("", "/") or url.query or url.fragment:
        print("OTA: upload_port doit être une adresse IP ou un nom DNS, avec port HTTP optionnel.")
        return 1
    connection = None
    try:
        token = base64.b64encode((user + ":" + password).encode()).decode()
        payload = filesystem_payload(image, env, url, token, user, password) if is_fs else image.read_bytes()
        if not payload:
            raise ValueError("fichier {} vide".format(label))
        connection = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=120)
        print("OTA: envoi de {} octets vers {}...".format(len(payload), destination))
        connection.request("POST", endpoint, body=payload, headers={
            "Content-Type": "application/octet-stream",
            "Authorization": "Basic " + token,
            "X-Checksum-Sha256": hashlib.sha256(payload).hexdigest(),
        })
        response = connection.getresponse()
        body = response.read().decode("utf-8", errors="replace")
        if response.status != 200:
            print("OTA: échec HTTP {}: {}".format(response.status, body))
            return 1
        result = json.loads(body)
        if result.get("ok") is not True:
            print("OTA: la carte n'a pas confirmé la mise à jour.")
            return 1
        print("OTA: {} accepté, redémarrage de la carte.".format(label))
        return 0
    except (OSError, ValueError, http.client.HTTPException, subprocess.CalledProcessError) as error:
        print("OTA: échec de la mise à jour: {}".format(error))
        return 1
    finally:
        if connection is not None:
            connection.close()


try:
    Import("env")
except NameError:
    pass
else:
    # Espressif's builder otherwise switches bare IP upload ports to espota.
    destination = str(env.get("UPLOAD_PORT") or "").strip()
    if destination and "://" not in destination:
        env.Replace(UPLOAD_PORT="http://" + destination)
    env.Replace(UPLOADCMD=upload_firmware)
    env.AddPostAction("upload", upload_web_after_firmware)
