/**
 * @file client.cc
 * @author ERaftGroup
 * @brief
 * @version 0.1
 * @date 2023-06-17
 *
 * @copyright Copyright (c) 2023
 *
 */

#include "client.h"

#include <stdint.h>
#include <sys/time.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "util.h"

PacketLength Client::_HandlePacket(const char *start, std::size_t bytes) {
  const char *const end = start + bytes;
  const char       *ptr = start;
  parser_.ParseRequest(ptr, end);

  CommandHandler *handler = nullptr;
  if (parser_.GetParams()[0] == "info") {
    handler = new InfoCommandHandler();
  } else if (parser_.GetParams()[0] == "set") {
    handler = new SetCommandHandler();
  } else if (parser_.GetParams()[0] == "get") {
    handler = new GetCommandHandler();
  } else {
    handler = new UnKnowCommandHandler();
  }

  handler->Execute(parser_.GetParams(), this);
  return static_cast<PacketLength>(bytes);
}

Client::Client(std::string meta_addrs) : leader_addr_("") {
  // init stub to meta server node
  auto meta_node_addrs = StringUtil::Split(meta_addrs, ',');
  for (auto meta_node_addr : meta_node_addrs) {
    TraceLog("DEBUG: init rpc link to ", meta_node_addr);
    auto chan_ =
        grpc::CreateChannel(meta_node_addr, grpc::InsecureChannelCredentials());
    std::unique_ptr<ERaftKv::Stub> stub_(ERaftKv::NewStub(chan_));
    this->stubs_[meta_node_addr] = std::move(stub_);
  }
  // sync config
  SyncClusterConfig();
  _Reset();
}

void Client::_Reset() {
  parser_.Reset();
  reply_.Clear();
}

void Client::OnConnect() {}

std::string Client::GetLeaderAddr(std::string partion_key) {
  std::string leader_address;
  int64_t     key_slot = -1;
  key_slot = HashUtil::CRC64(0, partion_key.c_str(), partion_key.size()) % 1024;
  TraceLog("DEBUG: partion key " + partion_key + " with slot ", key_slot);
  for (auto sg : cluster_conf_.shard_group()) {
    for (auto sl : sg.slots()) {
      if (key_slot == sl.id()) {
        // find sg leader addr
        for (auto server : sg.servers()) {
          if (server.id() == sg.leader_id()) {
            ClientContext                   context;
            eraftkv::ClusterConfigChangeReq req;
            req.set_change_type(eraftkv::ChangeType::MetaMembersQuery);
            eraftkv::ClusterConfigChangeResp resp;
            auto status = stubs_[server.address()]->ClusterConfigChange(
                &context, req, &resp);
            for (int i = 0; i < resp.shard_group(0).servers_size(); i++) {
              if (resp.shard_group(0).leader_id() ==
                  resp.shard_group(0).servers(i).id()) {
                leader_address = resp.shard_group(0).servers(i).address();
              }
            }
          }
        }
      }
    }
  }

  return leader_address;
}

EStatus Client::SyncClusterConfig() {
  ClientContext                   context;
  eraftkv::ClusterConfigChangeReq req;
  req.set_change_type(eraftkv::ChangeType::ShardsQuery);
  auto status_ = this->stubs_.begin()->second->ClusterConfigChange(
      &context, req, &cluster_conf_);
  for (auto sg : cluster_conf_.shard_group()) {
    for (auto server : sg.servers()) {
      auto chan_ = grpc::CreateChannel(server.address(),
                                       grpc::InsecureChannelCredentials());
      std::unique_ptr<ERaftKv::Stub> stub_(ERaftKv::NewStub(chan_));
      this->stubs_[server.address()] = std::move(stub_);
    }
  }

  return status_.ok() ? EStatus::kOk : EStatus::kError;
}