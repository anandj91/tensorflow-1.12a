/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/grappler/optimizers/graph_partitioner.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <stack>

#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/tensor.pb.h"  // NOLINT
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/grappler/clusters/virtual_cluster.h"
#include "tensorflow/core/grappler/costs/graph_memory.h"
#include "tensorflow/core/grappler/costs/graph_properties.h"
#include "tensorflow/core/grappler/graph_view.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/op_types.h"
#include "tensorflow/core/grappler/optimizers/graph_rewriter.h"
#include "tensorflow/core/grappler/optimizers/static_schedule.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/grappler/utils/topological_sort.h"
#include "tensorflow/core/grappler/utils/traversal.h"
#include "tensorflow/core/lib/math/math_util.h"
#include "tensorflow/core/protobuf/rewriter_config.pb.h"

namespace tensorflow {
namespace grappler {

static bool IsSwappable(const GraphView& graph, GraphView::OutputPort output) {
  const NodeDef& node = *output.node;
  // There is no point in swapping out persistent tensors, since the tensor will
  // continue to use memory.
  if (IsPersistent(node)) {
    return false;
  }

  const OpDef* op_def;
  if (!OpRegistry::Global()->LookUpOpDef(node.op(), &op_def).ok()) {
    return false;
  }

  DataType dtype;
  if (!OutputTypeForNode(node, *op_def, output.port_id, &dtype).ok()) {
    return false;
  }

  // References can only refer to persistent memory: therefore the node isn't
  // swappable.
  if (IsRefType(dtype)) {
    return false;
  }

  if (output.node->op() == "Identity" || output.node->op() == "Reshape") {
    // If placed on the same device, these nodes are just forwarding references
    // to their input. Therefore they are swappable iff their fanin is swappable
    // or it resides on a different device.
    GraphView::InputPort input;
    input.node = output.node;
    input.port_id = 0;

    GraphView::OutputPort fanin = graph.GetRegularFanin(input);
    CHECK(fanin.node);
    if (fanin.node->device() == node.device()) {
      return IsSwappable(graph, fanin);
    }
  }
  return true;
}

static bool IsSwappable(GraphView::InputPort input) {
  const NodeDef& node = *input.node;

  const OpDef* op_def;
  if (!OpRegistry::Global()->LookUpOpDef(node.op(), &op_def).ok()) {
    return false;
  }

  DataType dtype;
  if (!InputTypeForNode(node, *op_def, input.port_id, &dtype).ok()) {
    return false;
  }

  return !IsRefType(dtype);
}

void PartitionGraph(GraphDef* graph,
                    const std::unordered_map<string, DeviceProperties> &devices,
                    std::unordered_map<int32, std::vector<NodeDef*>> *node_partitions) {
  const size_t kPartitionSize = std::atoi(std::getenv("KPART"));

  SimpleGraphView graph_view;
  CHECK(graph_view.Initialize(*graph).ok());

  size_t num_devices = devices.size();
  std::vector<int> per_device_num_ops_curr_partition(num_devices, 0);
  std::unordered_map<string, int> device_name_to_index_map;
  int i = 0;
  for (const auto &device_pair : devices) {
    device_name_to_index_map[device_pair.first] = i++;
  }

  std::vector<std::stack<int>> per_device_ready_nodes(num_devices);
  std::vector<int> num_ready_inputs(graph_view.num_nodes(), 0);

  for (int i = 0; i < graph_view.num_nodes(); i++) {
    if (graph_view.inputs(i).empty()) {
      const string &device_name = graph_view.node(i).device();
      int32 device_index = device_name_to_index_map[device_name];
      per_device_ready_nodes[device_index].push(i);
    }

    if (IsMerge(graph_view.node(i))) {
      for (int input : graph_view.inputs(i)) {
        if (IsNextIteration(graph_view.node(input))) {
          num_ready_inputs[i]++;
        }
      }
    }
  }

  int32 partition_id = 1;
  bool executed_all = false;
  while (!executed_all) {
    executed_all = true;
    for (int dev_index = 0; dev_index < per_device_ready_nodes.size();
         dev_index++) {
      if (per_device_num_ops_curr_partition[dev_index] == kPartitionSize) {
        partition_id++;
        for (auto& num_ops : per_device_num_ops_curr_partition) num_ops = 0;
      }
      auto &ready_node_stack = per_device_ready_nodes[dev_index];
      if (!ready_node_stack.empty()) {
        int ready_node = ready_node_stack.top();
        ready_node_stack.pop();
        executed_all = false;
        NodeDef* node = graph->mutable_node(ready_node);
        node->set_priority(partition_id);
        (*node_partitions)[partition_id].emplace_back(node);
        per_device_num_ops_curr_partition[dev_index] += 1;

        for (int fanout : graph_view.outputs(ready_node)) {
          ++num_ready_inputs[fanout];

          if (num_ready_inputs[fanout] == graph_view.inputs(fanout).size()) {
            const string& fanout_device = graph_view.node(fanout).device();
            int fanout_device_index = device_name_to_index_map[fanout_device];
            per_device_ready_nodes[fanout_device_index].push(fanout);
          }
        }
      }
    }
  }
}

void AddSwapNodesForOneNode(GraphDef* graph,
                            NodeDef* generator,
                            std::unordered_map<int32, std::vector<GraphView::InputPort>
                            >* output_port_to_uses_after_swap,
                            const std::unordered_map<int32, std::vector<NodeDef*>>& node_partitions) {


  const OpDef* op_def;
  CHECK(OpRegistry::Global()->LookUpOpDef(generator->op(), &op_def).ok());

  int32 generator_partition = generator->priority();

  for (auto& output_port_to_uses_after_swap_pair :
           (*output_port_to_uses_after_swap)) {
    int32 output_port = output_port_to_uses_after_swap_pair.first;
    DataType output_type;
    CHECK(OutputTypeForNode(*generator, *op_def, output_port,
                            &output_type).ok());
    CHECK(!IsRefType(output_type)) << " node = " << generator->name();

    string tensor_to_swap = strings::StrCat(generator->name(), "_", output_port);
    string swap_out_name = strings::StrCat("swap_out_", tensor_to_swap);
    string swap_in_name_base = strings::StrCat("swap_in_", tensor_to_swap);

    // Force the tensor to be copied to cpu.
    NodeDef* swap_out_node = graph->add_node();
    swap_out_node->set_name(swap_out_name);
    swap_out_node->set_op("_CopyFromGpuToHost");
    swap_out_node->set_device(generator->device());
    swap_out_node->set_priority(generator->priority());
    swap_out_node->add_input(strings::StrCat(generator->name(), ":", output_port));

    string coloc_group = strings::StrCat("loc@", tensor_to_swap);
    (*swap_out_node->mutable_attr())["_class"].mutable_list()->add_s(coloc_group);
    (*generator->mutable_attr())["_class"].mutable_list()->add_s(coloc_group);
    (*swap_out_node->mutable_attr())["T"].set_type(output_type);

    auto& uses_after_swap = output_port_to_uses_after_swap_pair.second;
    std::sort(uses_after_swap.begin(),
              uses_after_swap.end(),
              [&] (const GraphView::InputPort& a, const GraphView::InputPort& b) {
                return a.node->priority() < b.node->priority();
              });

    NodeDef* prev_input_node = nullptr;
    NodeDef* prev_swap_in_node = nullptr;
    for (const auto& input_port : uses_after_swap) {
      NodeDef* input_node = input_port.node;
      int32 input_node_partition = input_node->priority();
      int32 port_id = input_port.port_id;
      NodeDef *swap_in_node = nullptr;

      if (!prev_input_node ||
          (prev_input_node && (prev_input_node->priority() + 1 < input_node->priority()))) {
        string swap_in_name = strings::StrCat(swap_in_name_base, "_", input_node->name(),
                                              "_", port_id);
        swap_in_node = graph->add_node();
        swap_in_node->set_name(swap_in_name);
        swap_in_node->set_op("_CopyFromHostToGpu");
        swap_in_node->add_input(swap_out_name);
        swap_in_node->set_device(generator->device());
        swap_in_node->set_priority(std::max(input_node->priority() - 1, 0));

        if (prev_input_node) {
          swap_in_node->add_input(strings::StrCat("^", prev_input_node->name()));
        }
        prev_swap_in_node = swap_in_node;

        (*swap_in_node->mutable_attr())["_class"].mutable_list()->add_s(coloc_group);
        (*swap_in_node->mutable_attr())["T"].set_type(output_type);

      } else {
        CHECK(prev_input_node->priority() == input_node->priority() ||
              prev_input_node->priority() + 1 == input_node->priority());
        swap_in_node = prev_swap_in_node;
      }

      prev_input_node = input_node;
      *(input_node->mutable_input(port_id)) = swap_in_node->name();
    }
  }
}

void SwapTensors(const GraphView &view,
                 const std::unordered_map<int32, std::vector<NodeDef*>>& node_partitions,
                 GraphDef* graph) {
  using OutputPortToUsesAfterSwapMap = std::unordered_map<int32, std::vector<GraphView::InputPort>>;
  std::unordered_map<NodeDef*, OutputPortToUsesAfterSwapMap> node_to_swap_map;

  for (const auto &node_partitions_pair : node_partitions) {
    int32 partition_id = node_partitions_pair.first;
    const std::vector<NodeDef*>& node_vec = node_partitions_pair.second;
    for (NodeDef* node : node_vec) {
      if (node->op() == "_CopyFromHostToGpu" || node->op() == "_CopyFromGpuToHost") continue;
      DeviceNameUtils::ParsedName parsed_device_name;
      DeviceNameUtils::ParseFullName(node->device(), &parsed_device_name);
      if (parsed_device_name.type != "GPU" &&
          parsed_device_name.type != "gpu") continue;

      auto fanout_edge_set = view.GetFanoutEdges(*node, false);
      OutputPortToUsesAfterSwapMap output_port_to_uses_after_swap;

      for (const auto &fanout_edge : fanout_edge_set) {
        GraphView::InputPort target = fanout_edge.tgt;
        NodeDef* dst_node = target.node;
        int partition_distance = 2;
        if (dst_node->device() != node->device()) continue;

        int32 dst_node_partition = dst_node->priority();

        if (dst_node_partition - partition_id > partition_distance) {
          GraphView::OutputPort source = fanout_edge.src;
          output_port_to_uses_after_swap[source.port_id].emplace_back(target);
        }
      }

      for (auto output_port_to_uses_after_swap_iter = output_port_to_uses_after_swap.begin();
           output_port_to_uses_after_swap_iter != output_port_to_uses_after_swap.end();) {
        int32 output_port = output_port_to_uses_after_swap_iter->first;
        GraphView::OutputPort output(node, output_port);
        if(!IsSwappable(view, output)) {
          output_port_to_uses_after_swap.erase(output_port_to_uses_after_swap_iter++);
        } else {
          output_port_to_uses_after_swap_iter++;
        }
      }
      if (output_port_to_uses_after_swap.size() > 0) {
        node_to_swap_map.emplace(
            std::make_pair(node, output_port_to_uses_after_swap));
      }
    }
  }

  for (auto& node_to_swap_map_pair : node_to_swap_map) {
    NodeDef* node = node_to_swap_map_pair.first;
    auto& output_port_to_uses_after_swap = node_to_swap_map_pair.second;
    AddSwapNodesForOneNode(graph,
                           node,
                           &output_port_to_uses_after_swap,
                           node_partitions);

  }
}

bool SwappingPass(RewriterConfig::MemOptType optimization_level,
                  Cluster* cluster, GrapplerItem* item) {
  GraphView view(&item->graph);
  const auto& fetch_vec = item->fetch;

  if (optimization_level == RewriterConfig::DEFAULT_MEM_OPT ||
      optimization_level == RewriterConfig::SWAPPING_HEURISTICS ||
      optimization_level == RewriterConfig::HEURISTICS) {
    GraphMemory memory(*item);
    const std::unordered_map<string, DeviceProperties>& devices = cluster->GetDevices();

    RunMetadata metadata;
    Status s = memory.InferStaticallyAndGetRunMetadata(devices, &metadata);
    if (!s.ok()) {
      VLOG(1) << "Failed to infer memory usage: " << s.error_message();
      return false;
    }

    bool need_swap = false;
    for (const auto& device : devices) {
      const DeviceProperties& prop = device.second;

      if (prop.type() != "GPU") continue;
      if (prop.memory_size() <= 0) continue;

      const string& device_name = device.first;
      const GraphMemory::MemoryUsage& mem_usage = memory.GetPeakMemoryUsage(device_name);

      if (prop.memory_size() <= mem_usage.used_memory) {
        need_swap = true;
        break;
      }
    }

    std::unordered_map<int32, std::vector<NodeDef*>> node_partitions;
    PartitionGraph(&(item->graph),
                   devices,
                   &node_partitions);

    SwapTensors(view,
                node_partitions,
                &item->graph);
  }

  GraphView new_view(&item->graph);
  for (const auto &node : item->graph.node()) {
    NodeDef* node_def = new_view.GetNode(node.name());
    LOG(INFO) << __func__ << " node = " << node_def->name()
            << " num_inputs = " << node_def->input_size()
            << " device = " << node_def->device()
            << "  op_type = " << node_def->op()
              << " priority = " << node_def->priority();
    for (auto &fanout : new_view.GetFanouts(*node_def, true)) {
      LOG(INFO) << __func__ << " fanout = " << fanout.node->name()
              << " device = " << fanout.node->device()
                << " priority = " << fanout.node->priority();
      }
    for (auto &fanin : new_view.GetFanins(*node_def, true)) {
      LOG(INFO) << __func__ << " fanin = " << fanin.node->name()
              << " device = " << fanin.node->device()
          << " priority = " << fanin.node->priority();
      }
    }
 return true;
}

void
ComputeAndPrintGraphStats(GrapplerItem* item) {
  SimpleGraphView graph_view;
  CHECK(graph_view.Initialize(item->graph).ok());
  std::vector<int> topo_order;
  ComputeTopologicalOrder(graph_view, &topo_order, nullptr);
  std::vector<int> rank(graph_view.num_nodes(), 0);
  int max_rank = 0;
  size_t total_inputs = 0;
  size_t total_outputs = 0;
  for (int i = 0; i < topo_order.size(); i++) {
    int node_id = topo_order[i];
    int node_rank = rank[node_id];
    total_inputs += graph_view.inputs(node_id).size();
    total_outputs += graph_view.outputs(node_id).size();
    for (auto output_node : graph_view.outputs(node_id)) {
      rank[output_node] = std::max(rank[output_node], node_rank + 1);
      max_rank = std::max(max_rank, rank[output_node]);
    }
  }

  size_t total_rank_diff_input = 0;
  size_t total_rank_diff_output = 0;
  for (int i = 0; i < topo_order.size(); i++) {
    int node_id = topo_order[i];
    int node_rank = rank[node_id];

    for (auto input_node : graph_view.inputs(node_id)) {
      total_rank_diff_input += node_rank - rank[input_node];
    }

    for (auto output_node : graph_view.outputs(node_id)) {
      total_rank_diff_output += rank[output_node] - node_rank;
    }
  }

  LOG(INFO) << __func__ << " depth = " << max_rank
            << " num_nodes = " << topo_order.size()
            << " avg_indegree = " << ((float) total_inputs) / ((float) topo_order.size())
            << " avg_outdegree = " << ((float) total_outputs) / ((float) topo_order.size())
            << " avg_input_rank_diff = " << ((float) total_rank_diff_input) / ((float) total_inputs)
            << " avg_output_rank_diff = " << ((float) total_rank_diff_output) / ((float) total_outputs);
}

Status GraphPartitioner::Optimize(Cluster* cluster, const GrapplerItem& item,
                                 GraphDef* optimized_graph) {
  *optimized_graph = item.graph;
  GrapplerItem optimized_item(item, optimized_graph);
  ComputeAndPrintGraphStats(&optimized_item);
  if ((optimization_level_ == RewriterConfig::DEFAULT_MEM_OPT ||
       optimization_level_ == RewriterConfig::SWAPPING_HEURISTICS ||
       optimization_level_ == RewriterConfig::HEURISTICS ||
       optimization_level_ == RewriterConfig::MANUAL) &&
      cluster != nullptr) {
    SwappingPass(optimization_level_, cluster,
               &optimized_item);
  }

  optimized_graph->Swap(&optimized_item.graph);
  return Status::OK();
}

void GraphPartitioner::Feedback(Cluster* cluster, const GrapplerItem& item,
                               const GraphDef& optimized_graph, double result) {
  // Nothing to do for GraphPartitioner.
}

}  // end namespace grappler
}  // end namespace tensorflow
