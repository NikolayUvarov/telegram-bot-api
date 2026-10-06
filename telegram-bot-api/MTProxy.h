//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/common.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"

namespace telegram_bot_api {

// MTProxy server, through which TDLib connects to Telegram
struct MTProxy {
  td::string server_;
  td::int32 port_ = 0;
  td::string secret_;  // base64url for fake TLS secrets, hexadecimal for others

  bool empty() const {
    return server_.empty();
  }

  // identifies the proxy: the same server, port and secret written differently give the same key
  td::string get_key() const;

  // tg://proxy?server=...&port=...&secret=...
  td::string get_link() const;

  // "simple", "dd" or "ee"
  td::Slice get_secret_type() const;

  // the domain imitated by a fake TLS ("ee") proxy, empty for other proxies
  td::string get_domain() const;

  // parses a link tg://proxy?server=...&port=...&secret=..., https://t.me/proxy?... or host:port:secret
  static td::Result<MTProxy> parse(td::Slice link);

  // finds links tg://proxy?... and https://t.me/proxy?... in the text;
  // a text without links and spaces is returned as is, it can be host:port:secret
  static td::vector<td::string> find_links(td::Slice text);
};

// prints the address of the proxy without the secret
td::StringBuilder &operator<<(td::StringBuilder &sb, const MTProxy &proxy);

// options of the MTProxy registry
struct MTProxyOptions {
  td::vector<MTProxy> proxies_;   // --mtproxy
  td::string file_;               // --mtproxy-file
  td::vector<td::int64> admins_;  // --mtproxy-admins: bots allowed to manage the registry
  double check_interval_ = 600;
  double switch_timeout_ = 60;
  double expire_time_ = 72 * 3600;
  td::int32 max_count_ = 200;
};

}  // namespace telegram_bot_api
