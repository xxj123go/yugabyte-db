// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#ifndef YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H
#define YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H

#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

#include "yb/client/client.h"
#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/status.h"
#include "yb/util/env.h"

namespace yb {

class ExternalDaemon;
class ExternalMaster;
class ExternalTabletServer;
class HostPort;
class MetricPrototype;
class MetricEntityPrototype;
class NodeInstancePB;
class Sockaddr;
class Subprocess;

namespace master {
class MasterServiceProxy;
} // namespace master

namespace rpc {
class Messenger;
} // namespace rpc

namespace server {
class ServerStatusPB;
} // namespace server

using yb::consensus::ChangeConfigType;
using yb::consensus::ConsensusServiceProxy;
using yb::consensus::OpId;

struct ExternalMiniClusterOptions {
  ExternalMiniClusterOptions();
  ~ExternalMiniClusterOptions();

  // Number of masters to start.
  // Default: 1
  int num_masters;

  // Number of TS to start.
  // Default: 1
  int num_tablet_servers;

  // Directory in which to store data.
  // Default: "", which auto-generates a unique path for this cluster.
  std::string data_root;

  // If true, binds each tablet server to a different loopback address.
  // This affects the server's RPC server, and also forces the server to
  // only use this IP address for outgoing socket connections as well.
  // This allows the use of iptables on the localhost to simulate network
  // partitions.
  //
  // The addressed used are 127.<A>.<B>.<C> where:
  // - <A,B> are the high and low bytes of the pid of the process running the
  //   minicluster (not the daemon itself).
  // - <C> is the index of the server within this minicluster.
  //
  // This requires that the system is set up such that processes may bind
  // to any IP address in the localhost netblock (127.0.0.0/8). This seems
  // to be the case on common Linux distributions. You can verify by running
  // 'ip addr | grep 127.0.0.1' and checking that the address is listed as
  // '127.0.0.1/8'.
  //
  // This option is disabled by default on OS X.
  //
  // NOTE: this does not currently affect the HTTP server.
  //
  // Default: true
  bool bind_to_unique_loopback_addresses;

  // The path where the yb daemons should be run from.
  // Default: "", which uses the same path as the currently running executable.
  // This works for unit tests, since they all end up in build/latest/bin.
  std::string daemon_bin_path;

  // Extra flags for tablet servers and masters respectively.
  //
  // In these flags, you may use the special string '${index}' which will
  // be substituted with the index of the tablet server or master.
  std::vector<std::string> extra_tserver_flags;
  std::vector<std::string> extra_master_flags;

  // If more than one master is specified, list of ports for the
  // masters in a consensus configuration. Port at index 0 is used for the leader
  // master.
  std::vector<uint16_t> master_rpc_ports;

  // Default timeout for operations involving RPC's, when none provided in the API.
  // Default : 10sec
  MonoDelta timeout_;

  Status RemovePort(const uint16_t port);
  Status AddPort(const uint16_t port);
};

// A mini-cluster made up of subprocesses running each of the daemons
// separately. This is useful for black-box or grey-box failure testing
// purposes -- it provides the ability to forcibly kill or stop particular
// cluster participants, which isn't feasible in the normal MiniCluster.
// On the other hand, there is little access to inspect the internal state
// of the daemons.
class ExternalMiniCluster {
 public:
  // Mode to which node types a certain action (like Shutdown()) should apply.
  enum NodeSelectionMode {
    TS_ONLY,
    ALL
  };

  // Threshold of the number of retries for master related rpc calls.
  static const int kMaxRetryIterations = 100;

  explicit ExternalMiniCluster(const ExternalMiniClusterOptions& opts);
  ~ExternalMiniCluster();

  // Start the cluster.
  Status Start();

  // Restarts the cluster. Requires that it has been Shutdown() first.
  Status Restart();

  // Like the previous method but performs initialization synchronously, i.e.
  // this will wait for all TS's to be started and initialized. Tests should
  // use this if they interact with tablets immediately after Start();
  Status StartSync();

  // Add a new TS to the cluster. The new TS is started.
  // Requires that the master is already running.
  Status AddTabletServer();

  // Shuts down the whole cluster or part of it, depending on the selected
  // 'mode'.
  // Currently, this uses SIGKILL on each daemon for a non-graceful shutdown.
  void Shutdown(NodeSelectionMode mode = ALL);

