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

#include "runtime/coordinator-backend-state.h"

#include <boost/lexical_cast.hpp>

#include "common/object-pool.h"
#include "exec/exec-node.h"
#include "exec/scan-node.h"
#include "runtime/exec-env.h"
#include "runtime/fragment-instance-state.h"
#include "runtime/debug-options.h"
#include "runtime/client-cache.h"
#include "runtime/backend-client.h"
#include "runtime/coordinator-filter-state.h"
#include "util/uid-util.h"
#include "util/network-util.h"
#include "util/counting-barrier.h"
#include "gen-cpp/ImpalaInternalService_constants.h"

#include "common/names.h"

using namespace impala;
using namespace rapidjson;
namespace accumulators = boost::accumulators;

Coordinator::BackendState::BackendState(
    const Coordinator& coord, int state_idx, TRuntimeFilterMode::type filter_mode)
  : coord_(coord),
    state_idx_(state_idx),
    filter_mode_(filter_mode) {
}

void Coordinator::BackendState::Init(
    const BackendExecParams& exec_params,
    const vector<FragmentStats*>& fragment_stats, ObjectPool* obj_pool) {
  backend_exec_params_ = &exec_params;
  host_ = backend_exec_params_->instance_params[0]->host;
  num_remaining_instances_ = backend_exec_params_->instance_params.size();

  // populate instance_stats_map_ and install instance
  // profiles as child profiles in fragment_stats' profile
  int prev_fragment_idx = -1;
  for (const FInstanceExecParams* instance_params:
       backend_exec_params_->instance_params) {
    DCHECK(host_ == instance_params->host);  // all hosts must be the same
    int fragment_idx = instance_params->fragment().idx;
    DCHECK_LT(fragment_idx, fragment_stats.size());
    if (prev_fragment_idx != -1 && fragment_idx != prev_fragment_idx) {
      // all instances of a fragment are contiguous
      DCHECK_EQ(fragments_.count(fragment_idx), 0);
      prev_fragment_idx = fragment_idx;
    }
    fragments_.insert(fragment_idx);

    instance_stats_map_.emplace(
        GetInstanceIdx(instance_params->instance_id),
        obj_pool->Add(
          new InstanceStats(*instance_params, fragment_stats[fragment_idx], obj_pool)));
  }
}

