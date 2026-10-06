//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/MTProxyManager.h"

#include "telegram-bot-api/Client.h"
#include "telegram-bot-api/ClientParameters.h"

#include "td/utils/algorithm.h"
#include "td/utils/crypto.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/Clocks.h"
#include "td/utils/port/Stat.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/utf8.h"

#include <algorithm>

namespace telegram_bot_api {

namespace td_api = td::td_api;

static constexpr td::Slice SOURCE_OPTION = "option";
static constexpr td::Slice SOURCE_FILE = "file";

bool MTProxyManager::Entry::is_permanent() const {
  return td::contains(sources_, SOURCE_OPTION.str()) || td::contains(sources_, SOURCE_FILE.str());
}

td::Slice MTProxyManager::Entry::get_state() const {
  if (last_check_date_ == 0 && last_success_date_ == 0) {
    return td::Slice("unchecked");
  }
  return is_working() ? td::Slice("working") : td::Slice("failing");
}

class MTProxyManager::JsonEntry final : public td::Jsonable {
 public:
  JsonEntry(const Entry &entry, bool is_active, bool for_file)
      : entry_(entry), is_active_(is_active), for_file_(for_file) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::JsonLong(entry_.id_));
    object("server", entry_.proxy_.server_);
    object("port", entry_.proxy_.port_);
    object("secret", entry_.proxy_.secret_);
    if (!for_file_) {
      object("link", entry_.proxy_.get_link());
      object("secret_type", entry_.proxy_.get_secret_type());
      auto domain = entry_.proxy_.get_domain();
      if (!domain.empty()) {
        object("domain", domain);
      }
      object("state", entry_.get_state());
      object("is_active", td::JsonBool(is_active_));
    }
    object("sources", td::json_array(entry_.sources_, [](const td::string &source) { return source; }));
    object("added_date", entry_.added_date_);
    object("last_check_date", entry_.last_check_date_);
    object("last_success_date", entry_.last_success_date_);
    if (!entry_.last_error_.empty() || for_file_) {
      object("last_error", entry_.last_error_);
    }
    object("failures_in_row", entry_.failures_in_row_);
    object("ping", td::JsonFloat(entry_.ping_));
  }

 private:
  const Entry &entry_;
  bool is_active_;
  bool for_file_;
};

class MTProxyManager::JsonRegistry final : public td::Jsonable {
 public:
  explicit JsonRegistry(const MTProxyManager *manager) : manager_(manager) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("active_id", td::JsonLong(manager_->active_id_));
    td::int32 connected_bot_count = 0;
    for (auto &it : manager_->clients_) {
      if (it.second.is_ready_) {
        connected_bot_count++;
      }
    }
    object("bot_count", static_cast<td::int32>(manager_->clients_.size()));
    object("connected_bot_count", connected_bot_count);
    object("is_checking", td::JsonBool(manager_->is_check_all_running_));
    object("proxies", td::json_array(manager_->entries_, [manager = manager_](const Entry &entry) {
             return JsonEntry(entry, entry.id_ == manager->active_id_, false);
           }));
  }

 private:
  const MTProxyManager *manager_;
};

class MTProxyManager::JsonRegistryFile final : public td::Jsonable {
 public:
  explicit JsonRegistryFile(const MTProxyManager *manager) : manager_(manager) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("active_id", td::JsonLong(manager_->active_id_));
    object("next_id", td::JsonLong(manager_->next_id_));
    object("admin_token_hashes", td::json_array(manager_->admin_token_hashes_, [](const auto &it) {
             return td::json_object([&it](auto &o) {
               o("bot_id", td::JsonLong(it.first));
               o("hash", it.second);
             });
           }));
    object("proxies",
           td::json_array(manager_->entries_, [](const Entry &entry) { return JsonEntry(entry, false, true); }));
  }

 private:
  const MTProxyManager *manager_;
};

