#!/usr/bin/env python3
"""Tests of the option --mtproxy.

A fake MTProxy accepts connections of TDLib and records their first bytes, so the test checks that bots
connect to Telegram through the proxy. Telegram itself isn't needed.

    python3 tests/mtproxy_test.py build/telegram-bot-api
"""

import base64
import os
import shutil
import subprocess
import sys
import tempfile
import time

from common import DD_SECRET, DOMAIN, EE_SECRET, SECRET_KEY, FakeMTProxy, Server, server_env, wait_for

BINARY = sys.argv[1] if len(sys.argv) > 1 else "build/telegram-bot-api"
HTTP_PORT = 18081
TOKEN = "123456:AAHtest-token-for-mtproxy-tests-0000"


def base64_secret_with_plus():
    """Fake TLS secret in base64 with '+', which is decoded from a link as a space"""
    for i in range(256):
        raw = bytes.fromhex("ee" + SECRET_KEY[:-2] + "%02x" % i) + DOMAIN
        encoded = base64.b64encode(raw).decode()
        if "+" in encoded:
            return encoded
    raise AssertionError("no base64 secret with '+'")


def start_error(*options, mtproxy_env=None):
    """Runs the server, which must exit at once, and returns its error"""
    result = subprocess.run([BINARY, "--api-id=1", "--api-hash=x"] + list(options), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=10, env=server_env(mtproxy_env))
    prefix = BINARY + ": "
    errors = [line[len(prefix):] for line in result.stdout.decode(errors="replace").splitlines()
              if line.startswith(prefix)]
    return result.returncode, errors[0] if errors else ""


def check_connection(server, proxy, link, check, description, use_env=False):
    proxy.clear()
    link = link.replace("PORT", str(proxy.port))
    if use_env:
        server.start(mtproxy_env=link)
    else:
        server.start("--mtproxy=" + link)
    try:
        server.start_bot(TOKEN)
        # the check of the proxy connects once, the bot reconnects again and again
        assert wait_for(lambda: proxy.count() >= 3, 30), description + ": TDLib didn't connect to the MTProxy"
        with proxy.lock:
            connections = list(proxy.connections)
        for _, data in connections:
            assert check(data), description + ": wrong first bytes " + data[:80].hex()
    finally:
        server.stop()
    log = server.log()
    assert "Connect to Telegram through MTProxy 127.0.0.1:%d" % proxy.port in log, description + ": no proxy in the log"
    assert SECRET_KEY not in log and EE_SECRET not in log, description + ": the secret is in the log"
    print("ok  ", description)


def main():
    failed = False
    work_dir = tempfile.mkdtemp(prefix="mtproxy_test_")
    server = Server(BINARY, work_dir, HTTP_PORT)
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
            check_connection(server, proxy, "tg://proxy?server=127.0.0.1&port=PORT&secret=" + EE_SECRET, is_fake_tls,
                             "tg:// link with ee secret")
            check_connection(server, proxy, "https://t.me/proxy?server=127.0.0.1&port=PORT&secret=" + EE_SECRET,
                             is_fake_tls, "https://t.me link with ee secret")
            check_connection(server, proxy, "tg://proxy?server=127.0.0.1&port=PORT&secret=" + base64_secret_with_plus(),
                             is_fake_tls, "tg:// link with base64 secret with '+'")
            # obfuscated connection: 64 bytes of the header
            check_connection(server, proxy, "127.0.0.1:PORT:" + DD_SECRET, lambda data: len(data) >= 64,
                             "host:port:secret with dd secret")
            check_connection(server, proxy, "127.0.0.1:PORT:" + EE_SECRET, is_fake_tls,
                             "TELEGRAM_MTPROXY environment variable", use_env=True)

            # without --mtproxy the proxy saved in the database of the bot is disabled
            proxy.clear()
            server.start()
            try:
                server.start_bot(TOKEN)
                time.sleep(10)
                if proxy.count() != 0:
                    print("FAIL the proxy is used without --mtproxy")
                    failed = True
                else:
                    print("ok   the proxy is disabled without --mtproxy")
            finally:
                server.stop()
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