void Coordinator::BackendState::SetRpcParams(const DebugOptions& debug_options,
    const FilterRoutingTable& filter_routing_table,
    TExecQueryFInstancesParams* rpc_params) {
  rpc_params->__set_protocol_version(ImpalaInternalServiceVersion::V1);
  rpc_params->__set_coord_state_idx(state_idx_);
  rpc_params->__set_min_mem_reservation_bytes(
      backend_exec_params_->min_mem_reservation_bytes);
  rpc_params->__set_initial_mem_reservation_total_claims(
      backend_exec_params_->initial_mem_reservation_total_claims);

  // set fragment_ctxs and fragment_instance_ctxs
  rpc_params->__isset.fragment_ctxs = true;
  rpc_params->__isset.fragment_instance_ctxs = true;
  rpc_params->fragment_instance_ctxs.resize(backend_exec_params_->instance_params.size());
  for (int i = 0; i < backend_exec_params_->instance_params.size(); ++i) {
    TPlanFragmentInstanceCtx& instance_ctx = rpc_params->fragment_instance_ctxs[i];
    const FInstanceExecParams& params = *backend_exec_params_->instance_params[i];
    int fragment_idx = params.fragment_exec_params.fragment.idx;

    // add a TPlanFragmentCtx, if we don't already have it
    if (rpc_params->fragment_ctxs.empty()
        || rpc_params->fragment_ctxs.back().fragment.idx != fragment_idx) {
      rpc_params->fragment_ctxs.emplace_back();
      TPlanFragmentCtx& fragment_ctx = rpc_params->fragment_ctxs.back();
      fragment_ctx.__set_fragment(params.fragment_exec_params.fragment);
      fragment_ctx.__set_destinations(params.fragment_exec_params.destinations);
    }

    instance_ctx.fragment_idx = fragment_idx;
    instance_ctx.fragment_instance_id = params.instance_id;
    instance_ctx.per_fragment_instance_idx = params.per_fragment_instance_idx;
    instance_ctx.__set_per_node_scan_ranges(params.per_node_scan_ranges);
    instance_ctx.__set_per_exch_num_senders(
        params.fragment_exec_params.per_exch_num_senders);
    instance_ctx.__set_sender_id(params.sender_id);
    if (debug_options.enabled()
        && (debug_options.instance_idx() == -1
            || debug_options.instance_idx() == GetInstanceIdx(params.instance_id))) {
      instance_ctx.__set_debug_options(debug_options.ToThrift());
    }

    if (filter_mode_ == TRuntimeFilterMode::OFF) continue;

    // Remove filters that weren't selected during filter routing table construction.
    // TODO: do this more efficiently, we're looping over the entire plan for each
    // instance separately
    DCHECK_EQ(rpc_params->query_ctx.client_request.query_options.mt_dop, 0);
    int instance_idx = GetInstanceIdx(params.instance_id);
    for (TPlanNode& plan_node: rpc_params->fragment_ctxs.back().fragment.plan.nodes) {
      if (!plan_node.__isset.hash_join_node) continue;
      if (!plan_node.__isset.runtime_filters) continue;

      vector<TRuntimeFilterDesc> required_filters;
      for (const TRuntimeFilterDesc& desc: plan_node.runtime_filters) {
        FilterRoutingTable::const_iterator filter_it =
            filter_routing_table.find(desc.filter_id);
        // filter was dropped in Coordinator::InitFilterRoutingTable()
        if (filter_it == filter_routing_table.end()) continue;
        const FilterState& f = filter_it->second;
        if (f.src_fragment_instance_idxs().find(instance_idx)
            == f.src_fragment_instance_idxs().end()) {
          DCHECK(desc.is_broadcast_join);
          continue;
        }
        // We don't need a target-side check here, because a filter is either sent to
        // all its targets or none, and the none case is handled by checking if the
        // filter is in the routing table.
        required_filters.push_back(desc);
      }
      plan_node.__set_runtime_filters(required_filters);
    }
  }
}

void Coordinator::BackendState::Exec(
    const DebugOptions& debug_options,
    const FilterRoutingTable& filter_routing_table,
    CountingBarrier* exec_complete_barrier) {
  NotifyBarrierOnExit notifier(exec_complete_barrier);
  TExecQueryFInstancesParams rpc_params;
  rpc_params.__set_query_ctx(query_ctx());
  SetRpcParams(debug_options, filter_routing_table, &rpc_params);
  VLOG_FILE << "making rpc: ExecQueryFInstances"
      << " host=" << TNetworkAddressToString(impalad_address()) << " query_id="
      << PrintId(query_id());

  // guard against concurrent UpdateBackendExecStatus() that may arrive after RPC returns
  lock_guard<mutex> l(lock_);
  int64_t start = MonotonicMillis();

  ImpalaBackendConnection backend_client(
      ExecEnv::GetInstance()->impalad_client_cache(), impalad_address(), &status_);
  if (!status_.ok()) return;

  TExecQueryFInstancesResult thrift_result;
  Status rpc_status = backend_client.DoRpc(
      &ImpalaBackendClient::ExecQueryFInstances, rpc_params, &thrift_result);
  rpc_sent_ = true;
  rpc_latency_ = MonotonicMillis() - start;

  const string ERR_TEMPLATE =
      "ExecQueryFInstances rpc query_id=$0 failed: $1";

  if (!rpc_status.ok()) {
    const string& err_msg =
        Substitute(ERR_TEMPLATE, PrintId(query_id()), rpc_status.msg().msg());
    VLOG_QUERY << err_msg;
    status_ = Status::Expected(err_msg);
    return;
  }

  Status exec_status = Status(thrift_result.status);
  if (!exec_status.ok()) {
    const string& err_msg = Substitute(ERR_TEMPLATE, PrintId(query_id()),
        exec_status.msg().GetFullMessageDetails());
    VLOG_QUERY << err_msg;
    status_ = Status::Expected(err_msg);
    return;
  }

  for (const auto& entry: instance_stats_map_) entry.second->stopwatch_.Start();
  VLOG_FILE << "rpc succeeded: ExecQueryFInstances query_id=" << PrintId(query_id());
}