class MTProxyManager::JsonAddResult final : public td::Jsonable {
 public:
  JsonAddResult(td::int32 added, td::int32 known, const td::vector<td::int64> &ids,
                const td::vector<std::pair<td::string, td::string>> &errors)
      : added_(added), known_(known), ids_(ids), errors_(errors) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("added", added_);
    object("known", known_);
    object("ids", td::json_array(ids_, [](td::int64 id) { return td::JsonLong(id); }));
    object("errors", td::json_array(errors_, [](const std::pair<td::string, td::string> &error) {
             return td::json_object([&error](auto &o) {
               o("link", error.first);
               o("error", error.second);
             });
           }));
  }

 private:
  td::int32 added_;
  td::int32 known_;
  const td::vector<td::int64> &ids_;
  const td::vector<std::pair<td::string, td::string>> &errors_;
};

MTProxyManager::MTProxyManager(std::shared_ptr<const ClientParameters> parameters)
    : parameters_(std::move(parameters))
    , options_(parameters_->mtproxy_options_)
    , file_path_(parameters_->working_directory_ + "mtproxy.json") {
}

bool MTProxyManager::is_mtproxy_method(td::Slice method) {
  return method == "addmtproxies" || method == "getmtproxies" || method == "setmtproxy" || method == "removemtproxy" ||
         method == "checkmtproxies";
}

td::int32 MTProxyManager::get_unix_time() {
  return static_cast<td::int32>(td::Clocks::system());
}

td::string MTProxyManager::get_token_hash(td::Slice token) {
  return td::hex_encode(td::sha256(token));
}

void MTProxyManager::start_up() {
  // TDLib used for checks needs its own context, as in Client
  set_context(std::make_shared<td::ActorContext>());

  load();

  // the sources "option" and "file" are taken from the current options
  remove_source(SOURCE_OPTION);
  remove_source(SOURCE_FILE);
  for (auto &proxy : options_.proxies_) {
    bool is_new = false;
    add_proxy(proxy, SOURCE_OPTION, is_new).ignore();
  }
  load_proxy_file(true);
  remove_entries_without_sources();

  if (get_entry(active_id_) == nullptr) {
    active_id_ = 0;
  }
  if (active_id_ == 0 && !entries_.empty()) {
    // the best proxy by the results of previous checks: working ones by ping, then unchecked, then failing
    auto get_rank = [](const Entry &entry) {
      return entry.is_working() ? 0 : (entry.get_state() == "unchecked" ? 1 : 2);
    };
    const Entry *best = &entries_[0];
    for (auto &entry : entries_) {
      auto rank = get_rank(entry);
      auto best_rank = get_rank(*best);
      if (rank < best_rank || (rank == 0 && best_rank == 0 && entry.ping_ < best->ping_)) {
        best = &entry;
      }
    }
    active_id_ = best->id_;
  }
  if (auto *entry = get_entry(active_id_)) {
    LOG(WARNING) << "Connect to Telegram through MTProxy " << entry->proxy_ << " with identifier " << entry->id_;
  }
  last_switch_time_ = td::Time::now();
  save();

  next_check_all_time_ = td::Time::now();
  set_timeout_in(1.0);
}

void MTProxyManager::tear_down() {
  td_client_.reset();
}

void MTProxyManager::timeout_expired() {
  if (!is_check_all_running_ && td::Time::now() >= next_check_all_time_) {
    check_all();
  }
  update_active();
  set_timeout_in(1.0);
}

MTProxyManager::Entry *MTProxyManager::get_entry(td::int64 id) {
  for (auto &entry : entries_) {
    if (entry.id_ == id) {
      return &entry;
    }
  }
  return nullptr;
}

const MTProxyManager::Entry *MTProxyManager::get_entry(td::int64 id) const {
  for (auto &entry : entries_) {
    if (entry.id_ == id) {
      return &entry;
    }
  }
  return nullptr;
}

td::Result<td::int64> MTProxyManager::add_proxy(const MTProxy &proxy, td::Slice source, bool &is_new) {
  is_new = false;
  auto key = proxy.get_key();
  for (auto &entry : entries_) {
    if (entry.proxy_.get_key() == key) {
      if (!td::contains(entry.sources_, source.str())) {
        entry.sources_.push_back(source.str());
      }
      return entry.id_;
    }
  }

  bool is_permanent = source == SOURCE_OPTION || source == SOURCE_FILE;
  if (!is_permanent && static_cast<td::int32>(entries_.size()) >= options_.max_count_ && !evict_entry()) {
    return td::Status::Error("the registry is full: remove MTProxy servers or increase --mtproxy-max");
  }

  Entry entry;
  entry.id_ = next_id_++;
  entry.proxy_ = proxy;
  entry.sources_.push_back(source.str());
  entry.added_date_ = get_unix_time();
  entries_.push_back(std::move(entry));
  is_new = true;
  return entries_.back().id_;
}

