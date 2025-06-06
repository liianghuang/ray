// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/raylet/scheduling/cluster_resource_manager.h"

#include <string>
#include <utility>
#include <vector>

#include "ray/common/grpc_util.h"
#include "ray/common/ray_config.h"

namespace ray {

ClusterResourceManager::ClusterResourceManager(instrumented_io_context &io_service)
    : timer_(PeriodicalRunner::Create(io_service)) {
  timer_->RunFnPeriodically(
      [this]() {
        auto syncer_delay = absl::Milliseconds(
            RayConfig::instance().ray_syncer_message_refresh_interval_ms());
        for (auto &[node_id, resource] : received_node_resources_) {
          auto modified_ts = GetNodeResourceModifiedTs(node_id);
          if (modified_ts && *modified_ts + syncer_delay < absl::Now()) {
            AddOrUpdateNode(node_id, resource);
          }
        }
      },
      RayConfig::instance().ray_syncer_message_refresh_interval_ms(),
      "ClusterResourceManager.ResetRemoteNodeView");
}

std::optional<absl::Time> ClusterResourceManager::GetNodeResourceModifiedTs(
    scheduling::NodeID node_id) const {
  auto iter = nodes_.find(node_id);
  if (iter == nodes_.end()) {
    return std::nullopt;
  }
  return iter->second.GetViewModifiedTs();
}

void ClusterResourceManager::AddOrUpdateNode(
    scheduling::NodeID node_id,
    const absl::flat_hash_map<std::string, double> &resources_total,
    const absl::flat_hash_map<std::string, double> &resources_available) {
  NodeResources node_resources =
      ResourceMapToNodeResources(resources_total, resources_available);
  AddOrUpdateNode(node_id, node_resources);
}

void ClusterResourceManager::AddOrUpdateNode(scheduling::NodeID node_id,
                                             const NodeResources &node_resources) {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    // This node is new, so add it to the map.
    nodes_.emplace(node_id, node_resources);
  } else {
    // This node exists, so update its resources.
    it->second = Node(node_resources);
  }
}

bool ClusterResourceManager::UpdateNode(
    scheduling::NodeID node_id,
    const syncer::ResourceViewSyncMessage &resource_view_sync_message) {
  if (!nodes_.contains(node_id)) {
    return false;
  }

  auto resources_total = MapFromProtobuf(resource_view_sync_message.resources_total());
  auto resources_available =
      MapFromProtobuf(resource_view_sync_message.resources_available());
  NodeResources node_resources =
      ResourceMapToNodeResources(resources_total, resources_available);
  NodeResources local_view;
  RAY_CHECK(GetNodeResources(node_id, &local_view));

  local_view.total = node_resources.total;
  local_view.available = node_resources.available;
  local_view.object_pulls_queued = resource_view_sync_message.object_pulls_queued();

  // Update the idle duration for the node in terms of resources usage.
  local_view.idle_resource_duration_ms = resource_view_sync_message.idle_duration_ms();

  // Last update time to the local node resources view.
  local_view.last_resource_update_time = absl::Now();

  local_view.is_draining = resource_view_sync_message.is_draining();
  local_view.draining_deadline_timestamp_ms =
      resource_view_sync_message.draining_deadline_timestamp_ms();

  AddOrUpdateNode(node_id, local_view);
  received_node_resources_[node_id] = std::move(local_view);
  return true;
}

bool ClusterResourceManager::RemoveNode(scheduling::NodeID node_id) {
  received_node_resources_.erase(node_id);
  return nodes_.erase(node_id) != 0;
}

bool ClusterResourceManager::GetNodeResources(scheduling::NodeID node_id,
                                              NodeResources *ret_resources) const {
  auto it = nodes_.find(node_id);
  if (it != nodes_.end()) {
    *ret_resources = it->second.GetLocalView();
    return true;
  } else {
    return false;
  }
}

const NodeResources &ClusterResourceManager::GetNodeResources(
    scheduling::NodeID node_id) const {
  const auto &node = map_find_or_die(nodes_, node_id);
  return node.GetLocalView();
}

int64_t ClusterResourceManager::NumNodes() const { return nodes_.size(); }

void ClusterResourceManager::UpdateResourceCapacity(scheduling::NodeID node_id,
                                                    scheduling::ResourceID resource_id,
                                                    double resource_total) {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    NodeResources node_resources;
    it = nodes_.emplace(node_id, node_resources).first;
  }

  auto local_view = it->second.GetMutableLocalView();
  FixedPoint resource_total_fp(resource_total);
  auto local_total = local_view->total.Get(resource_id);
  auto local_available = local_view->available.Get(resource_id);
  auto diff_capacity = resource_total_fp - local_total;
  auto total = local_total + diff_capacity;
  auto available = local_available + diff_capacity;
  if (total < 0) {
    total = 0;
  }
  if (available < 0) {
    available = 0;
  }
  local_view->total.Set(resource_id, total);
  local_view->available.Set(resource_id, available);
}

