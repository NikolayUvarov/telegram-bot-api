"""Helpers of tests: a fake MTProxy and the server"""

import hashlib
import json
import os
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

SECRET_KEY = "5ec7e75ec7e75ec7e75ec7e75ec7e75e"  # 16 bytes of the key of the proxy, must not get to logs
DOMAIN = b"example.com"
# fake TLS secret: "ee", the key, the domain imitated by the proxy
EE_SECRET = "ee" + SECRET_KEY + DOMAIN.hex()
DD_SECRET = "dd" + SECRET_KEY
API_HASH = "0123456789abcdef0123456789abcdef"


class FakeMTProxy:
    """Records the time and the first bytes of every connection and closes it"""

    def __init__(self):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(64)
        self.port = self.listener.getsockname()[1]
        self.connections = []  # (time, first bytes)
        self.lock = threading.Lock()
        threading.Thread(target=self.serve, daemon=True).start()

    def link(self, secret=DD_SECRET):
        return "tg://proxy?server=127.0.0.1&port=%d&secret=%s" % (self.port, secret)

    def serve(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            accepted = time.time()
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
                self.connections.append((accepted, data))

    def clear(self):
        with self.lock:
            self.connections.clear()

    def count(self, since=0.0):
        with self.lock:
            return len([c for c in self.connections if c[0] >= since])

    def wait_connection(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if self.connections:
                    return self.connections[0][1]
            time.sleep(0.2)
        return None

    def close(self):
        self.listener.close()


def server_env(mtproxy=None):
    env = dict(os.environ)
    for name in ["TELEGRAM_MTPROXY", "TELEGRAM_MTPROXY_FILE", "TELEGRAM_MTPROXY_ADMINS"]:
        env.pop(name, None)
    if mtproxy is not None:
        env["TELEGRAM_MTPROXY"] = mtproxy
    return env


class Server:
    """telegram-bot-api in the working directory"""

    def __init__(self, binary, work_dir, http_port):
        self.binary = binary
        self.work_dir = work_dir
        self.http_port = http_port
        self.process = None

    @property
    def log_path(self):
        return os.path.join(self.work_dir, "server.log")

    def start(self, *options, mtproxy_env=None):
        if os.path.exists(self.log_path):
            os.remove(self.log_path)
        args = [self.binary, "--api-id=1", "--api-hash=" + API_HASH, "--dir=" + self.work_dir,
                "--http-port=%d" % self.http_port, "--verbosity=2", "--log=" + self.log_path]
        self.process = subprocess.Popen(args + list(options), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        env=server_env(mtproxy_env))
        for _ in range(100):
            if self.process.poll() is not None:
                raise AssertionError("the server exited: " + self.process.stdout.read().decode(errors="replace"))
            try:
                socket.create_connection(("127.0.0.1", self.http_port), timeout=1).close()
                return self
            except OSError:
                time.sleep(0.1)
        raise AssertionError("the server doesn't listen on the port %d" % self.http_port)

    def stop(self):
        if self.process is None:
            return 0.0
        started = time.time()
        self.process.terminate()
        try:
            self.process.wait(15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.process = None
        return time.time() - started

    def log(self):
        with open(self.log_path, errors="replace") as f:
            return f.read()

    def call(self, token, method, params=None, as_json=False, timeout=5):
        """Returns (HTTP status, response) of the Bot API method"""
        url = "http://127.0.0.1:%d/bot%s/%s" % (self.http_port, token, method)
        headers = {}
        data = None
        if params is not None:
            if as_json:
                data = json.dumps(params).encode()
                headers["Content-Type"] = "application/json"
            else:
                data = urllib.parse.urlencode(params).encode()
        request = urllib.request.Request(url, data=data, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as e:
            return e.code, json.load(e)

    def start_bot(self, token):
        """Makes the server create the bot, which starts connecting to Telegram"""
        def request():
            try:
                self.call(token, "getMe", timeout=3)
            except Exception:
                pass  # there is no Telegram, the request fails
        threading.Thread(target=request, daemon=True).start()


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


def write_registry(work_dir, registry):
    with open(os.path.join(work_dir, "mtproxy.json"), "w") as f:
        json.dump(registry, f)


def read_registry(work_dir):
    with open(os.path.join(work_dir, "mtproxy.json")) as f:
        return json.load(f)


def wait_for(condition, timeout, step=0.2):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if condition():
            return True
        time.sleep(step)
    return condition()