bool MTProxyManager::evict_entry() {
  // the proxy, which hasn't worked for the longest time
  const Entry *victim = nullptr;
  for (auto &entry : entries_) {
    if (entry.is_permanent() || entry.id_ == active_id_ || entry.is_working()) {
      continue;
    }
    auto date = td::max(entry.last_success_date_, entry.added_date_);
    if (victim == nullptr || date < td::max(victim->last_success_date_, victim->added_date_)) {
      victim = &entry;
    }
  }
  if (victim == nullptr) {
    return false;
  }
  LOG(INFO) << "Remove MTProxy " << victim->proxy_ << ": the registry is full";
  remove_entry(victim->id_);
  return true;
}

void MTProxyManager::remove_entry(td::int64 id) {
  td::remove_if(entries_, [id](const Entry &entry) { return entry.id_ == id; });
  td::remove(switch_candidates_, id);
  if (active_id_ == id) {
    active_id_ = 0;
  }
}

void MTProxyManager::remove_source(td::Slice source) {
  for (auto &entry : entries_) {
    td::remove(entry.sources_, source.str());
  }
}

void MTProxyManager::remove_entries_without_sources() {
  td::vector<td::int64> ids;
  for (auto &entry : entries_) {
    if (entry.sources_.empty()) {
      ids.push_back(entry.id_);
    }
  }
  for (auto id : ids) {
    remove_entry(id);
  }
}

void MTProxyManager::load_proxy_file(bool force) {
  if (options_.file_.empty()) {
    return;
  }
  auto r_stat = td::stat(options_.file_);
  if (r_stat.is_error()) {
    if (!is_proxy_file_missing_logged_) {
      LOG(ERROR) << "Can't read --mtproxy-file " << options_.file_ << ": " << r_stat.error();
      is_proxy_file_missing_logged_ = true;
    }
    return;
  }
  is_proxy_file_missing_logged_ = false;
  if (!force && r_stat.ok().mtime_nsec_ == proxy_file_mtime_) {
    return;
  }
  auto r_content = td::read_file_str(options_.file_);
  if (r_content.is_error()) {
    LOG(ERROR) << "Can't read --mtproxy-file " << options_.file_ << ": " << r_content.error();
    return;
  }
  proxy_file_mtime_ = r_stat.ok().mtime_nsec_;

  remove_source(SOURCE_FILE);
  size_t line_number = 0;
  for (auto line : td::full_split(td::Slice(r_content.ok()), '\n')) {
    line_number++;
    line = td::trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto links = MTProxy::find_links(line);
    if (links.empty()) {
      LOG(ERROR) << "Wrong MTProxy in the line " << line_number << " of " << options_.file_
                 << ": there are no MTProxy links";
    }
    for (auto &link : links) {
      auto r_proxy = MTProxy::parse(link);
      if (r_proxy.is_error()) {
        LOG(ERROR) << "Wrong MTProxy in the line " << line_number << " of " << options_.file_ << ": "
                   << r_proxy.error().message();
        continue;
      }
      bool is_new = false;
      add_proxy(r_proxy.ok(), SOURCE_FILE, is_new).ignore();
      if (is_new) {
        check_queue_.push_back(entries_.back().id_);
      }
    }
  }
  remove_entries_without_sources();
  LOG(INFO) << "Load MTProxy servers from " << options_.file_;
}

void MTProxyManager::expire_entries() {
  auto min_date = get_unix_time() - static_cast<td::int32>(options_.expire_time_);
  td::vector<td::int64> ids;
  for (auto &entry : entries_) {
    if (!entry.is_permanent() && entry.id_ != active_id_ && !entry.is_working() &&
        td::max(entry.last_success_date_, entry.added_date_) < min_date) {
      ids.push_back(entry.id_);
    }
  }
  for (auto id : ids) {
    LOG(INFO) << "Remove MTProxy " << get_entry(id)->proxy_ << ": it doesn't work for a long time";
    remove_entry(id);
  }
}

