#include "raft/group_manager.h"

#include "resource_mgmt/io_priority.h"

namespace raft {

group_manager::group_manager(
  model::node_id self,
  model::timeout_clock::duration disk_timeout,
  std::chrono::milliseconds heartbeat_interval,
  ss::sharded<rpc::connection_cache>& clients)
  : _self(self)
  , _disk_timeout(disk_timeout)
  , _client(make_rpc_client_protocol(clients))
  , _heartbeats(heartbeat_interval, _client) {}

ss::future<> group_manager::start() { return _heartbeats.start(); }

ss::future<> group_manager::stop() {
    return _gate.close()
      .then([this] { return _heartbeats.stop(); })
      .then([this] {
          return ss::parallel_for_each(
            _groups,
            [](ss::lw_shared_ptr<consensus> raft) { return raft->stop(); });
      });
}

ss::future<ss::lw_shared_ptr<raft::consensus>> group_manager::start_group(
  raft::group_id id,
  std::vector<model::broker> nodes,
  storage::log log,
  std::optional<raft::consensus::append_entries_cb_t> append_entries_cb) {
    auto raft = ss::make_lw_shared<raft::consensus>(
      _self,
      id,
      raft::group_configuration{.nodes = std::move(nodes)},
      raft::timeout_jitter(
        config::shard_local_cfg().raft_election_timeout_ms()),
      log,
      raft_priority(),
      _disk_timeout,
      _client,
      [this](raft::leadership_status st) {
          trigger_leadership_notification(std::move(st));
      },
      std::move(append_entries_cb));

    return ss::with_gate(_gate, [this, raft] {
        return raft->start().then([this, raft] {
            return _heartbeats.register_group(raft).then([this, raft] {
                _groups.push_back(raft);
                return raft;
            });
        });
    });
}

ss::future<> group_manager::stop_group(ss::lw_shared_ptr<raft::consensus> c) {
    return c->stop()
      .then(
        [this, id = c->group()] { return _heartbeats.deregister_group(id); })
      .finally([this, c] {
          _groups.erase(
            std::remove(_groups.begin(), _groups.end(), c), _groups.end());
      });
}

void group_manager::trigger_leadership_notification(
  raft::leadership_status st) {
    for (auto& cb : _notifications) {
        cb.second(st.group, st.term, st.current_leader);
    }
}

} // namespace raft