Status Coordinator::BackendState::GetStatus(bool* is_fragment_failure,
    TUniqueId* failed_instance_id) {
  lock_guard<mutex> l(lock_);
  DCHECK_EQ(is_fragment_failure == nullptr, failed_instance_id == nullptr);
  if (!status_.ok() && failed_instance_id != nullptr) {
    *is_fragment_failure = is_fragment_failure_;
    *failed_instance_id = failed_instance_id_;
  }
  return status_;
}

Coordinator::ResourceUtilization Coordinator::BackendState::ComputeResourceUtilization() {
  lock_guard<mutex> l(lock_);
  return ComputeResourceUtilizationLocked();
}

Coordinator::ResourceUtilization
Coordinator::BackendState::ComputeResourceUtilizationLocked() {
  ResourceUtilization result;
  for (const auto& entry : instance_stats_map_) {
    RuntimeProfile* profile = entry.second->profile_;
    ResourceUtilization instance_utilization;
    // Update resource utilization and apply delta.
    RuntimeProfile::Counter* user_time = profile->GetCounter("TotalThreadsUserTime");
    if (user_time != nullptr) instance_utilization.cpu_user_ns = user_time->value();

    RuntimeProfile::Counter* system_time = profile->GetCounter("TotalThreadsSysTime");
    if (system_time != nullptr) instance_utilization.cpu_sys_ns = system_time->value();

    for (RuntimeProfile::Counter* c : entry.second->bytes_read_counters_) {
      instance_utilization.bytes_read += c->value();
    }

    RuntimeProfile::Counter* peak_mem =
        profile->GetCounter(FragmentInstanceState::PER_HOST_PEAK_MEM_COUNTER);
    if (peak_mem != nullptr)
      instance_utilization.peak_per_host_mem_consumption = peak_mem->value();
    result.Merge(instance_utilization);
  }
  return result;
}

void Coordinator::BackendState::MergeErrorLog(ErrorLogMap* merged) {
  lock_guard<mutex> l(lock_);
  if (error_log_.size() > 0)  MergeErrorMaps(error_log_, merged);
}

void Coordinator::BackendState::LogFirstInProgress(
    std::vector<Coordinator::BackendState*> backend_states) {
  for (Coordinator::BackendState* backend_state : backend_states) {
    lock_guard<mutex> l(backend_state->lock_);
    if (!backend_state->IsDone()) {
      VLOG_QUERY << "query_id=" << PrintId(backend_state->query_id())
                 << ": first in-progress backend: "
                 << TNetworkAddressToString(backend_state->impalad_address());
      break;
    }
  }
}

inline bool Coordinator::BackendState::IsDone() const {
  return num_remaining_instances_ == 0 || !status_.ok();
}