void MTProxyManager::load() {
  auto r_content = td::read_file_str(file_path_);
  if (r_content.is_error()) {
    return;  // there is no registry yet
  }
  auto content = r_content.move_as_ok();
  auto r_value = td::json_decode(content);
  if (r_value.is_error() || r_value.ok().type() != td::JsonValue::Type::Object) {
    LOG(ERROR) << "Can't parse " << file_path_ << ", the MTProxy registry is empty";
    return;
  }

  // wrong values are replaced with defaults
  auto get_long = [](const td::JsonObject &object, td::Slice name) {
    auto r_value = object.get_optional_long_field(name);
    return r_value.is_ok() ? r_value.ok() : 0;
  };
  auto get_int = [](const td::JsonObject &object, td::Slice name) {
    auto r_value = object.get_optional_int_field(name);
    return r_value.is_ok() ? r_value.ok() : 0;
  };
  auto get_double = [](const td::JsonObject &object, td::Slice name) {
    auto r_value = object.get_optional_double_field(name);
    return r_value.is_ok() ? r_value.ok() : 0.0;
  };
  auto get_string = [](const td::JsonObject &object, td::Slice name) {
    auto r_value = object.get_optional_string_field(name);
    return r_value.is_ok() ? r_value.move_as_ok() : td::string();
  };

  auto &object = r_value.ok_ref().get_object();
  active_id_ = get_long(object, "active_id");
  next_id_ = td::max(get_long(object, "next_id"), static_cast<td::int64>(1));

  auto hashes = object.extract_field("admin_token_hashes");
  if (hashes.type() == td::JsonValue::Type::Array) {
    for (auto &value : hashes.get_array()) {
      if (value.type() != td::JsonValue::Type::Object) {
        continue;
      }
      auto &hash_object = value.get_object();
      auto bot_id = get_long(hash_object, "bot_id");
      auto hash = get_string(hash_object, "hash");
      if (bot_id > 0 && !hash.empty()) {
        admin_token_hashes_[bot_id] = std::move(hash);
      }
    }
  }

  auto proxies = object.extract_field("proxies");
  if (proxies.type() != td::JsonValue::Type::Array) {
    return;
  }
  for (auto &value : proxies.get_array()) {
    if (value.type() != td::JsonValue::Type::Object) {
      continue;
    }
    auto &proxy_object = value.get_object();
    auto server = get_string(proxy_object, "server");
    auto port = get_int(proxy_object, "port");
    auto r_proxy = MTProxy::parse(PSLICE() << server << ':' << port << ':' << get_string(proxy_object, "secret"));
    if (r_proxy.is_error()) {
      LOG(ERROR) << "Skip wrong MTProxy " << server << ':' << port << " in " << file_path_;
      continue;
    }
    Entry entry;
    entry.id_ = get_long(proxy_object, "id");
    if (entry.id_ <= 0 || get_entry(entry.id_) != nullptr) {
      entry.id_ = next_id_;
    }
    next_id_ = td::max(next_id_, entry.id_ + 1);
    entry.proxy_ = r_proxy.move_as_ok();
    auto sources = proxy_object.extract_field("sources");
    if (sources.type() == td::JsonValue::Type::Array) {
      for (auto &source : sources.get_array()) {
        if (source.type() == td::JsonValue::Type::String) {
          entry.sources_.push_back(source.get_string().str());
        }
      }
    }
    entry.added_date_ = get_int(proxy_object, "added_date");
    entry.last_check_date_ = get_int(proxy_object, "last_check_date");
    entry.last_success_date_ = get_int(proxy_object, "last_success_date");
    entry.last_error_ = get_string(proxy_object, "last_error");
    entry.failures_in_row_ = get_int(proxy_object, "failures_in_row");
    entry.ping_ = get_double(proxy_object, "ping");
    entries_.push_back(std::move(entry));
  }
  std::sort(entries_.begin(), entries_.end(), [](const Entry &lhs, const Entry &rhs) { return lhs.id_ < rhs.id_; });
}

void MTProxyManager::save() const {
  if (entries_.empty() && admin_token_hashes_.empty() && next_id_ == 1 && td::stat(file_path_).is_error()) {
    return;  // the registry isn't used
  }
  auto content = td::json_encode<td::string>(JsonRegistryFile(this), true);
  auto status = td::atomic_write_file(file_path_, content);
  if (status.is_error()) {
    LOG(ERROR) << "Can't save the MTProxy registry to " << file_path_ << ": " << status;
  }
}

