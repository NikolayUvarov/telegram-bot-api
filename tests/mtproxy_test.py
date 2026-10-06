#!/usr/bin/env python3
"""Tests of the --mtproxy option.

A fake MTProxy accepts connections of TDLib and records their first bytes, so the test checks that the server
connects to Telegram through the proxy. Telegram itself isn't needed.

    python3 tests/mtproxy_test.py build/telegram-bot-api
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

SERVER = sys.argv[1] if len(sys.argv) > 1 else "build/telegram-bot-api"
HTTP_PORT = 18081
TOKEN = "123456:AAHtest-token-for-mtproxy-tests-0000"
DOMAIN = b"example.com"
SECRET_KEY = "5ec7e75ec7e75ec7e75ec7e75ec7e75e"  # 16 bytes of the key of the proxy, must not get to logs
# fake TLS secret: "ee", the key, the domain imitated by the proxy
EE_SECRET = "ee" + SECRET_KEY + DOMAIN.hex()
DD_SECRET = "dd" + SECRET_KEY


def base64_secret_with_plus():
    """Fake TLS secret in base64 with '+', which is decoded from a link as a space"""
    import base64
    for i in range(256):
        raw = bytes.fromhex("ee" + SECRET_KEY[:-2] + "%02x" % i) + DOMAIN
        encoded = base64.b64encode(raw).decode()
        if "+" in encoded:
            return encoded
    raise AssertionError("no base64 secret with '+'")


class FakeMTProxy:
    """Records the first bytes of every connection and closes it"""

    def __init__(self):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(16)
        self.port = self.listener.getsockname()[1]
        self.connections = []
        self.lock = threading.Lock()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            conn.settimeout(3)
            data = b""
            try:
                while len(data) < 64:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    data += chunk
            except OSError:
                pass
            conn.close()
            with self.lock:
                self.connections.append(data)

    def wait_connection(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if self.connections:
                    return self.connections[0]
            time.sleep(0.2)
        return None

    def close(self):
        self.listener.close()


def server_env(mtproxy=None):
    env = dict(os.environ)
    env.pop("TELEGRAM_MTPROXY", None)
    if mtproxy is not None:
        env["TELEGRAM_MTPROXY"] = mtproxy
    return env


def run_server(work_dir, *options, mtproxy_env=None):
    log = os.path.join(work_dir, "server.log")
    if os.path.exists(log):
        os.remove(log)
    args = [SERVER, "--api-id=1", "--api-hash=0123456789abcdef0123456789abcdef", "--dir=" + work_dir,
            "--http-port=%d" % HTTP_PORT, "--verbosity=2", "--log=" + log]
    return subprocess.Popen(args + list(options), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=server_env(mtproxy_env))


def start_error(*options, mtproxy_env=None):
    """Runs the server, which must exit at once, and returns its error"""
    result = subprocess.run([SERVER, "--api-id=1", "--api-hash=x"] + list(options), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=10, env=server_env(mtproxy_env))
    prefix = SERVER + ": "
    errors = [line[len(prefix):] for line in result.stdout.decode(errors="replace").splitlines()
              if line.startswith(prefix)]
    return result.returncode, errors[0] if errors else ""


def request_bot(timeout=3):
    """Makes the server create the bot, which starts connecting to Telegram"""
    try:
        urllib.request.urlopen("http://127.0.0.1:%d/bot%s/getMe" % (HTTP_PORT, TOKEN), timeout=timeout)
    except Exception:
        pass  # there is no Telegram, the request fails


def wait_http():
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", HTTP_PORT), timeout=1).close()
            return
        except OSError:
            time.sleep(0.1)
    raise AssertionError("the server doesn't listen on the port %d" % HTTP_PORT)


def stop(server):
    server.terminate()
    try:
        server.wait(10)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()


def check_connection(work_dir, proxy, link, check, description, use_env=False):
    with proxy.lock:
        proxy.connections.clear()
    link = link.replace("PORT", str(proxy.port))
    if use_env:
        server = run_server(work_dir, mtproxy_env=link)
    else:
        server = run_server(work_dir, "--mtproxy=" + link)
    try:
        wait_http()
        threading.Thread(target=request_bot, daemon=True).start()
        data = proxy.wait_connection(30)
        assert data is not None, description + ": TDLib didn't connect to the MTProxy"
        assert check(data), description + ": wrong first bytes " + data[:80].hex()
    finally:
        stop(server)
    with open(os.path.join(work_dir, "server.log"), errors="replace") as f:
        log = f.read()
    assert "Connect to Telegram through MTProxy 127.0.0.1:%d" % proxy.port in log, description + ": no proxy in the log"
    assert SECRET_KEY not in log, description + ": the secret is in the log"
    print("ok  ", description)


def main():
    failed = False
    work_dir = tempfile.mkdtemp(prefix="mtproxy_test_")
    try:
        # wrong values: the server must exit with an error about the MTProxy
        for value in ["", "host:443", "host:0:" + DD_SECRET, "host:443:nothex", "socks5://host:1080",
                      "https://example.com/proxy?server=host&port=443&secret=" + DD_SECRET,
                      "tg://socks?server=host&port=1080"]:
            for use_env in [False, True]:
                if use_env and value == "":
                    continue  # an empty variable means no proxy
                if use_env:
                    code, error = start_error(mtproxy_env=value)
                    expected = "Invalid TELEGRAM_MTPROXY: "
                else:
                    code, error = start_error("--mtproxy=" + value)
                    expected = ""
                where = "TELEGRAM_MTPROXY" if use_env else "--mtproxy"
                if code == 0 or not error.startswith(expected) or "MTProxy" not in error:
                    print("FAIL wrong %s=%r: exit code %d, error %r" % (where, value, code, error))
                    failed = True
                elif SECRET_KEY in error:
                    print("FAIL wrong %s=%r: the secret is in the error %r" % (where, value, error))
                    failed = True
                else:
                    print("ok   wrong %s=%r: %s" % (where, value, error))

        proxy = FakeMTProxy()
        try:
            # fake TLS: the connection starts with TLS ClientHello with the domain of the secret
            is_fake_tls = lambda data: data.startswith(b"\x16\x03\x01") and DOMAIN in data
            check_connection(work_dir, proxy, "tg://proxy?server=127.0.0.1&port=PORT&secret=" + EE_SECRET,
                             is_fake_tls, "tg:// link with ee secret")
            check_connection(work_dir, proxy, "https://t.me/proxy?server=127.0.0.1&port=PORT&secret=" + EE_SECRET,
                             is_fake_tls, "https://t.me link with ee secret")
            check_connection(work_dir, proxy, "tg://proxy?server=127.0.0.1&port=PORT&secret=" + base64_secret_with_plus(),
                             is_fake_tls, "tg:// link with base64 secret with '+'")
            # obfuscated connection: 64 bytes of the header
            check_connection(work_dir, proxy, "127.0.0.1:PORT:" + DD_SECRET, lambda data: len(data) >= 64,
                             "host:port:secret with dd secret")
            check_connection(work_dir, proxy, "127.0.0.1:PORT:" + EE_SECRET, is_fake_tls,
                             "TELEGRAM_MTPROXY environment variable", use_env=True)

            # without --mtproxy the proxy saved in the database of the bot is disabled
            with proxy.lock:
                proxy.connections.clear()
            server = run_server(work_dir)
            try:
                wait_http()
                threading.Thread(target=request_bot, daemon=True).start()
                if proxy.wait_connection(10) is not None:
                    print("FAIL the proxy is used without --mtproxy")
                    failed = True
                else:
                    print("ok   the proxy is disabled without --mtproxy")
            finally:
                stop(server)
        finally:
            proxy.close()
    except AssertionError as e:
        print("FAIL", e)
        failed = True
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