bool Coordinator::BackendState::ApplyExecStatusReport(
    const TReportExecStatusParams& backend_exec_status, ExecSummary* exec_summary,
    ProgressUpdater* scan_range_progress) {
  lock_guard<SpinLock> l1(exec_summary->lock);
  lock_guard<mutex> l2(lock_);
  last_report_time_ms_ = MonotonicMillis();

  // If this backend completed previously, don't apply the update.
  if (IsDone()) return false;
  for (const TFragmentInstanceExecStatus& instance_exec_status:
      backend_exec_status.instance_exec_status) {
    Status instance_status(instance_exec_status.status);
    int instance_idx = GetInstanceIdx(instance_exec_status.fragment_instance_id);
    DCHECK_EQ(instance_stats_map_.count(instance_idx), 1);
    InstanceStats* instance_stats = instance_stats_map_[instance_idx];
    DCHECK(instance_stats->exec_params_.instance_id ==
        instance_exec_status.fragment_instance_id);
    // Ignore duplicate or out-of-order messages.
    if (instance_stats->done_) continue;

    instance_stats->Update(instance_exec_status, exec_summary, scan_range_progress);

    // If a query is aborted due to an error encountered by a single fragment instance,
    // all other fragment instances will report a cancelled status; make sure not to mask
    // the original error status.
    if (!instance_status.ok() && (status_.ok() || status_.IsCancelled())) {
      status_ = instance_status;
      failed_instance_id_ = instance_exec_status.fragment_instance_id;
      is_fragment_failure_ = true;
    }
    DCHECK_GT(num_remaining_instances_, 0);
    if (instance_exec_status.done) {
      DCHECK(!instance_stats->done_);
      instance_stats->done_ = true;
      --num_remaining_instances_;
    }

    // TODO: clean up the ReportQuerySummary() mess
    if (status_.ok()) {
      // We can't update this backend's profile if ReportQuerySummary() is running,
      // because it depends on all profiles not changing during its execution (when it
      // calls SortChildren()). ReportQuerySummary() only gets called after
      // WaitForBackendCompletion() returns or at the end of CancelFragmentInstances().
      // WaitForBackendCompletion() only returns after all backends have completed (in
      // which case we wouldn't be in this function), or when there's an error, in which
      // case CancelFragmentInstances() is called. CancelFragmentInstances sets all
      // exec_state's statuses to cancelled.
      // TODO: We're losing this profile information. Call ReportQuerySummary only after
      // all backends have completed.
    }
  }

  // status_ has incorporated the status from all fragment instances. If the overall
  // backend status is not OK, but no specific fragment instance reported an error, then
  // this is a general backend error. Incorporate the general error into status_.
  Status overall_backend_status(backend_exec_status.status);
  if (!overall_backend_status.ok() && (status_.ok() || status_.IsCancelled())) {
    status_ = overall_backend_status;
  }

  // Log messages aggregated by type
  if (backend_exec_status.__isset.error_log && backend_exec_status.error_log.size() > 0) {
    // Append the log messages from each update with the global state of the query
    // execution
    MergeErrorMaps(backend_exec_status.error_log, &error_log_);
    VLOG_FILE << "host=" << TNetworkAddressToString(host_) << " error log: " <<
        PrintErrorMapToString(error_log_);
  }

  // TODO: keep backend-wide stopwatch?
  return IsDone();
}

void Coordinator::BackendState::UpdateExecStats(
    const vector<FragmentStats*>& fragment_stats) {
  lock_guard<mutex> l(lock_);
  for (const auto& entry: instance_stats_map_) {
    const InstanceStats& instance_stats = *entry.second;
    int fragment_idx = instance_stats.exec_params_.fragment().idx;
    DCHECK_LT(fragment_idx, fragment_stats.size());
    FragmentStats* f = fragment_stats[fragment_idx];
    int64_t completion_time = instance_stats.stopwatch_.ElapsedTime();
    f->completion_times_(completion_time);
    if (completion_time > 0) {
      f->rates_(instance_stats.total_split_size_
        / (completion_time / 1000.0 / 1000.0 / 1000.0));
    }
    f->avg_profile_->UpdateAverage(instance_stats.profile_);
  }
}

