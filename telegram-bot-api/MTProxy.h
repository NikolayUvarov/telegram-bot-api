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
  td::string secret_;  // hexadecimal or base64url, as in links

  bool empty() const {
    return server_.empty();
  }

  // parses a link tg://proxy?server=...&port=...&secret=..., https://t.me/proxy?... or host:port:secret
  static td::Result<MTProxy> parse(td::Slice link);
};

// prints the address of the proxy without the secret
td::StringBuilder &operator<<(td::StringBuilder &sb, const MTProxy &proxy);

}  // namespace telegram_bot_api
