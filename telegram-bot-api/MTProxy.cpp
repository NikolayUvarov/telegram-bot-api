//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/MTProxy.h"

#include "td/mtproto/ProxySecret.h"

#include "td/utils/HttpUrl.h"
#include "td/utils/misc.h"
#include "td/utils/SliceBuilder.h"

#include <algorithm>

namespace telegram_bot_api {

td::Result<MTProxy> MTProxy::parse(td::Slice link) {
  link = td::trim(link);
  auto lower_link = td::to_lower(link);

  td::Slice server;
  td::Slice port;
  td::Slice secret;
  td::HttpUrlQuery query;
  auto parse_query = [&](size_t prefix_size) -> td::Status {
    query = td::parse_url_query(link.substr(prefix_size));
    if (query.path_.size() != 1 || td::to_lower(query.path_[0]) != "proxy") {
      return td::Status::Error("only MTProxy links tg://proxy?... and https://t.me/proxy?... are supported");
    }
    server = query.get_arg("server");
    port = query.get_arg("port");
    secret = query.get_arg("secret");
    return td::Status::OK();
  };

  if (td::begins_with(lower_link, "tg:")) {
    TRY_STATUS(parse_query(td::begins_with(lower_link, "tg://") ? 5 : 3));
  } else if (td::begins_with(lower_link, "https://") || td::begins_with(lower_link, "http://")) {
    auto host_begin = lower_link.find("://") + 3;
    auto host_end = lower_link.find('/', host_begin);
    if (host_end == td::string::npos) {
      host_end = lower_link.size();
    }
    auto host = td::Slice(lower_link).substr(host_begin, host_end - host_begin);
    if (td::begins_with(host, "www.")) {
      host.remove_prefix(4);
    }
    if (host != "t.me" && host != "telegram.me" && host != "telegram.dog") {
      return td::Status::Error("only MTProxy links tg://proxy?... and https://t.me/proxy?... are supported");
    }
    TRY_STATUS(parse_query(host_end));
  } else if (lower_link.find("://") != td::string::npos) {
    return td::Status::Error("only MTProxy links tg://proxy?... and https://t.me/proxy?... are supported");
  } else {
    // host:port:secret, the host may be an IPv6 address in brackets
    auto secret_pos = link.rfind(':');
    if (secret_pos == td::Slice::npos) {
      return td::Status::Error("MTProxy must be a link or host:port:secret");
    }
    secret = link.substr(secret_pos + 1);
    auto address = link.substr(0, secret_pos);
    auto port_pos = address.rfind(':');
    if (port_pos == td::Slice::npos) {
      return td::Status::Error("MTProxy must be a link or host:port:secret");
    }
    port = address.substr(port_pos + 1);
    server = address.substr(0, port_pos);
    if (server.size() >= 2 && server[0] == '[' && server.back() == ']') {
      server.remove_prefix(1);
      server.remove_suffix(1);
    }
  }

  if (server.empty()) {
    return td::Status::Error("server of the MTProxy is not specified");
  }
  auto r_port = td::to_integer_safe<td::int32>(port);
  if (r_port.is_error() || r_port.ok() <= 0 || r_port.ok() > 65535) {
    return td::Status::Error("wrong port of the MTProxy");
  }
  // a base64 secret can contain '+', which is decoded from a link as a space
  auto secret_str = secret.str();
  std::replace(secret_str.begin(), secret_str.end(), ' ', '+');
  auto r_secret = td::mtproto::ProxySecret::from_link(secret_str);
  if (r_secret.is_error()) {
    return td::Status::Error(PSLICE() << "wrong secret of the MTProxy: " << r_secret.error().message());
  }

  MTProxy result;
  result.server_ = server.str();
  result.port_ = r_port.ok();
  result.secret_ = r_secret.ok().get_encoded_secret();
  return std::move(result);
}

td::string MTProxy::get_key() const {
  return PSTRING() << td::to_lower(server_) << ':' << port_ << ':' << secret_;
}

td::string MTProxy::get_link() const {
  return PSTRING() << "tg://proxy?server=" << td::url_encode(server_) << "&port=" << port_
                   << "&secret=" << td::url_encode(secret_);
}

td::Slice MTProxy::get_secret_type() const {
  auto r_secret = td::mtproto::ProxySecret::from_link(secret_);
  if (r_secret.is_error()) {
    return td::Slice();
  }
  if (r_secret.ok().emulate_tls()) {
    return td::Slice("ee");
  }
  if (r_secret.ok().use_random_padding()) {
    return td::Slice("dd");
  }
  return td::Slice("simple");
}

td::string MTProxy::get_domain() const {
  auto r_secret = td::mtproto::ProxySecret::from_link(secret_);
  if (r_secret.is_error() || !r_secret.ok().emulate_tls()) {
    return td::string();
  }
  return r_secret.ok().get_domain();
}

td::vector<td::string> MTProxy::find_links(td::Slice text) {
  static const char *const MARKERS[] = {"tg://proxy?", "tg:proxy?", "t.me/proxy?", "telegram.me/proxy?",
                                        "telegram.dog/proxy?"};
  auto is_link_end = [](char c) {
    return static_cast<unsigned char>(c) <= ' ' || static_cast<unsigned char>(c) >= 0x80 || c == '"' || c == '\'' ||
           c == '<' || c == '>' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|' ||
           c == '\\' || c == '^' || c == '`';
  };

  td::vector<td::string> links;
  auto lower_text = td::to_lower(text);
  size_t pos = 0;
  while (true) {
    size_t begin = td::string::npos;
    for (auto marker : MARKERS) {
      begin = td::min(begin, lower_text.find(marker, pos));
    }
    if (begin == td::string::npos) {
      break;
    }

    td::string prefix;
    if (!td::begins_with(td::Slice(lower_text).substr(begin), "tg:")) {
      if (begin >= 4 && td::Slice(lower_text).substr(begin - 4, 4) == "www.") {
        begin -= 4;
      }
      if (begin >= 8 && td::Slice(lower_text).substr(begin - 8, 8) == "https://") {
        begin -= 8;
      } else if (begin >= 7 && td::Slice(lower_text).substr(begin - 7, 7) == "http://") {
        begin -= 7;
      } else {
        prefix = "https://";
      }
    }
    auto end = begin;
    while (end < text.size() && !is_link_end(text[end])) {
      end++;
    }
    pos = end;
    while (end > begin && td::Slice(".,;:!?").find(text[end - 1]) != td::Slice::npos) {
      end--;
    }
    links.push_back(prefix + text.substr(begin, end - begin).str());
  }

  auto trimmed_text = td::trim(text);
  if (links.empty() && !trimmed_text.empty()) {
    bool has_spaces = false;
    for (auto c : trimmed_text) {
      if (static_cast<unsigned char>(c) <= ' ') {
        has_spaces = true;
      }
    }
    if (!has_spaces) {
      links.push_back(trimmed_text.str());
    }
  }
  return links;
}

td::StringBuilder &operator<<(td::StringBuilder &sb, const MTProxy &proxy) {
  if (proxy.server_.find(':') != td::string::npos) {
    return sb << '[' << proxy.server_ << "]:" << proxy.port_;
  }
  return sb << proxy.server_ << ':' << proxy.port_;
}

}  // namespace telegram_bot_api