bool Coordinator::BackendState::Cancel() {
  lock_guard<mutex> l(lock_);

  // Nothing to cancel if the exec rpc was not sent
  if (!rpc_sent_) return false;

  // don't cancel if it already finished (for any reason)
  if (IsDone()) return false;

  /// If the status is not OK, we still try to cancel - !OK status might mean
  /// communication failure between backend and coordinator, but fragment
  /// instances might still be running.

  // set an error status to make sure we only cancel this once
  if (status_.ok()) status_ = Status::CANCELLED;

  TCancelQueryFInstancesParams params;
  params.protocol_version = ImpalaInternalServiceVersion::V1;
  params.__set_query_id(query_id());
  TCancelQueryFInstancesResult dummy;
  VLOG_QUERY << "sending CancelQueryFInstances rpc for query_id=" << PrintId(query_id()) <<
      " backend=" << TNetworkAddressToString(impalad_address());

  Status rpc_status;
  Status client_status;
  // Try to send the RPC 3 times before failing.
  for (int i = 0; i < 3; ++i) {
    ImpalaBackendConnection backend_client(ExecEnv::GetInstance()->impalad_client_cache(),
        impalad_address(), &client_status);
    if (!client_status.ok()) continue;

    rpc_status = DebugAction(query_ctx().client_request.query_options,
        "COORD_CANCEL_QUERY_FINSTANCES_RPC");
    if (!rpc_status.ok()) continue;

    // The return value 'dummy' is ignored as it's only set if the fragment
    // instance cannot be found in the backend. The fragment instances of a query
    // can all be cancelled locally in a backend due to RPC failure to
    // coordinator. In which case, the query state can be gone already.
    rpc_status = backend_client.DoRpc(
        &ImpalaBackendClient::CancelQueryFInstances, params, &dummy);
    if (rpc_status.ok()) break;
  }
  if (!client_status.ok()) {
    status_.MergeStatus(client_status);
    VLOG_QUERY << "CancelQueryFInstances query_id= " << PrintId(query_id())
               << " failed to connect to " << TNetworkAddressToString(impalad_address())
               << " :" << client_status.msg().msg();
    return true;
  }
  if (!rpc_status.ok()) {
    status_.MergeStatus(rpc_status);
    VLOG_QUERY << "CancelQueryFInstances query_id= " << PrintId(query_id())
               << " rpc to " << TNetworkAddressToString(impalad_address())
               << " failed: " << rpc_status.msg().msg();
    return true;
  }
  return true;
}

void Coordinator::BackendState::PublishFilter(const TPublishFilterParams& rpc_params) {
  DCHECK(rpc_params.dst_query_id == query_id());
  {
    // If the backend is already done, it's not waiting for this filter, so we skip
    // sending it in this case.
    lock_guard<mutex> l(lock_);
    if (IsDone()) return;
  }

  if (fragments_.count(rpc_params.dst_fragment_idx) == 0) return;
  Status status;
  ImpalaBackendConnection backend_client(
      ExecEnv::GetInstance()->impalad_client_cache(), host_, &status);
  if (!status.ok()) return;
  TPublishFilterResult res;
  status = backend_client.DoRpc(&ImpalaBackendClient::PublishFilter, rpc_params, &res);
  if (!status.ok()) {
    LOG(WARNING) << "Error publishing filter, continuing..." << status.GetDetail();
  }
}

Coordinator::BackendState::InstanceStats::InstanceStats(
    const FInstanceExecParams& exec_params, FragmentStats* fragment_stats,
    ObjectPool* obj_pool)
  : exec_params_(exec_params),
    profile_(nullptr) {
  const string& profile_name = Substitute("Instance $0 (host=$1)",
      PrintId(exec_params.instance_id), TNetworkAddressToString(exec_params.host));
  profile_ = RuntimeProfile::Create(obj_pool, profile_name);
  fragment_stats->root_profile()->AddChild(profile_);

  // add total split size to fragment_stats->bytes_assigned()
  for (const PerNodeScanRanges::value_type& entry: exec_params_.per_node_scan_ranges) {
    for (const TScanRangeParams& scan_range_params: entry.second) {
      if (!scan_range_params.scan_range.__isset.hdfs_file_split) continue;
      total_split_size_ += scan_range_params.scan_range.hdfs_file_split.length;
    }
  }
  (*fragment_stats->bytes_assigned())(total_split_size_);
}

void Coordinator::BackendState::InstanceStats::InitCounters() {
  vector<RuntimeProfile*> children;
  profile_->GetAllChildren(&children);
  for (RuntimeProfile* p: children) {
    PlanNodeId id = ExecNode::GetNodeIdFromProfile(p);
    // This profile is not for an exec node.
    if (id == g_ImpalaInternalService_constants.INVALID_PLAN_NODE_ID) continue;

    RuntimeProfile::Counter* c =
        p->GetCounter(ScanNode::SCAN_RANGES_COMPLETE_COUNTER);
    if (c != nullptr) scan_ranges_complete_counters_.push_back(c);

    RuntimeProfile::Counter* bytes_read =
        p->GetCounter(ScanNode::BYTES_READ_COUNTER);
    if (bytes_read != nullptr) bytes_read_counters_.push_back(bytes_read);
  }

}

