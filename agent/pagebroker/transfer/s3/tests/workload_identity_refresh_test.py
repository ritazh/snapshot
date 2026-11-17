# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Exercise real SDK renewal during one upload batch or a native reader session.

Only loopback servers and obviously fake credentials are used. The first data
request rotates the token file and holds its response until credentials expire.
"""
import datetime
import hashlib
import http.server
import json
import os
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest

PROBE = str(pathlib.Path(sys.argv.pop(1)).resolve())

class IdentityRefreshTest(unittest.TestCase):
    def exercise(self, fail_refresh, https=False, trust_ca=True):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            token = root / "token"
            token.write_text("first-fake-token")
            provider_tokens, signing_keys, completed_transfers = [], [], []
            class Provider(http.server.BaseHTTPRequestHandler):
                def log_message(self, *args): pass
                def do_GET(self):
                    value = self.headers.get("Authorization")
                    provider_tokens.append(value)
                    if value not in ("first-fake-token", "rotated-fake-token") or (fail_refresh and value == "rotated-fake-token"):
                        self.send_error(401)
                        return
                    key = "FIRSTKEY" if value == "first-fake-token" else "ROTATEDKEY"
                    expiry = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=1)
                    data = json.dumps({"AccessKeyId": key, "SecretAccessKey": "fake-secret", "Token": "fake-session", "Expiration": expiry.strftime("%Y-%m-%dT%H:%M:%SZ")}).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
            class Storage(http.server.BaseHTTPRequestHandler):
                def log_message(self, *args): pass
                def do_HEAD(self):
                    self.send_error(404)
                def data_request(self, read):
                    auth = self.headers.get("Authorization", "")
                    signing_keys.append(auth)
                    expected = "FIRSTKEY" if len(signing_keys) == 1 else "ROTATEDKEY"
                    if "Credential=" + expected + "/" not in auth:
                        self.send_error(403)
                        return
                    if not read:
                        self.rfile.read(int(self.headers.get("Content-Length", "0")))
                    if len(signing_keys) == 1:
                        token.write_text("rotated-fake-token")
                        time.sleep(1.2)
                    completed_transfers.append(auth)
                    self.send_response(206 if read else 200)
                    self.send_header("ETag", '"' + hashlib.md5(b"snapshot").hexdigest() + '"')
                    self.send_header("Content-Length", "8" if read else "0")
                    if read: self.send_header("Content-Range", "bytes 0-7/8")
                    self.end_headers()
                    if read: self.wfile.write(b"snapshot")
                def do_PUT(self): self.data_request(False)
                def do_GET(self): self.data_request(True)
            provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
            storage = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Storage)
            if https:
                # A private test CA proves both credential TLS verification and
                # reuse of the existing AWS_CA_BUNDLE setting, without real keys.
                cert, key = root / "ca.pem", root / "server.key"
                subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                                "-nodes", "-keyout", str(key), "-out", str(cert),
                                "-days", "1", "-subj", "/CN=127.0.0.1",
                                "-addext", "subjectAltName=IP:127.0.0.1"],
                               check=True, capture_output=True)
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(cert, key)
                provider.socket = context.wrap_socket(provider.socket, server_side=True)
            for server in (provider, storage):
                threading.Thread(target=server.serve_forever, daemon=True).start()
            env = {k: v for k, v in os.environ.items() if not k.startswith(("AWS_", "RCLONE_"))}
            env.update(AWS_CONTAINER_CREDENTIALS_FULL_URI=f"{'https' if https else 'http'}://127.0.0.1:{provider.server_port}/credentials",
                       AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE=str(token), AWS_EC2_METADATA_DISABLED="true",
                       AWS_SHARED_CREDENTIALS_FILE="/dev/null", AWS_CONFIG_FILE="/dev/null")
            if https and trust_ca:
                env["AWS_CA_BUNDLE"] = str(cert)
            try:
                result = subprocess.run([PROBE, f"http://127.0.0.1:{storage.server_port}", str(root)], env=env, capture_output=True, text=True, timeout=25)
                if https and not trust_ca:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(provider_tokens, [], "untrusted TLS must not receive the token")
                    self.assertFalse(any("Credential=" in auth for auth in signing_keys),
                                     "untrusted identity must not supply signing credentials")
                    return
                self.assertIn("rotated-fake-token", provider_tokens)
                self.assertIn("Credential=FIRSTKEY/", signing_keys[0])
                if fail_refresh:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(len(completed_transfers), 1, "failed renewal must not complete another transfer")
                    self.assertNotIn("rotated-fake-token", result.stderr)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(len(signing_keys), 2)
                    self.assertIn("Credential=ROTATEDKEY/", signing_keys[1])
            finally:
                for server in (provider, storage):
                    server.shutdown()
                    server.server_close()
    def test_refresh_during_transfer(self): self.exercise(False)
    def test_failed_refresh_fails_transfer(self): self.exercise(True)
    def test_https_refresh_uses_configured_ca(self): self.exercise(False, https=True)
    def test_https_rejects_untrusted_ca(self): self.exercise(False, https=True, trust_ca=False)

if __name__ == "__main__": unittest.main()