  // Return the IP address that the tablet server with the given index will bind to.
  // If options.bind_to_unique_loopback_addresses is false, this will be 127.0.0.1
  // Otherwise, it is another IP in the local netblock.
  std::string GetBindIpForTabletServer(int index) const;

  // Return a pointer to the running leader master. This may be NULL
  // if the cluster is not started.
  ExternalMaster* GetLeaderMaster();

  // Perform an RPC to determine the leader of the external mini
  // cluster.  Set 'index' to the leader master's index (for calls to
  // to master() below).
  //
  // NOTE: if a leader election occurs after this method is executed,
  // the last result may not be valid.
  Status GetLeaderMasterIndex(int* idx);

  // Return a non-leader master index
  Status GetFirstNonLeaderMasterIndex(int* idx);

  // Starts a new master and returns the address of the master object on success.
  // Not thread safe for now. We could move this to a static function outside External Mini Cluster,
  // but keeping it here for now as it is currently used only in conjunction with EMC.
  Status StartNewMaster(ExternalMaster** new_master);

  // Performs an add or remove from the existing config of this EMC, of the given master.
  Status ChangeConfig(ExternalMaster* master, ChangeConfigType type);

  // Performs an RPC to the given master to get the number of masters it is tracking in-memory.
  Status GetNumMastersAsSeenBy(ExternalMaster* master, int* num_peers);

  // Get the last committed opid for the current leader master.
  Status GetLastOpIdForLeader(OpId* opid);

  // The leader master sometimes does not commit the config in time on first setup, causing
  // CheckHasCommittedOpInCurrentTermUnlocked check - that the current term
  // should have had at least one commit - to fail. This API waits for the leader's commit term to
  // move ahead by one.
  Status WaitForLeaderCommitTermAdvance();

  // This API waits for the commit indices of all the master peers to reach the target index.
  Status WaitForMastersToCommitUpTo(int target_index);

  // If this cluster is configured for a single non-distributed
  // master, return the single master or NULL if the master is not
  // started. Exits with a CHECK failure if there are multiple
  // masters.
  ExternalMaster* master() const {
    if (masters_.empty())
      return nullptr;

    CHECK_EQ(masters_.size(), 1)
        << "master() should not be used with multiple masters, use GetLeaderMaster() instead.";
    return master(0);
  }

  // Return master at 'idx' or NULL if the master at 'idx' has not
  // been started.
  ExternalMaster* master(int idx) const {
    CHECK_LT(idx, masters_.size());
    return masters_[idx].get();
  }

  ExternalTabletServer* tablet_server(int idx) const {
    CHECK_LT(idx, tablet_servers_.size());
    return tablet_servers_[idx].get();
  }

  // Return ExternalTabletServer given its UUID. If not found, returns NULL.
  ExternalTabletServer* tablet_server_by_uuid(const std::string& uuid) const;

  // Return the index of the ExternalTabletServer that has the given 'uuid', or
  // -1 if no such UUID can be found.
  int tablet_server_index_by_uuid(const std::string& uuid) const;

  // Return all tablet servers and masters.
  std::vector<ExternalDaemon*> daemons() const;

  int num_tablet_servers() const {
    return tablet_servers_.size();
  }

  int num_masters() const {
    return masters_.size();
  }

  // Return the client messenger used by the ExternalMiniCluster.
  std::shared_ptr<rpc::Messenger> messenger();

  // Get the master leader consensus proxy.
  std::shared_ptr<consensus::ConsensusServiceProxy> GetLeaderConsensusProxy();

  // Get the given master's consensus proxy.
  std::shared_ptr<consensus::ConsensusServiceProxy> GetConsensusProxy(
      scoped_refptr<ExternalMaster> master);

  // If the cluster is configured for a single non-distributed master,
  // return a proxy to that master. Requires that the single master is
  // running.
  std::shared_ptr<master::MasterServiceProxy> master_proxy();

  // Returns an RPC proxy to the master at 'idx'. Requires that the
  // master at 'idx' is running.
  std::shared_ptr<master::MasterServiceProxy> master_proxy(int idx);

  // Wait until the number of registered tablet servers reaches the
  // given count on at least one of the running masters.  Returns
  // Status::TimedOut if the desired count is not achieved with the
  // given timeout.
  Status WaitForTabletServerCount(int count, const MonoDelta& timeout);