MTProxy MTProxyManager::get_active_proxy() const {
  auto *entry = get_entry(active_id_);
  return entry == nullptr ? MTProxy() : entry->proxy_;
}

void MTProxyManager::add_client(td::uint64 client_id, td::ActorId<Client> client) {
  auto &state = clients_[client_id];
  state.actor_ = client;
  state.is_ready_ = false;
  state.not_ready_since_ = td::Time::now();
  send_closure(client, &Client::set_mtproxy, get_active_proxy());
}

void MTProxyManager::remove_client(td::uint64 client_id) {
  clients_.erase(client_id);
}

void MTProxyManager::on_client_connection_state(td::uint64 client_id, bool is_ready) {
  auto it = clients_.find(client_id);
  if (it == clients_.end()) {
    return;
  }
  auto &state = it->second;
  if (state.is_ready_ == is_ready) {
    return;
  }
  state.is_ready_ = is_ready;
  if (!is_ready) {
    state.not_ready_since_ = td::Time::now();
    return;
  }

  // the bot has connected through the active proxy, so it works
  auto *entry = get_entry(active_id_);
  if (entry != nullptr && !entry->is_working()) {
    entry->failures_in_row_ = 0;
    entry->last_success_date_ = get_unix_time();
    entry->last_error_.clear();
    save();
  }
}

void MTProxyManager::on_client_authorized(td::int64 bot_user_id, td::string token) {
  if (!td::contains(options_.admins_, bot_user_id)) {
    return;
  }
  auto hash = get_token_hash(token);
  auto &saved_hash = admin_token_hashes_[bot_user_id];
  if (saved_hash != hash) {
    saved_hash = std::move(hash);
    LOG(INFO) << "Remember the token of the MTProxy administrator " << bot_user_id;
    save();
  }
}

void MTProxyManager::ensure_td_client() {
  if (!td_client_.empty()) {
    return;
  }

  class TdCallback final : public td::TdCallback {
   public:
    explicit TdCallback(td::ActorId<MTProxyManager> manager) : manager_(std::move(manager)) {
    }
    void on_result(td::uint64 id, td_api::object_ptr<td_api::Object> result) final {
      send_closure_later(manager_, &MTProxyManager::on_td_result, id, std::move(result));
    }
    void on_error(td::uint64 id, td_api::object_ptr<td_api::error> result) final {
      send_closure_later(manager_, &MTProxyManager::on_td_result, id, std::move(result));
    }

   private:
    td::ActorId<MTProxyManager> manager_;
  };
  // the client is never initialized: testProxy works before setTdlibParameters
  td::ClientActor::Options options;
  options.net_query_stats = parameters_->net_query_stats_;
  td_client_ = td::create_actor_on_scheduler<td::ClientActor>(
      "MTProxyCheckerActor", 0, td::make_unique<TdCallback>(actor_id(this)), std::move(options));
}

void MTProxyManager::check_all() {
  load_proxy_file(false);
  is_check_all_running_ = true;
  for (auto &entry : entries_) {
    if (!entry.is_checking_ && !td::contains(check_queue_, entry.id_)) {
      check_queue_.push_back(entry.id_);
    }
  }
  run_checks();
}

void MTProxyManager::run_checks() {
  while (running_check_count_ < MAX_PARALLEL_CHECKS && !check_queue_.empty()) {
    auto id = check_queue_.front();
    check_queue_.erase(check_queue_.begin());
    auto *entry = get_entry(id);
    if (entry == nullptr || entry->is_checking_) {
      continue;
    }
    running_check_count_++;
    send_check(id, CheckPurpose::Periodic);
  }
  if (running_check_count_ == 0 && check_queue_.empty()) {
    if (is_check_all_running_) {
      is_check_all_running_ = false;
      next_check_all_time_ = td::Time::now() + options_.check_interval_;
      expire_entries();
    }
    save();
  }
}