void Coordinator::BackendState::InstanceStats::Update(
    const TFragmentInstanceExecStatus& exec_status, ExecSummary* exec_summary,
    ProgressUpdater* scan_range_progress) {
  last_report_time_ms_ = MonotonicMillis();
  if (exec_status.done) stopwatch_.Stop();
  profile_->Update(exec_status.profile);
  if (!profile_created_) {
    profile_created_ = true;
    InitCounters();
  }
  profile_->ComputeTimeInProfile();

  // update exec_summary
  // TODO: why do this every time we get an updated instance profile?
  vector<RuntimeProfile*> children;
  profile_->GetAllChildren(&children);

  TExecSummary& thrift_exec_summary = exec_summary->thrift_exec_summary;
  for (RuntimeProfile* child: children) {
    int node_id = ExecNode::GetNodeIdFromProfile(child);
    if (node_id == -1) continue;

    // TODO: create plan_node_id_to_summary_map_
    TPlanNodeExecSummary& node_exec_summary =
        thrift_exec_summary.nodes[exec_summary->node_id_to_idx_map[node_id]];
    int per_fragment_instance_idx = exec_params_.per_fragment_instance_idx;
    DCHECK_LT(per_fragment_instance_idx, node_exec_summary.exec_stats.size())
        << " node_id=" << node_id << " instance_id=" << PrintId(exec_params_.instance_id)
        << " fragment_idx=" << exec_params_.fragment().idx;
    TExecStats& instance_stats =
        node_exec_summary.exec_stats[per_fragment_instance_idx];

    RuntimeProfile::Counter* rows_counter = child->GetCounter("RowsReturned");
    RuntimeProfile::Counter* mem_counter = child->GetCounter("PeakMemoryUsage");
    if (rows_counter != nullptr) instance_stats.__set_cardinality(rows_counter->value());
    if (mem_counter != nullptr) instance_stats.__set_memory_used(mem_counter->value());
    instance_stats.__set_latency_ns(child->local_time());
    // TODO: track interesting per-node metrics
    node_exec_summary.__isset.exec_stats = true;
  }

  // determine newly-completed scan ranges and update scan_range_progress
  int64_t total = 0;
  for (RuntimeProfile::Counter* c: scan_ranges_complete_counters_) total += c->value();
  int64_t delta = total - total_ranges_complete_;
  total_ranges_complete_ = total;
  scan_range_progress->Update(delta);

  // extract the current execution state of this instance
  current_state_ = exec_status.current_state;
}

void Coordinator::BackendState::InstanceStats::ToJson(Value* value, Document* document) {
  Value instance_id_val(PrintId(exec_params_.instance_id).c_str(),
      document->GetAllocator());
  value->AddMember("instance_id", instance_id_val, document->GetAllocator());

  // We send 'done' explicitly so we don't have to infer it by comparison with a string
  // constant in the debug page JS code.
  value->AddMember("done", done_, document->GetAllocator());

  Value state_val(FragmentInstanceState::ExecStateToString(current_state_).c_str(),
      document->GetAllocator());
  value->AddMember("current_state", state_val, document->GetAllocator());

  Value fragment_name_val(exec_params_.fragment().display_name.c_str(),
      document->GetAllocator());
  value->AddMember("fragment_name", fragment_name_val, document->GetAllocator());

  value->AddMember("first_status_update_received", last_report_time_ms_ > 0,
      document->GetAllocator());
  value->AddMember("time_since_last_heard_from", MonotonicMillis() - last_report_time_ms_,
      document->GetAllocator());
}

Coordinator::FragmentStats::FragmentStats(const string& avg_profile_name,
    const string& root_profile_name, int num_instances, ObjectPool* obj_pool)
  : avg_profile_(RuntimeProfile::Create(obj_pool, avg_profile_name, true)),
    root_profile_(RuntimeProfile::Create(obj_pool, root_profile_name)),
    num_instances_(num_instances) {
}

