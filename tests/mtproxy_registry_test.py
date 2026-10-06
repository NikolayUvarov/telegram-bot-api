#!/usr/bin/env python3
"""Tests of the MTProxy registry: methods, access, persistence, checks and switching.

Fake MTProxy servers accept connections and close them, so every check fails and bots never connect;
Telegram isn't needed. A successful check needs a real proxy and Telegram, it isn't tested here.

    python3 tests/mtproxy_registry_test.py build/telegram-bot-api
"""

import os
import shutil
import sys
import tempfile
import time
import traceback

from common import (DD_SECRET, EE_SECRET, SECRET_KEY, FakeMTProxy, Server, read_registry, token_hash, wait_for,
                    write_registry)

BINARY = sys.argv[1] if len(sys.argv) > 1 else "build/telegram-bot-api"
HTTP_PORT = 18083
ADMIN_ID = 123456
TOKEN = "123456:AAHtest-token-for-mtproxy-tests-0000"
WRONG_TOKEN = "123456:AAHwrong-token-for-mtproxy-test-000"
OTHER_TOKEN = "654321:AAHother-token-for-mtproxy-test-000"
FAST = ["--mtproxy-check-interval=3600", "--mtproxy-switch-timeout=3600"]


def admin_registry():
    return {"admin_token_hashes": [{"bot_id": ADMIN_ID, "hash": token_hash(TOKEN)}]}


def new_server(base_dir, name, registry=None):
    work_dir = os.path.join(base_dir, name)
    os.makedirs(work_dir)
    if registry is not None:
        write_registry(work_dir, registry)
    return Server(BINARY, work_dir, HTTP_PORT)


def get_registry(server):
    status, response = server.call(TOKEN, "getMTProxies")
    assert status == 200, response
    return response["result"]


def by_port(registry, proxy):
    for entry in registry["proxies"]:
        if entry["port"] == proxy.port:
            return entry
    return None


def checks_done(server):
    registry = get_registry(server)
    return not registry["is_checking"] and all(entry["state"] == "failing" for entry in registry["proxies"])


def expect_error(response, status, code, text):
    assert status == code and not response["ok"] and text in response["description"], (status, response)


def test_access(base_dir):
    server = new_server(base_dir, "access").start()
    try:
        status, response = server.call(TOKEN, "getMTProxies")
        expect_error(response, status, 403, "specify --mtproxy-admins")
    finally:
        server.stop()

    server.start("--mtproxy-admins=%d, 777" % ADMIN_ID)
    try:
        status, response = server.call(TOKEN, "getMTProxies")
        expect_error(response, status, 403, "must connect to Telegram once")
        status, response = server.call(OTHER_TOKEN, "getMTProxies")
        expect_error(response, status, 403, "isn't an MTProxy administrator")
    finally:
        server.stop()

    write_registry(server.work_dir, admin_registry())
    server.start("--mtproxy-admins=%d" % ADMIN_ID)
    try:
        status, response = server.call(TOKEN, "getmtproxies")  # methods are case-insensitive
        assert status == 200 and response["result"]["proxies"] == [], response
        status, response = server.call(WRONG_TOKEN, "getMTProxies")
        expect_error(response, status, 403, "token differs")
    finally:
        server.stop()

    try:
        server.start("--mtproxy-admins=abc")
    except AssertionError as e:
        assert "Invalid bot identifier" in str(e), e
    else:
        server.stop()
        raise AssertionError("a wrong --mtproxy-admins is accepted")