void MTProxyManager::send_check(td::int64 id, CheckPurpose purpose) {
  ensure_td_client();
  auto *entry = get_entry(id);
  CHECK(entry != nullptr);
  entry->is_checking_ = true;

  auto request_id = ++last_request_id_;
  PendingCheck check;
  check.entry_id_ = id;
  check.start_time_ = td::Time::now();
  check.purpose_ = purpose;
  check.switch_generation_ = switch_generation_;
  pending_checks_[request_id] = check;

  auto &proxy = entry->proxy_;
  send_closure(td_client_, &td::ClientActor::request, request_id,
               td_api::make_object<td_api::testProxy>(
                   td_api::make_object<td_api::proxy>(proxy.server_, proxy.port_,
                                                      td_api::make_object<td_api::proxyTypeMtproto>(proxy.secret_)),
                   CHECK_DC_ID, CHECK_TIMEOUT));
}

void MTProxyManager::on_td_result(td::uint64 request_id, td_api::object_ptr<td_api::Object> result) {
  auto it = pending_checks_.find(request_id);
  if (it == pending_checks_.end()) {
    return;  // updates of TDLib
  }
  auto check = it->second;
  pending_checks_.erase(it);
  on_check_result(check, std::move(result));
}

void MTProxyManager::on_check_result(PendingCheck check, td_api::object_ptr<td_api::Object> result) {
  bool is_ok = result->get_id() != td_api::error::ID;
  auto *entry = get_entry(check.entry_id_);
  if (entry != nullptr) {
    entry->is_checking_ = false;
    entry->last_check_date_ = get_unix_time();
    if (is_ok) {
      auto ping = td::Time::now() - check.start_time_;
      entry->ping_ = entry->ping_ <= 0.0 ? ping : 0.7 * entry->ping_ + 0.3 * ping;
      entry->failures_in_row_ = 0;
      entry->last_success_date_ = entry->last_check_date_;
      entry->last_error_.clear();
    } else {
      entry->failures_in_row_++;
      entry->last_error_ = static_cast<const td_api::error *>(result.get())->message_;
    }
    LOG(INFO) << "MTProxy " << entry->proxy_ << " check: " << (is_ok ? td::Slice("OK") : entry->last_error_);
  }

  if (check.purpose_ == CheckPurpose::Periodic) {
    CHECK(running_check_count_ > 0);
    running_check_count_--;
    return run_checks();
  }

  if (check.switch_generation_ != switch_generation_ || !is_switching_) {
    return;  // the switch was cancelled
  }
  if (entry != nullptr && is_ok) {
    is_switching_ = false;
    return set_active(entry->id_, switch_reason_);
  }
  try_next_candidate();
}

void MTProxyManager::update_active() {
  if (is_switching_) {
    return;
  }
  if (entries_.empty()) {
    if (active_id_ != 0) {
      set_active(0, "the MTProxy registry is empty");
    }
    return;
  }
  auto *active = get_entry(active_id_);
  if (active == nullptr) {
    return start_switch("there is no active MTProxy");
  }

  auto now = td::Time::now();
  bool has_ready_client = false;
  for (auto &it : clients_) {
    auto &state = it.second;
    if (state.is_ready_) {
      has_ready_client = true;
      continue;
    }
    auto not_ready_time = now - td::max(state.not_ready_since_, last_switch_time_);
    if (not_ready_time > options_.switch_timeout_) {
      return start_switch(PSTRING() << "a bot has no connection to Telegram for "
                                    << static_cast<td::int32>(not_ready_time) << " seconds");
    }
  }
  if (!has_ready_client && active->failures_in_row_ >= MAX_FAILED_CHECKS_OF_ACTIVE &&
      now - last_switch_time_ > options_.switch_timeout_) {
    return start_switch(PSTRING() << "the active MTProxy failed " << active->failures_in_row_
                                  << " checks in a row: " << active->last_error_);
  }
}

void MTProxyManager::start_switch(td::string reason) {
  auto now = td::Time::now();
  if (auto *active = get_entry(active_id_)) {
    active->failed_as_active_time_ = now;
  }

  switch_candidates_.clear();
  for (auto &entry : entries_) {
    if (entry.id_ != active_id_ && entry.is_working() && entry.failed_as_active_time_ + FAILED_ACTIVE_COOLDOWN < now) {
      switch_candidates_.push_back(entry.id_);
    }
  }
  std::stable_sort(switch_candidates_.begin(), switch_candidates_.end(),
                   [this](td::int64 lhs, td::int64 rhs) { return get_entry(lhs)->ping_ < get_entry(rhs)->ping_; });

  is_switching_ = true;
  switch_generation_++;
  switch_reason_ = std::move(reason);
  try_next_candidate();
}

