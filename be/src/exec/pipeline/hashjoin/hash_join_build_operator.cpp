// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/pipeline/hashjoin/hash_join_build_operator.h"

#include <numeric>
#include <utility>

#include "exec/hash_joiner.h"
#include "exec/pipeline/hashjoin/hash_joiner_factory.h"
#include "exec/pipeline/query_context.h"
#include "exec/pipeline/runtime_filter_types.h"
#include "exprs/runtime_filter_bank.h"
#include "runtime/current_thread.h"
#include "runtime/runtime_filter_worker.h"
#include "util/race_detect.h"

namespace starrocks::pipeline {

HashJoinBuildOperator::HashJoinBuildOperator(OperatorFactory* factory, int32_t id, const string& name,
                                             int32_t plan_node_id, int32_t driver_sequence, HashJoinerPtr join_builder,
                                             PartialRuntimeFilterMerger* partial_rf_merger,
                                             const TJoinDistributionMode::type distribution_mode)
        : Operator(factory, id, name, plan_node_id, false, driver_sequence),
          _join_builder(std::move(join_builder)),
          _partial_rf_merger(partial_rf_merger),
          _distribution_mode(distribution_mode) {}

Status HashJoinBuildOperator::push_chunk(RuntimeState* state, const ChunkPtr& chunk) {
    return _join_builder->append_chunk_to_ht(chunk);
}

Status HashJoinBuildOperator::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(Operator::prepare(state));

    _partial_rf_merger->incr_builder();

    // For prober.
    // HashJoinProbeOperator may be instantiated lazily, so join_builder is ref here
    // and unref when all the probers are finished in join_builder->decr_prober.
    _join_builder->ref();
    // For builder.
    _join_builder->ref();

    RETURN_IF_ERROR(_join_builder->prepare_builder(state, _unique_metrics.get()));
    _join_builder->attach_build_observer(state, observer());

    return Status::OK();
}
void HashJoinBuildOperator::close(RuntimeState* state) {
    COUNTER_SET(_join_builder->build_metrics().hash_table_memory_usage,
                _join_builder->hash_join_builder()->ht_mem_usage());
    _join_builder->unref(state);

    Operator::close(state);
}

StatusOr<ChunkPtr> HashJoinBuildOperator::pull_chunk(RuntimeState* state) {
    const char* msg = "pull_chunk not supported in HashJoinBuildOperator";
    CHECK(false) << msg;
    return Status::NotSupported(msg);
}

size_t HashJoinBuildOperator::output_amplification_factor() const {
    if (_avg_keys_per_bucket > 0) {
        return _avg_keys_per_bucket;
    }
    _avg_keys_per_bucket = _join_builder->avg_keys_per_bucket();
    _avg_keys_per_bucket = std::max<size_t>(_avg_keys_per_bucket, 1);

    return _avg_keys_per_bucket;
}