def test_methods(base_dir):
    a, b, c, d = FakeMTProxy(), FakeMTProxy(), FakeMTProxy(), FakeMTProxy()
    server = new_server(base_dir, "methods", admin_registry())
    server.start("--mtproxy=" + a.link(), "--mtproxy-admins=%d" % ADMIN_ID, *FAST)
    try:
        # links are found in texts, a JSON array in the body
        text = ("Fresh proxies: tg://proxy?server=127.0.0.1&port=%d&secret=%s, www.t.me/proxy?server=127.0.0.1&port=%d"
                "&secret=%s. Broken: tg://proxy?server=127.0.0.1&port=1&secret=zz" % (b.port, DD_SECRET, c.port,
                                                                                       EE_SECRET))
        status, response = server.call(TOKEN, "addMTProxies", {
            "links": [text, "https://t.me/proxy?server=127.0.0.1&port=%d&secret=%s" % (a.port, DD_SECRET)],
            "source": "channel:-100123/45"}, as_json=True)
        assert status == 200, response
        result = response["result"]
        assert result["added"] == 2 and result["known"] == 1 and len(result["ids"]) == 3, result
        assert len(result["errors"]) == 1 and "secret" in result["errors"][0]["error"], result

        # a single link in a form, without a source
        status, response = server.call(TOKEN, "addMTProxies", {"links": b.link()})
        assert status == 200 and response["result"]["known"] == 1 and response["result"]["added"] == 0, response

        status, response = server.call(TOKEN, "addMTProxies", {"links": "just text"})
        assert status == 200 and response["result"]["added"] == 0 and len(response["result"]["errors"]) == 1, response
        status, response = server.call(TOKEN, "addMTProxies", {"links": ""})
        expect_error(response, status, 400, "links are empty")
        status, response = server.call(TOKEN, "addMTProxies", {"links": a.link(), "source": "option"})
        expect_error(response, status, 400, "wrong source")

        registry = get_registry(server)
        entry_a, entry_b, entry_c = by_port(registry, a), by_port(registry, b), by_port(registry, c)
        assert registry["active_id"] == entry_a["id"] and entry_a["is_active"], registry
        assert entry_a["sources"] == ["option", "channel:-100123/45"], entry_a
        assert entry_b["sources"] == ["channel:-100123/45", "bot:%d" % ADMIN_ID], entry_b
        assert entry_c["secret_type"] == "ee" and entry_c["domain"] == "example.com", entry_c
        assert entry_b["secret_type"] == "dd" and "domain" not in entry_b, entry_b
        assert entry_c["link"].startswith("tg://proxy?server=127.0.0.1&port=%d&secret=" % c.port), entry_c

        # the fake proxies close connections, so all checks fail
        assert wait_for(lambda: checks_done(server), 40), get_registry(server)
        entry_b = by_port(get_registry(server), b)
        assert entry_b["failures_in_row"] >= 1 and entry_b["last_error"] and entry_b["last_check_date"] > 0, entry_b

        # the bot connects through the proxy chosen by setMTProxy
        status, response = server.call(TOKEN, "setMTProxy", {"id": entry_b["id"]})
        assert status == 200 and response["result"] is True, response
        assert get_registry(server)["active_id"] == entry_b["id"]
        for proxy in [a, b, c]:
            proxy.clear()
        server.start_bot(OTHER_TOKEN)
        assert wait_for(lambda: b.count() > 0, 15), "the bot doesn't connect through the chosen proxy"
        time.sleep(3)
        assert a.count() == 0 and c.count() == 0, (a.count(), c.count())
        assert get_registry(server)["bot_count"] == 1

        status, response = server.call(TOKEN, "setMTProxy", {"id": 999})
        expect_error(response, status, 400, "not found")
        status, response = server.call(TOKEN, "setMTProxy", {"id": "abc"})
        expect_error(response, status, 400, "id must be")
        status, response = server.call(TOKEN, "removeMTProxy", {"id": entry_a["id"]})
        expect_error(response, status, 400, "--mtproxy")

        # removal of the active proxy switches the bots to another one
        status, response = server.call(TOKEN, "removeMTProxy", {"id": entry_b["id"]})
        assert status == 200 and response["result"] is True, response
        registry = get_registry(server)
        assert by_port(registry, b) is None and registry["active_id"] in [entry_a["id"], entry_c["id"]], registry
        status, response = server.call(TOKEN, "checkMTProxies")
        assert status == 200 and response["result"] is True, response

        # a proxy added by a bot is kept between restarts
        status, response = server.call(TOKEN, "addMTProxies", {"links": d.link()})
        assert response["result"]["added"] == 1, response
        log = server.log()
        assert SECRET_KEY not in log and EE_SECRET not in log, "the secret is in the log"
    finally:
        stop_time = server.stop()
    assert stop_time < 10, "the server stops for %.1f seconds" % stop_time

    # the registry is kept between restarts
    server.start("--mtproxy=" + b.link(), "--mtproxy-admins=%d" % ADMIN_ID, *FAST)
    try:
        registry = get_registry(server)
        assert by_port(registry, c)["id"] == entry_c["id"] and by_port(registry, d) is not None, registry
        assert by_port(registry, a)["sources"] == ["channel:-100123/45"], registry
        assert by_port(registry, b)["sources"] == ["option"] and by_port(registry, b)["id"] > entry_c["id"], registry
    finally:
        server.stop()
    # a proxy specified only by --mtproxy is forgotten without the option
    server.start("--mtproxy-admins=%d" % ADMIN_ID, *FAST)
    try:
        registry = get_registry(server)
        assert by_port(registry, b) is None and by_port(registry, a) is not None, registry
        assert "option" not in read_registry(server.work_dir)["proxies"][0]["sources"]
    finally:
        server.stop()
        for proxy in [a, b, c, d]:
            proxy.close()