void Coordinator::FragmentStats::AddSplitStats() {
  double min = accumulators::min(bytes_assigned_);
  double max = accumulators::max(bytes_assigned_);
  double mean = accumulators::mean(bytes_assigned_);
  double stddev = sqrt(accumulators::variance(bytes_assigned_));
  stringstream ss;
  ss << " min: " << PrettyPrinter::Print(min, TUnit::BYTES)
    << ", max: " << PrettyPrinter::Print(max, TUnit::BYTES)
    << ", avg: " << PrettyPrinter::Print(mean, TUnit::BYTES)
    << ", stddev: " << PrettyPrinter::Print(stddev, TUnit::BYTES);
  avg_profile_->AddInfoString("split sizes", ss.str());
}

void Coordinator::FragmentStats::AddExecStats() {
  root_profile_->SortChildrenByTotalTime();
  stringstream times_label;
  times_label
    << "min:" << PrettyPrinter::Print(
        accumulators::min(completion_times_), TUnit::TIME_NS)
    << "  max:" << PrettyPrinter::Print(
        accumulators::max(completion_times_), TUnit::TIME_NS)
    << "  mean: " << PrettyPrinter::Print(
        accumulators::mean(completion_times_), TUnit::TIME_NS)
    << "  stddev:" << PrettyPrinter::Print(
        sqrt(accumulators::variance(completion_times_)), TUnit::TIME_NS);

  stringstream rates_label;
  rates_label
    << "min:" << PrettyPrinter::Print(
        accumulators::min(rates_), TUnit::BYTES_PER_SECOND)
    << "  max:" << PrettyPrinter::Print(
        accumulators::max(rates_), TUnit::BYTES_PER_SECOND)
    << "  mean:" << PrettyPrinter::Print(
        accumulators::mean(rates_), TUnit::BYTES_PER_SECOND)
    << "  stddev:" << PrettyPrinter::Print(
        sqrt(accumulators::variance(rates_)), TUnit::BYTES_PER_SECOND);

  // why plural?
  avg_profile_->AddInfoString("completion times", times_label.str());
  // why plural?
  avg_profile_->AddInfoString("execution rates", rates_label.str());
  avg_profile_->AddInfoString("num instances", lexical_cast<string>(num_instances_));
}

void Coordinator::BackendState::ToJson(Value* value, Document* document) {
  lock_guard<mutex> l(lock_);
  ResourceUtilization resource_utilization = ComputeResourceUtilizationLocked();
  value->AddMember("num_instances", fragments_.size(), document->GetAllocator());
  value->AddMember("done", IsDone(), document->GetAllocator());
  value->AddMember("peak_per_host_mem_consumption",
      resource_utilization.peak_per_host_mem_consumption, document->GetAllocator());
  value->AddMember("bytes_read", resource_utilization.bytes_read,
      document->GetAllocator());
  value->AddMember("cpu_user_s", resource_utilization.cpu_user_ns / 1e9,
      document->GetAllocator());
  value->AddMember("cpu_sys_s", resource_utilization.cpu_sys_ns / 1e9,
      document->GetAllocator());

  string host = TNetworkAddressToString(impalad_address());
  Value val(host.c_str(), document->GetAllocator());
  value->AddMember("host", val, document->GetAllocator());

  value->AddMember("rpc_latency", rpc_latency(), document->GetAllocator());
  value->AddMember("time_since_last_heard_from", MonotonicMillis() - last_report_time_ms_,
      document->GetAllocator());

  string status_str = status_.ok() ? "OK" : status_.GetDetail();
  Value status_val(status_str.c_str(), document->GetAllocator());
  value->AddMember("status", status_val, document->GetAllocator());

  value->AddMember(
      "num_remaining_instances", num_remaining_instances_, document->GetAllocator());
}

void Coordinator::BackendState::InstanceStatsToJson(Value* value, Document* document) {
  Value instance_stats(kArrayType);
  {
    lock_guard<mutex> l(lock_);
    for (const auto& elem : instance_stats_map_) {
      Value val(kObjectType);
      elem.second->ToJson(&val, document);
      instance_stats.PushBack(val, document->GetAllocator());
    }
  }
  value->AddMember("instance_stats", instance_stats, document->GetAllocator());

  // impalad_address is not protected by lock_. The lifetime of the backend state is
  // protected by Coordinator::lock_.
  Value val(TNetworkAddressToString(impalad_address()).c_str(), document->GetAllocator());
  value->AddMember("host", val, document->GetAllocator());
}
