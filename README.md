# Telegram Bot API

The Telegram Bot API provides an HTTP API for creating [Telegram Bots](https://core.telegram.org/bots).

If you've got any questions about bots or would like to report an issue with your bot, kindly contact us at [@BotSupport](https://t.me/BotSupport) in Telegram.

Please note that only global Bot API issues that affect all bots are suitable for this repository.

**This fork** can connect to Telegram through [MTProxy](#mtproxy) servers, where Telegram is blocked: it keeps the registry
of them, checks them and switches to a working one. Bots work with the server as with `https://api.telegram.org`, e.g. with
[laser_tele](https://github.com/NikolayUvarov/laser_tele) (`APIURL`) or [laser_tele_rs](https://github.com/NikolayUvarov/laser_tele_rs) (`api_url`).

## Table of Contents
- [Installation](#installation)
- [Dependencies](#dependencies)
- [Usage](#usage)
- [Connecting to Telegram through MTProxy](#mtproxy)
- [Documentation](#documentation)
- [Moving a bot to a local server](#switching)
- [Moving a bot from one local server to another](#moving)
- [License](#license)

<a name="installation"></a>
## Installation

The simplest way to build and install `Telegram Bot API server` is to use our [Telegram Bot API server build instructions generator](https://tdlib.github.io/telegram-bot-api/build.html).
If you do that, you'll only need to choose the target operating system to receive the complete build instructions.

In general, you need to install all `Telegram Bot API server` [dependencies](#dependencies) and compile the source code using CMake:

```sh
git clone --recursive https://github.com/tdlib/telegram-bot-api.git
cd telegram-bot-api
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target install
```

<a name="dependencies"></a>
## Dependencies
To build and run `Telegram Bot API server` you will need:

* OpenSSL
* zlib
* C++17 compatible compiler (e.g., Clang 5.0+, GCC 7.0+, MSVC 19.1+ (Visual Studio 2017.7+), Intel C++ Compiler 19+) (build only)
* gperf (build only)
* CMake (3.10+, build only)

<a name="usage"></a>
## Usage

Use `telegram-bot-api --help` to receive the list of all available options of the Telegram Bot API server.

The only mandatory options are `--api-id` and `--api-hash`. You must obtain your own `api_id` and `api_hash`
as described in https://core.telegram.org/api/obtaining_api_id and specify them using the `--api-id` and `--api-hash` options
or the `TELEGRAM_API_ID` and `TELEGRAM_API_HASH` environment variables.

To enable Bot API features not available at `https://api.telegram.org`, specify the option `--local`. In the local mode the Bot API server allows to:
* Download files without a size limit.
* Upload files up to 2000 MB.
* Upload files using their local path and [the file URI scheme](https://en.wikipedia.org/wiki/File_URI_scheme).
* Use an HTTP URL for the webhook.
* Use any local IP address for the webhook.
* Use any port for the webhook.
* Set *max_webhook_connections* up to 100000.
* Receive the absolute local path as a value of the *file_path* field without the need to download the file after a *getFile* request.

The Telegram Bot API server accepts only HTTP requests, so a TLS termination proxy needs to be used to handle remote HTTPS requests.

By default the Telegram Bot API server is launched on the port 8081, which can be changed using the option `--http-port`.

<a name="mtproxy"></a>
## Connecting to Telegram through MTProxy

The server can connect to Telegram through MTProxy servers. It keeps the registry of them, checks them and switches
all its bots to a working one, when the current one stops working. Bots work with the server as with
`https://api.telegram.org`, see [Moving a bot to a local server](#switching).

### Adding MTProxy servers

Servers are specified by links, as published in channels with proxies, or by the address and the secret.
All kinds of secrets are supported: simple, `dd` and `ee` (fake TLS), in hexadecimal, base64url or base64 encoding.

```sh
telegram-bot-api --api-id=<arg> --api-hash=<arg> \
  --mtproxy='tg://proxy?server=proxy1.example.com&port=443&secret=ee...' \
  --mtproxy='https://t.me/proxy?server=proxy2.example.com&port=443&secret=dd...' \
  --mtproxy=proxy3.example.com:443:ee...
```

* `--mtproxy` can be repeated; the space-separated list in the `TELEGRAM_MTPROXY` environment variable is used
  if the option isn't specified.
* `--mtproxy-file` (or `TELEGRAM_MTPROXY_FILE`) is a file with servers, one per line; lines starting with `#` are comments.
  The file is reread before every check if it has changed.
* Bots from `--mtproxy-admins` (or `TELEGRAM_MTPROXY_ADMINS`), comma-separated identifiers of bots (the number before `:`
  in the token), manage the registry with the methods below, e.g. a bot collecting proxies from channels.

Servers from options and the file can't be removed by bots and disappear from the registry when they disappear from
options and the file. Servers added by bots are kept until they are removed or don't work for `--mtproxy-expire` hours.

### Checks and switching

* Every `--mtproxy-check-interval` seconds (600 by default) all servers are checked: TDLib connects to a Telegram
  datacenter through the server and makes a key exchange with it, so a server, which only accepts connections, fails.
  New servers are checked at once.
* When a bot has no connection to Telegram for `--mtproxy-switch-timeout` seconds (60 by default), all bots are switched
  to the working server with the least ping, checked again right before the switch. If no server passes the check,
  the servers are tried in turn.
* While the current server works, the server isn't changed, even if another one is faster.
* A server, which doesn't work for `--mtproxy-expire` hours (72 by default), is removed. The registry keeps at most
  `--mtproxy-max` servers (200 by default): a new server replaces the one, which hasn't worked for the longest time.
* The registry with the results of checks is kept in `<dir>/mtproxy.json` and survives restarts.
* Every switch is logged with the reason; secrets are never logged.

The servers are used for connections to Telegram only: requests of bots to the server and webhooks don't go through them
(webhooks use `--proxy`). A proxy sees neither the tokens of bots nor their messages, connections to Telegram are
encrypted, but it sees the IP address of the server.

### Methods of the registry

The methods are called like other methods of the Bot API, `https://<server>/bot<token>/<method>`, by bots from
`--mtproxy-admins`. They work even without connection to Telegram. To protect from forged tokens, the server remembers
the token, with which a bot from the list has connected to Telegram, so a bot must connect at least once before using
the methods.

| Method | Parameters | Result |
|---|---|---|
| `addMTProxies` | `links`: array of strings, each is a link or a text with links, e.g. a channel post; `source` (optional): where they come from, e.g. `channel:-1001234567890/45` | `added`, `known`, `ids` of the servers, `errors` |
| `getMTProxies` | | `active_id`, `bot_count`, `connected_bot_count`, `is_checking` and `proxies`: `id`, `server`, `port`, `secret`, `link`, `secret_type`, `domain`, `state` (`unchecked`, `working` or `failing`), `is_active`, `sources`, dates of the last check and the last success, `last_error`, `failures_in_row`, `ping` |
| `setMTProxy` | `id` | switches all bots to the server now |
| `removeMTProxy` | `id` | removes a server added by bots |
| `checkMTProxies` | | starts checks of all servers now |

```sh
curl -d 'links=["Fresh proxies: tg://proxy?server=...&port=443&secret=ee... and https://t.me/proxy?..."]' \
  http://localhost:8081/bot<token>/addMTProxies
curl http://localhost:8081/bot<token>/getMTProxies
```

### Docker

The server can be run in Docker, the image is built from the sources (cloned with `--recursive`):

```sh
docker build -t telegram-bot-api .
docker run -d -p 8081:8081 -v telegram-bot-api:/var/lib/telegram-bot-api \
  -e TELEGRAM_API_ID=<arg> -e TELEGRAM_API_HASH=<arg> -e TELEGRAM_MTPROXY='tg://proxy?server=...' \
  -e TELEGRAM_MTPROXY_ADMINS=<bot identifier> telegram-bot-api
```

Building TDLib takes a lot of memory: with less than 4 GB per core limit the number of parallel jobs,
`docker build --build-arg JOBS=2 ...` or `cmake --build build -j2`.

### Tests

Tests need neither Telegram nor real proxies: fake MTProxy servers record connections of TDLib and refuse them.

```sh
cd tests
python3 mtproxy_test.py ../build/telegram-bot-api
python3 mtproxy_registry_test.py ../build/telegram-bot-api
```

The design of the registry is described in [docs/mtproxy-registry.md](docs/mtproxy-registry.md).

<a name="documentation"></a>
## Documentation
See [Bots: An introduction for developers](https://core.telegram.org/bots) for a brief description of Telegram Bots and their features.

See the [Telegram Bot API documentation](https://core.telegram.org/bots/api) for a description of the Bot API interface and a complete list of available classes, methods and updates.

See the [Telegram Bot API server build instructions generator](https://tdlib.github.io/telegram-bot-api/build.html) for detailed instructions on how to build the Telegram Bot API server.

Subscribe to [@BotNews](https://t.me/botnews) to be the first to know about the latest updates and join the discussion in [@BotTalk](https://t.me/bottalk).

<a name="switching"></a>
## Moving a bot to a local server

To guarantee that your bot will receive all updates, you must deregister it with the `https://api.telegram.org` server by calling the method [logOut](https://core.telegram.org/bots/api#logout).
After the bot is logged out, you can replace the address to which the bot sends requests with the address of your local server and use it in the usual way.
If the server is launched in `--local` mode, make sure that the bot can correctly handle absolute file paths in response to `getFile` requests.

<a name="moving"></a>
## Moving a bot from one local server to another

If the bot is logged in on more than one server simultaneously, there is no guarantee that it will receive all updates.
To move a bot from one local server to another you can use the method [logOut](https://core.telegram.org/bots/api#logout) to log out on the old server before switching to the new one.

If you want to avoid losing updates between logging out on the old server and launching on the new server, you can remove the bot's webhook using the method
[deleteWebhook](https://core.telegram.org/bots/api#deletewebhook), then use the method [close](https://core.telegram.org/bots/api#close) to close the bot instance.
After the instance is closed, locate the bot's subdirectory in the working directory of the old server by the bot's user ID, move the subdirectory to the working directory of the new server
and continue sending requests to the new server as usual.

<a name="license"></a>
## License
`Telegram Bot API server` source code is licensed under the terms of the Boost Software License. See [LICENSE_1_0.txt](http://www.boost.org/LICENSE_1_0.txt) for more information.