def test_switching(base_dir):
    a, b = FakeMTProxy(), FakeMTProxy()
    server = new_server(base_dir, "switching", admin_registry())
    server.start("--mtproxy=" + a.link(), "--mtproxy=" + b.link(EE_SECRET), "--mtproxy-admins=%d" % ADMIN_ID,
                 "--mtproxy-switch-timeout=2", "--mtproxy-check-interval=3600")
    try:
        registry = get_registry(server)
        id_a, id_b = by_port(registry, a)["id"], by_port(registry, b)["id"]
        assert registry["active_id"] == id_a, registry
        server.start_bot(OTHER_TOKEN)
        # the bot has no connection, nothing passes checks: the proxies are used in turn
        seen = set()
        assert wait_for(lambda: seen.add(get_registry(server)["active_id"]) or seen == {id_a, id_b}, 20), seen
        assert b.count() > 0
        log = server.log()
        assert "a bot has no connection to Telegram" in log and "the next one in turn is used" in log
    finally:
        server.stop()
        a.close()
        b.close()


def test_limits(base_dir):
    a, b, c, d, e = FakeMTProxy(), FakeMTProxy(), FakeMTProxy(), FakeMTProxy(), FakeMTProxy()
    registry = admin_registry()
    # a proxy which hasn't worked for a long time is removed after the check
    registry["proxies"] = [{"id": 5, "server": "127.0.0.1", "port": e.port, "secret": DD_SECRET, "sources": ["bot:1"],
                            "added_date": 1000, "last_success_date": 2000, "failures_in_row": 3}]
    server = new_server(base_dir, "limits", registry)
    server.start("--mtproxy=" + a.link(), "--mtproxy-admins=%d" % ADMIN_ID, "--mtproxy-max=3", *FAST)
    try:
        assert wait_for(lambda: by_port(get_registry(server), e) is None, 30), get_registry(server)
        status, response = server.call(TOKEN, "addMTProxies", {"links": [b.link(), c.link()]}, as_json=True)
        assert response["result"]["added"] == 2, response
        # the registry is full: the proxy, which hasn't worked for the longest time, is removed
        status, response = server.call(TOKEN, "addMTProxies", {"links": d.link()})
        assert response["result"]["added"] == 1, response
        registry = get_registry(server)
        assert len(registry["proxies"]) == 3 and by_port(registry, a) and by_port(registry, d), registry
        assert by_port(registry, a)["id"] == 6, registry  # identifiers aren't reused
    finally:
        server.stop()

    # options don't fit into the registry: nothing can be removed for a new proxy
    server = new_server(base_dir, "limits-full", admin_registry())
    server.start("--mtproxy=" + a.link(), "--mtproxy=" + b.link(), "--mtproxy-admins=%d" % ADMIN_ID,
                 "--mtproxy-max=1", *FAST)
    try:
        assert len(get_registry(server)["proxies"]) == 2
        status, response = server.call(TOKEN, "addMTProxies", {"links": c.link()})
        assert response["result"]["added"] == 0 and "registry is full" in response["result"]["errors"][0]["error"], \
            response
    finally:
        server.stop()
        for proxy in [a, b, c, d, e]:
            proxy.close()


def test_proxy_file(base_dir):
    a, b, c = FakeMTProxy(), FakeMTProxy(), FakeMTProxy()
    server = new_server(base_dir, "file", admin_registry())
    path = os.path.join(server.work_dir, "proxies.txt")
    with open(path, "w") as f:
        f.write("# MTProxy servers\n\n%s\ngarbage line\n127.0.0.1:443:zz\nhttps://t.me/proxy?server=127.0.0.1&port=%d"
                "&secret=%s\n" % (a.link(), b.port, EE_SECRET))
    server.start("--mtproxy-file=" + path, "--mtproxy-admins=%d" % ADMIN_ID, *FAST)
    try:
        registry = get_registry(server)
        assert by_port(registry, a)["sources"] == ["file"] and by_port(registry, b), registry
        assert len(registry["proxies"]) == 2, registry
        log = server.log()
        assert "Wrong MTProxy in the line 4" in log and "Wrong MTProxy in the line 5" in log, log

        # the changed file is reread before checks
        time.sleep(1.1)
        with open(path, "w") as f:
            f.write("%s\n%s\n" % (b.link(EE_SECRET), c.link()))
        status, response = server.call(TOKEN, "checkMTProxies")
        assert status == 200, response
        assert wait_for(lambda: by_port(get_registry(server), c) is not None, 10), get_registry(server)
        registry = get_registry(server)
        assert by_port(registry, a) is None and len(registry["proxies"]) == 2, registry
    finally:
        server.stop()
        for proxy in [a, b, c]:
            proxy.close()


def main():
    failed = []
    base_dir = tempfile.mkdtemp(prefix="mtproxy_registry_test_")
    try:
        for test in [test_access, test_methods, test_switching, test_limits, test_proxy_file]:
            started = time.time()
            try:
                test(base_dir)
                print("ok   %s (%.1f s)" % (test.__name__, time.time() - started))
            except Exception:
                failed.append(test.__name__)
                print("FAIL %s\n%s" % (test.__name__, traceback.format_exc()))
    finally:
        shutil.rmtree(base_dir, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