void MTProxyManager::try_next_candidate() {
  while (!switch_candidates_.empty()) {
    auto id = switch_candidates_.front();
    switch_candidates_.erase(switch_candidates_.begin());
    if (get_entry(id) == nullptr) {
      continue;
    }
    // the candidate is checked right before the switch, even if a periodic check of it is running
    return send_check(id, CheckPurpose::BeforeSwitch);
  }

  // no MTProxy passed the check: try the next one in turn, states of the proxies can be outdated
  is_switching_ = false;
  if (!is_check_all_running_) {
    check_all();
  }
  auto id = get_next_in_turn();
  if (id == 0 || id == active_id_) {
    LOG(INFO) << "There is no other MTProxy: " << switch_reason_;
    last_switch_time_ = td::Time::now();
    return;
  }
  set_active(id, PSLICE() << switch_reason_ << "; no MTProxy passed the check, so the next one in turn is used");
}

td::int64 MTProxyManager::get_next_in_turn() const {
  if (entries_.empty()) {
    return 0;
  }
  for (auto &entry : entries_) {
    if (entry.id_ > active_id_) {
      return entry.id_;
    }
  }
  return entries_[0].id_;
}

void MTProxyManager::set_active(td::int64 id, td::Slice reason) {
  active_id_ = id;
  last_switch_time_ = td::Time::now();
  auto proxy = get_active_proxy();
  if (proxy.empty()) {
    LOG(WARNING) << "Connect to Telegram without MTProxy: " << reason;
  } else {
    LOG(WARNING) << "Connect to Telegram through MTProxy " << proxy << " with identifier " << id << ": " << reason;
  }
  for (auto &it : clients_) {
    send_closure(it.second.actor_, &Client::set_mtproxy, proxy);
  }
  save();
}

td::Status MTProxyManager::check_access(td::int64 bot_user_id, td::Slice token) const {
  if (options_.admins_.empty()) {
    return td::Status::Error(403, "Forbidden: MTProxy methods are disabled, specify --mtproxy-admins");
  }
  if (!td::contains(options_.admins_, bot_user_id)) {
    return td::Status::Error(403, "Forbidden: the bot isn't an MTProxy administrator");
  }
  auto it = admin_token_hashes_.find(bot_user_id);
  if (it == admin_token_hashes_.end()) {
    return td::Status::Error(403, "Forbidden: the bot must connect to Telegram once before managing MTProxy");
  }
  if (it->second != get_token_hash(token)) {
    return td::Status::Error(403, "Forbidden: the token differs from the last token used to connect to Telegram");
  }
  return td::Status::OK();
}

void MTProxyManager::send(td::int64 bot_user_id, PromisedQueryPtr query) {
  auto status = check_access(bot_user_id, query->token());
  if (status.is_error()) {
    return fail_query(status.code(), status.message(), std::move(query));
  }

  auto method = query->method();
  if (method == "addmtproxies") {
    return process_add(bot_user_id, std::move(query));
  }
  if (method == "getmtproxies") {
    return answer_query(JsonRegistry(this), std::move(query));
  }
  if (method == "setmtproxy") {
    return process_set(std::move(query));
  }
  if (method == "removemtproxy") {
    return process_remove(std::move(query));
  }
  CHECK(method == "checkmtproxies");
  if (!is_check_all_running_) {
    check_all();
  }
  answer_query(td::JsonTrue(), std::move(query));
}

td::Result<td::vector<td::string>> MTProxyManager::get_texts(td::Slice links) {
  td::vector<td::string> texts;
  if (!td::begins_with(td::trim(links), "[")) {
    if (!td::trim(links).empty()) {
      texts.push_back(links.str());
    }
    return std::move(texts);
  }
  auto json = links.str();
  auto r_value = td::json_decode(json);
  if (r_value.is_error()) {
    return td::Status::Error(400, PSLICE()
                                      << "Bad Request: can't parse links JSON array: " << r_value.error().message());
  }
  if (r_value.ok().type() != td::JsonValue::Type::Array) {
    return td::Status::Error(400, "Bad Request: links must be an array of strings");
  }
  for (auto &value : r_value.ok().get_array()) {
    if (value.type() != td::JsonValue::Type::String) {
      return td::Status::Error(400, "Bad Request: links must be an array of strings");
    }
    texts.push_back(value.get_string().str());
  }
  return std::move(texts);
}