bool ClusterResourceManager::DeleteResources(
    scheduling::NodeID node_id, const std::vector<scheduling::ResourceID> &resource_ids) {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    return false;
  }

  auto local_view = it->second.GetMutableLocalView();
  for (const auto &resource_id : resource_ids) {
    local_view->total.Set(resource_id, 0);
    local_view->available.Set(resource_id, 0);
  }
  return true;
}

std::string ClusterResourceManager::GetNodeResourceViewString(
    scheduling::NodeID node_id) const {
  const auto &node = map_find_or_die(nodes_, node_id);
  return node.GetLocalView().DictString();
}

const absl::flat_hash_map<scheduling::NodeID, Node>
    &ClusterResourceManager::GetResourceView() const {
  return nodes_;
}

bool ClusterResourceManager::SubtractNodeAvailableResources(
    scheduling::NodeID node_id, const ResourceRequest &resource_request) {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    return false;
  }

  NodeResources *resources = it->second.GetMutableLocalView();

  resources->available -= resource_request.GetResourceSet();
  resources->available.RemoveNegative();

  // TODO(swang): We should also subtract object store memory if the task has
  // arguments. Right now we do not modify object_pulls_queued in case of
  // performance regressions in spillback.

  return true;
}

bool ClusterResourceManager::HasFeasibleResources(
    scheduling::NodeID node_id, const ResourceRequest &resource_request) const {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    return false;
  }

  return it->second.GetLocalView().IsFeasible(resource_request);
}

bool ClusterResourceManager::HasAvailableResources(
    scheduling::NodeID node_id,
    const ResourceRequest &resource_request,
    bool ignore_object_store_memory_requirement) const {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    return false;
  }

  return it->second.GetLocalView().IsAvailable(resource_request,
                                               ignore_object_store_memory_requirement);
}

bool ClusterResourceManager::AddNodeAvailableResources(scheduling::NodeID node_id,
                                                       const ResourceSet &resource_set) {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    return false;
  }

  auto node_resources = it->second.GetMutableLocalView();
  for (auto &resource_id : resource_set.ResourceIds()) {
    if (node_resources->total.Has(resource_id)) {
      auto available = node_resources->available.Get(resource_id);
      auto total = node_resources->total.Get(resource_id);
      auto new_available = available + resource_set.Get(resource_id);
      if (new_available > total) {
        new_available = total;
      }
      node_resources->available.Set(resource_id, new_available);
    }
  }
  return true;
}

bool ClusterResourceManager::UpdateNodeNormalTaskResources(
    scheduling::NodeID node_id, const rpc::ResourcesData &resource_data) {
  auto iter = nodes_.find(node_id);
  if (iter != nodes_.end()) {
    auto node_resources = iter->second.GetMutableLocalView();
    if (resource_data.resources_normal_task_changed() &&
        resource_data.resources_normal_task_timestamp() >
            node_resources->latest_resources_normal_task_timestamp) {
      auto normal_task_resources =
          ResourceSet(MapFromProtobuf(resource_data.resources_normal_task()));
      auto &local_normal_task_resources = node_resources->normal_task_resources;
      if (normal_task_resources != local_normal_task_resources) {
        local_normal_task_resources = normal_task_resources;
        node_resources->latest_resources_normal_task_timestamp =
            resource_data.resources_normal_task_timestamp();
        return true;
      }
    }
  }

  return false;
}

std::string ClusterResourceManager::DebugString(
    std::optional<size_t> max_num_nodes_to_include) const {
  std::stringstream buffer;
  size_t num_nodes_included = 0;
  for (auto &node : GetResourceView()) {
    if (max_num_nodes_to_include.has_value() &&
        num_nodes_included >= max_num_nodes_to_include.value()) {
      break;
    }
    buffer << "node id: " << node.first.ToInt();
    buffer << node.second.GetLocalView().DebugString();
    ++num_nodes_included;
  }
  buffer << " " << bundle_location_index_.DebugString();
  return buffer.str();
}

BundleLocationIndex &ClusterResourceManager::GetBundleLocationIndex() {
  return bundle_location_index_;
}

void ClusterResourceManager::SetNodeLabels(
    const scheduling::NodeID &node_id,
    const absl::flat_hash_map<std::string, std::string> &labels) {
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) {
    NodeResources node_resources;
    it = nodes_.emplace(node_id, node_resources).first;
  }
  it->second.GetMutableLocalView()->labels = labels;
}

}  // namespace ray
