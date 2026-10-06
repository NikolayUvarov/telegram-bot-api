//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/MTProxy.h"
#include "telegram-bot-api/Query.h"

#include "td/telegram/ClientActor.h"
#include "td/telegram/td_api.h"

#include "td/actor/actor.h"

#include "td/utils/common.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

#include <map>
#include <memory>

namespace telegram_bot_api {

class Client;
struct ClientParameters;

// The registry of MTProxy servers: keeps them in <dir>/mtproxy.json, checks them and switches all bots of the server
// to a working one. Bots from --mtproxy-admins manage it with methods addMTProxies, getMTProxies, setMTProxy,
// removeMTProxy and checkMTProxies
class MTProxyManager final : public td::Actor {
 public:
  explicit MTProxyManager(std::shared_ptr<const ClientParameters> parameters);

  // the method is lowercased, as in Query
  static bool is_mtproxy_method(td::Slice method);

  void send(td::int64 bot_user_id, PromisedQueryPtr query);

  void add_client(td::uint64 client_id, td::ActorId<Client> client);

  void remove_client(td::uint64 client_id);

  void on_client_connection_state(td::uint64 client_id, bool is_ready);

  void on_client_authorized(td::int64 bot_user_id, td::string token);

  void on_td_result(td::uint64 request_id, td::td_api::object_ptr<td::td_api::Object> result);

 private:
  static constexpr double CHECK_TIMEOUT = 10.0;
  static constexpr td::int32 CHECK_DC_ID = 2;
  static constexpr size_t MAX_PARALLEL_CHECKS = 8;
  static constexpr td::int32 MAX_FAILED_CHECKS_OF_ACTIVE = 2;
  static constexpr double FAILED_ACTIVE_COOLDOWN = 600.0;
  static constexpr size_t MAX_SOURCE_LENGTH = 128;

  struct Entry {
    td::int64 id_ = 0;
    MTProxy proxy_;
    td::vector<td::string> sources_;
    td::int32 added_date_ = 0;
    td::int32 last_check_date_ = 0;
    td::int32 last_success_date_ = 0;
    td::string last_error_;
    td::int32 failures_in_row_ = 0;
    double ping_ = 0.0;

    // not saved
    double failed_as_active_time_ = -1e9;
    bool is_checking_ = false;

    bool is_working() const {
      return last_success_date_ != 0 && failures_in_row_ == 0;
    }

    // specified by --mtproxy or --mtproxy-file
    bool is_permanent() const;

    td::Slice get_state() const;
  };

  struct ClientState {
    td::ActorId<Client> actor_;
    bool is_ready_ = false;
    double not_ready_since_ = 0.0;
  };

  enum class CheckPurpose : td::int32 { Periodic, BeforeSwitch };

  struct PendingCheck {
    td::int64 entry_id_ = 0;
    double start_time_ = 0.0;
    CheckPurpose purpose_ = CheckPurpose::Periodic;
    td::uint64 switch_generation_ = 0;
  };

  class JsonEntry;
  class JsonRegistry;
  class JsonRegistryFile;
  class JsonAddResult;

  std::shared_ptr<const ClientParameters> parameters_;
  const MTProxyOptions &options_;
  td::string file_path_;

  td::vector<Entry> entries_;  // ordered by id
  td::int64 next_id_ = 1;
  td::int64 active_id_ = 0;
  double last_switch_time_ = 0.0;
  std::map<td::int64, td::string> admin_token_hashes_;
  td::uint64 proxy_file_mtime_ = 0;
  bool is_proxy_file_missing_logged_ = false;

  std::map<td::uint64, ClientState> clients_;

  td::ActorOwn<td::ClientActor> td_client_;
  td::uint64 last_request_id_ = 0;
  std::map<td::uint64, PendingCheck> pending_checks_;
  td::vector<td::int64> check_queue_;
  size_t running_check_count_ = 0;
  bool is_check_all_running_ = false;
  double next_check_all_time_ = 0.0;

  bool is_switching_ = false;
  td::uint64 switch_generation_ = 0;
  td::vector<td::int64> switch_candidates_;
  td::string switch_reason_;

  void start_up() final;

  void tear_down() final;

  void timeout_expired() final;

  static td::int32 get_unix_time();

  static td::string get_token_hash(td::Slice token);

  Entry *get_entry(td::int64 id);

  const Entry *get_entry(td::int64 id) const;

  // adds the proxy or the source to the known proxy; returns the identifier of the proxy
  td::Result<td::int64> add_proxy(const MTProxy &proxy, td::Slice source, bool &is_new);

  bool evict_entry();

  void remove_entry(td::int64 id);

  void remove_source(td::Slice source);

  void remove_entries_without_sources();

  void load_proxy_file(bool force);

  void expire_entries();

  void load();

  void save() const;

  MTProxy get_active_proxy() const;

  void ensure_td_client();

  void check_all();

  void run_checks();

  void send_check(td::int64 id, CheckPurpose purpose);

  void on_check_result(PendingCheck check, td::td_api::object_ptr<td::td_api::Object> result);

  void update_active();

  void start_switch(td::string reason);

  void try_next_candidate();

  td::int64 get_next_in_turn() const;

  void set_active(td::int64 id, td::Slice reason);

  td::Status check_access(td::int64 bot_user_id, td::Slice token) const;

  void process_add(td::int64 bot_user_id, PromisedQueryPtr query);

  void process_set(PromisedQueryPtr query);

  void process_remove(PromisedQueryPtr query);

  static td::Result<td::vector<td::string>> get_texts(td::Slice links);

  static td::Result<td::int64> get_id_arg(const Query *query);
};

}  // namespace telegram_bot_api