td::Result<td::int64> MTProxyManager::get_id_arg(const Query *query) {
  auto r_id = td::to_integer_safe<td::int64>(query->arg("id"));
  if (r_id.is_error()) {
    return td::Status::Error(400, "Bad Request: id must be an identifier of an MTProxy from getMTProxies");
  }
  return r_id.ok();
}

void MTProxyManager::process_add(td::int64 bot_user_id, PromisedQueryPtr query) {
  auto r_texts = get_texts(query->arg("links"));
  if (r_texts.is_error()) {
    return fail_query(r_texts.error().code(), r_texts.error().message(), std::move(query));
  }
  if (r_texts.ok().empty()) {
    return fail_query(400, "Bad Request: links are empty", std::move(query));
  }
  td::string source = td::trim(query->arg("source")).str();
  if (source.empty()) {
    source = PSTRING() << "bot:" << bot_user_id;
  }
  if (source.size() > MAX_SOURCE_LENGTH || source == SOURCE_OPTION || source == SOURCE_FILE) {
    return fail_query(400, "Bad Request: wrong source", std::move(query));
  }

  td::int32 added = 0;
  td::int32 known = 0;
  td::vector<td::int64> ids;
  td::vector<std::pair<td::string, td::string>> errors;
  for (auto &text : r_texts.ok()) {
    auto links = MTProxy::find_links(text);
    if (links.empty()) {
      errors.emplace_back(td::utf8_truncate(text, 100), "there are no MTProxy links");
    }
    for (auto &link : links) {
      auto r_proxy = MTProxy::parse(link);
      if (r_proxy.is_error()) {
        errors.emplace_back(link, r_proxy.error().message().str());
        continue;
      }
      bool is_new = false;
      auto r_id = add_proxy(r_proxy.ok(), source, is_new);
      if (r_id.is_error()) {
        errors.emplace_back(link, r_id.error().message().str());
        continue;
      }
      if (td::contains(ids, r_id.ok())) {
        continue;
      }
      ids.push_back(r_id.ok());
      if (is_new) {
        added++;
        check_queue_.push_back(r_id.ok());
        LOG(INFO) << "Add MTProxy " << r_proxy.ok() << " from " << source;
      } else {
        known++;
      }
    }
  }
  run_checks();
  save();
  answer_query(JsonAddResult(added, known, ids, errors), std::move(query));
}

void MTProxyManager::process_set(PromisedQueryPtr query) {
  auto r_id = get_id_arg(query.get());
  if (r_id.is_error()) {
    return fail_query(r_id.error().code(), r_id.error().message(), std::move(query));
  }
  if (get_entry(r_id.ok()) == nullptr) {
    return fail_query(400, "Bad Request: MTProxy not found", std::move(query));
  }
  is_switching_ = false;
  switch_generation_++;
  set_active(r_id.ok(), "setMTProxy");
  answer_query(td::JsonTrue(), std::move(query));
}

void MTProxyManager::process_remove(PromisedQueryPtr query) {
  auto r_id = get_id_arg(query.get());
  if (r_id.is_error()) {
    return fail_query(r_id.error().code(), r_id.error().message(), std::move(query));
  }
  auto *entry = get_entry(r_id.ok());
  if (entry == nullptr) {
    return fail_query(400, "Bad Request: MTProxy not found", std::move(query));
  }
  if (entry->is_permanent()) {
    return fail_query(400, "Bad Request: the MTProxy is specified by --mtproxy or --mtproxy-file", std::move(query));
  }
  LOG(INFO) << "Remove MTProxy " << entry->proxy_ << " by removeMTProxy";
  remove_entry(r_id.ok());
  save();
  answer_query(td::JsonTrue(), std::move(query));
  update_active();
}

}  // namespace telegram_bot_api