Status HashJoinBuildOperator::set_finishing(RuntimeState* state) {
    ONCE_DETECT(_set_finishing_once);
    // notify probe side
    auto notify = _join_builder->defer_notify_probe();
    DeferOp op([this]() { _is_finished = true; });

    if (state->is_cancelled()) {
        return Status::Cancelled("runtime state is cancelled");
    }
    RETURN_IF_ERROR(_join_builder->build_ht(state));

    size_t merger_index = _driver_sequence;
    // Broadcast Join only has one build operator.
    DCHECK(_distribution_mode != TJoinDistributionMode::BROADCAST || _driver_sequence == 0);
    {
        SCOPED_TIMER(_join_builder->build_metrics().build_runtime_filter_timer);
        RETURN_IF_ERROR(_join_builder->create_runtime_filters(state));
    }

    auto ht_row_count = _join_builder->get_ht_row_count();
    auto& partial_in_filters = _join_builder->get_runtime_in_filters();
    auto& partial_bloom_filter_build_params = _join_builder->get_runtime_bloom_filter_build_params();
    auto& partial_bloom_filters = _join_builder->get_runtime_bloom_filters();

    // for skew join's boradcast site, we need key column for runtime filter
    bool is_skew_broadcast_join =
            _join_builder->is_skew_join() && _distribution_mode == TJoinDistributionMode::BROADCAST;
    std::vector<Columns> keyColumns;
    std::vector<bool> null_safe;
    std::vector<TypeDescriptor> type_descs;
    if (is_skew_broadcast_join) {
        keyColumns.reserve(partial_bloom_filter_build_params.size());
        null_safe.reserve(partial_bloom_filter_build_params.size());
        type_descs.reserve(partial_bloom_filter_build_params.size());
        for (auto& param : partial_bloom_filter_build_params) {
            if (UNLIKELY(!param.has_value())) {
                return Status::InternalError("skew join build rf failed");
            }
            keyColumns.emplace_back(param.value().columns);
            null_safe.emplace_back(param.value().eq_null);
            type_descs.emplace_back(param.value()._type_descriptor);
        }
    }

    auto mem_tracker = state->query_ctx()->mem_tracker();
    SCOPED_THREAD_LOCAL_MEM_TRACKER_SETTER(mem_tracker.get());

    // retain string-typed key columns to avoid premature de-allocation when both probe side and build side
    // PipelineDrivers finalization before in-filers is merged.
    ((HashJoinBuildOperatorFactory*)_factory)
            ->retain_string_key_columns(_driver_sequence, _join_builder->string_key_columns());

    if (partial_bloom_filters.size() != partial_bloom_filter_build_params.size()) {
        // if in short-circuit mode, phase is EOS. partial_bloom_filter_build_params is empty.
        DCHECK(_join_builder->is_done());
    }

    // push colocate partial runtime filter
    bool is_colocate_runtime_filter = runtime_filter_hub()->is_colocate_runtime_filters(_plan_node_id);
    if (is_colocate_runtime_filter) {
        // init local colocate in/bloom filters
        RuntimeInFilterList in_filter_lists(partial_in_filters.begin(), partial_in_filters.end());
        if (partial_bloom_filters.size() == partial_bloom_filter_build_params.size()) {
            for (size_t i = 0; i < partial_bloom_filters.size(); ++i) {
                if (partial_bloom_filter_build_params[i].has_value()) {
                    partial_bloom_filters[i]->set_or_concat(partial_bloom_filter_build_params[i]->runtime_filter.get(),
                                                            _driver_sequence);
                }
            }
        }
        RuntimeMembershipFilterList bloom_filters(partial_bloom_filters.begin(), partial_bloom_filters.end());
        runtime_filter_hub()->set_collector(_plan_node_id, _driver_sequence,
                                            std::make_unique<RuntimeFilterCollector>(in_filter_lists));
        state->runtime_filter_port()->publish_local_colocate_filters(bloom_filters);

    } else {
        // add partial filters generated by this HashJoinBuildOperator to PartialRuntimeFilterMerger to merge into a
        // total one.
        bool all_build_merged = false;
        {
            SCOPED_TIMER(_join_builder->build_metrics().build_runtime_filter_timer);
            auto status = _partial_rf_merger->add_partial_filters(
                    merger_index, ht_row_count, std::move(partial_in_filters),
                    std::move(partial_bloom_filter_build_params), std::move(partial_bloom_filters));
            ASSIGN_OR_RETURN(all_build_merged, status);
        }

        if (all_build_merged) {
            auto&& in_filters = _partial_rf_merger->get_total_in_filters();
            auto&& bloom_filters = _partial_rf_merger->get_total_bloom_filters();

            {
                size_t total_bf_bytes =
                        std::accumulate(bloom_filters.begin(), bloom_filters.end(), 0ull,
                                        [](size_t total, RuntimeFilterBuildDescriptor* desc) -> size_t {
                                            if (desc->runtime_filter() == nullptr) {
                                                return total;
                                            }
                                            return desc->runtime_filter()->get_membership_filter()->bf_alloc_size();
                                        });
                COUNTER_UPDATE(_join_builder->build_metrics().partial_runtime_bloom_filter_bytes, total_bf_bytes);
            }

            // publish runtime bloom
            if (is_skew_broadcast_join) {
                // for skew join's broadcast join, we only publish local in/bloom runtime filter, and send key columns to rf coordinator
                // and rf coordinator will merge key column with shuffle join's bloom filter instance
                state->runtime_filter_port()->publish_runtime_filters_for_skew_broadcast_join(bloom_filters, keyColumns,
                                                                                              null_safe, type_descs);
            } else {
                state->runtime_filter_port()->publish_runtime_filters(bloom_filters);
            }

            // move runtime filters into RuntimeFilterHub.
            runtime_filter_hub()->set_collector(_plan_node_id,
                                                std::make_unique<RuntimeFilterCollector>(std::move(in_filters)));
        }
    }

    _join_builder->enter_probe_phase();

    return Status::OK();
}

bool HashJoinBuildOperator::is_finished() const {
    return _is_finished || _join_builder->is_finished();
}

HashJoinBuildOperatorFactory::HashJoinBuildOperatorFactory(
        int32_t id, int32_t plan_node_id, HashJoinerFactoryPtr hash_joiner_factory,
        std::unique_ptr<PartialRuntimeFilterMerger>&& partial_rf_merger,
        const TJoinDistributionMode::type distribution_mode, SpillProcessChannelFactoryPtr spill_channel_factory)
        : OperatorFactory(id, "hash_join_build", plan_node_id),
          _hash_joiner_factory(std::move(hash_joiner_factory)),
          _partial_rf_merger(std::move(partial_rf_merger)),
          _distribution_mode(distribution_mode),
          _spill_channel_factory(std::move(spill_channel_factory)) {}

Status HashJoinBuildOperatorFactory::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(OperatorFactory::prepare(state));
    return _hash_joiner_factory->prepare(state);
}

void HashJoinBuildOperatorFactory::close(RuntimeState* state) {
    _hash_joiner_factory->close(state);
    OperatorFactory::close(state);
}

OperatorPtr HashJoinBuildOperatorFactory::create(int32_t dop, int32_t driver_sequence) {
    if (_string_key_columns.empty()) {
        _string_key_columns.resize(dop);
    }

    return std::make_shared<HashJoinBuildOperator>(this, _id, _name, _plan_node_id, driver_sequence,
                                                   _hash_joiner_factory->create_builder(dop, driver_sequence),
                                                   _partial_rf_merger.get(), _distribution_mode);
}

void HashJoinBuildOperatorFactory::retain_string_key_columns(int32_t driver_sequence, Columns&& columns) {
    _string_key_columns[driver_sequence] = std::move(columns);
}
} // namespace starrocks::pipeline