  // Runs gtest assertions that no servers have crashed.
  void AssertNoCrashes();

  // Wait until all tablets on the given tablet server are in 'RUNNING'
  // state.
  Status WaitForTabletsRunning(ExternalTabletServer* ts, const MonoDelta& timeout);

  // Create a client configured to talk to this cluster.
  // Builder may contain override options for the client. The master address will
  // be overridden to talk to the running master.
  //
  // REQUIRES: the cluster must have already been Start()ed.
  Status CreateClient(client::YBClientBuilder& builder,
                      client::sp::shared_ptr<client::YBClient>* client);

  // Sets the given flag on the given daemon, which must be running.
  //
  // This uses the 'force' flag on the RPC so that, even if the flag
  // is considered unsafe to change at runtime, it is changed.
  Status SetFlag(ExternalDaemon* daemon,
                 const std::string& flag,
                 const std::string& value);

  // Allocates a free port and stores a file lock guarding access to that port into an internal
  // array of file locks.
  uint16_t AllocateFreePort();

 private:
  FRIEND_TEST(MasterFailoverTest, TestKillAnyMaster);

  Status StartSingleMaster();

  Status StartDistributedMasters();

  std::string GetBinaryPath(const std::string& binary) const;
  std::string GetDataPath(const std::string& daemon_id) const;

  Status DeduceBinRoot(std::string* ret);
  Status HandleOptions();

  // Helper function to get a leader or (random) follower index
  Status GetPeerMasterIndex(int* idx, bool is_leader);

  // API to help update the cluster state (rpc ports)
  Status AddMaster(ExternalMaster* master);
  Status RemoveMaster(ExternalMaster* master);

  // Get the index of this master in the vector of masters. This might not be the insertion
  // order as we might have removed some masters within the vector.
  int GetIndexOfMaster(ExternalMaster* master) const;

  // Checks that the masters_ list and opts_ match in terms of the number of elements.
  Status CheckPortAndMasterSizes() const;

  // Return the list of opid's for all master's in this cluster.
  Status GetLastOpIdForEachMasterPeer(
      const MonoDelta& timeout,
      consensus::OpIdType opid_type,
      vector<OpId>* op_ids);

  // Ensure that the leader server is allowed to process a config change (by having at least one
  // commit in the current term as leader).
  Status WaitForLeaderToAllowChangeConfig(
      const string& uuid,
      ConsensusServiceProxy* leader_proxy);

  ExternalMiniClusterOptions opts_;

  // The root for binaries.
  std::string daemon_bin_path_;

  std::string data_root_;

  std::vector<scoped_refptr<ExternalMaster> > masters_;
  std::vector<scoped_refptr<ExternalTabletServer> > tablet_servers_;

  std::shared_ptr<rpc::Messenger> messenger_;

  std::vector<std::unique_ptr<FileLock>> free_port_file_locks_;

  DISALLOW_COPY_AND_ASSIGN(ExternalMiniCluster);
};

class ExternalDaemon : public RefCountedThreadSafe<ExternalDaemon> {
 public:
  ExternalDaemon(
    std::string short_description,
    std::shared_ptr<rpc::Messenger> messenger,
    std::string exe,
    std::string data_dir,
    std::vector<std::string> extra_flags);

  HostPort bound_rpc_hostport() const;
  Sockaddr bound_rpc_addr() const;
  HostPort bound_http_hostport() const;
  const NodeInstancePB& instance_id() const;
  const std::string& uuid() const;

  // Return the pid of the running process.
  // Causes a CHECK failure if the process is not running.
  pid_t pid() const;

  // Sends a SIGSTOP signal to the daemon.
  Status Pause();

  // Sends a SIGCONT signal to the daemon.
  Status Resume();

  // Return true if we have explicitly shut down the process.
  bool IsShutdown() const;

  // Return true if the process is still running.
  // This may return false if the process crashed, even if we didn't
  // explicitly call Shutdown().
  bool IsProcessAlive() const;

  virtual void Shutdown();

  const std::string& data_dir() const { return data_dir_; }

  // Return a pointer to the flags used for this server on restart.
  // Modifying these flags will only take effect on the next restart.
  std::vector<std::string>* mutable_flags() { return &extra_flags_; }

  // Retrieve the value of a given metric from this server. The metric must
  // be of int64_t type.
  //
  // 'value_field' represents the particular field of the metric to be read.
  // For example, for a counter or gauge, this should be 'value'. For a
  // histogram, it might be 'total_count' or 'mean'.
  //
  // 'entity_id' may be NULL, in which case the first entity of the same type
  // as 'entity_proto' will be matched.
  Status GetInt64Metric(const MetricEntityPrototype* entity_proto,
                        const char* entity_id,
                        const MetricPrototype* metric_proto,
                        const char* value_field,
                        int64_t* value) const;

 protected:
  friend class RefCountedThreadSafe<ExternalDaemon>;
  virtual ~ExternalDaemon();

  Status StartProcess(const std::vector<std::string>& flags);

  // In a code-coverage build, try to flush the coverage data to disk.
  // In a non-coverage build, this does nothing.
  void FlushCoverage();

  const std::string short_description_;
  const std::shared_ptr<rpc::Messenger> messenger_;
  const std::string exe_;
  const std::string data_dir_;
  std::vector<std::string> extra_flags_;

  gscoped_ptr<Subprocess> process_;

  gscoped_ptr<server::ServerStatusPB> status_;

  // These capture the daemons parameters and running ports and
  // are used to Restart() the daemon with the same parameters.
  HostPort bound_rpc_;
  HostPort bound_http_;

  DISALLOW_COPY_AND_ASSIGN(ExternalDaemon);

 private:
   void StartTailerThread(std::string line_prefix, int child_fd, ostream* out);
};

// Resumes a daemon that was stopped with ExteranlDaemon::Pause() upon
// exiting a scope.
class ScopedResumeExternalDaemon {
 public:
  // 'daemon' must remain valid for the lifetime of a
  // ScopedResumeExternalDaemon object.
  explicit ScopedResumeExternalDaemon(ExternalDaemon* daemon);

  // Resume 'daemon_'.
  ~ScopedResumeExternalDaemon();

 private:
  ExternalDaemon* daemon_;

  DISALLOW_COPY_AND_ASSIGN(ScopedResumeExternalDaemon);
};


class ExternalMaster : public ExternalDaemon {
 public:
  ExternalMaster(
    int master_index,
    const std::shared_ptr<rpc::Messenger>& messenger,
    const std::string& exe,
    const std::string& data_dir,
    const std::vector<std::string>& extra_flags,
    const std::string& rpc_bind_address = "127.0.0.1:0",
    uint16_t http_port = 0,
    const std::string& master_addrs = "");

  Status Start(bool shell_mode = false);

  // Restarts the daemon.
  // Requires that it has previously been shutdown.
  Status Restart() WARN_UNUSED_RESULT;

 private:
  friend class RefCountedThreadSafe<ExternalMaster>;
  virtual ~ExternalMaster();

  // used on start to create the cluster; on restart, this should not be used!
  const std::string rpc_bind_address_;
  const std::string master_addrs_;
  const uint16_t http_port_;
};

class ExternalTabletServer : public ExternalDaemon {
 public:
  ExternalTabletServer(
    int tablet_server_index,
    const std::shared_ptr<rpc::Messenger>& messenger,
    const std::string& exe,
    const std::string& data_dir,
    std::string bind_host,
    uint16_t rpc_port,
    uint16_t http_port,
    const std::vector<HostPort>& master_addrs,
    const std::vector<std::string>& extra_flags);

  Status Start();

  // Restarts the daemon.
  // Requires that it has previously been shutdown.
  Status Restart() WARN_UNUSED_RESULT;


 private:
  const std::string master_addrs_;
  const std::string bind_host_;
  const uint16_t rpc_port_;
  const uint16_t http_port_;

  friend class RefCountedThreadSafe<ExternalTabletServer>;
  virtual ~ExternalTabletServer();
};

// Custom functor for predicate based comparison with the master list.
struct MasterComparator {
  MasterComparator(ExternalMaster* master) : master_(master) { }

  // We look for the exact master match. Since it is possible to stop/restart master on
  // a given host/port, we do not want a stale master pointer input to match a newer master.
  bool operator()(const scoped_refptr<ExternalMaster>& other) const {
    return master_ == other.get();
  }

 private:
  const ExternalMaster* master_;
};

} // namespace yb
#endif /* YB_INTEGRATION_TESTS_EXTERNAL_MINI_CLUSTER_H */
