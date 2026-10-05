#include <Interpreters/QueryJoinLog.h>

#include <Columns/ColumnLowCardinality.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Core/Settings.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Common/CurrentThread.h>
#include <Common/DateLUT.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadStatus.h>
#include <Common/logger_useful.h>
#include <base/getFQDNOrHostName.h>

#include <chrono>


namespace ProfileEvents
{
    extern const Event UserTimeMicroseconds;
    extern const Event SystemTimeMicroseconds;
    extern const Event OSCPUWaitMicroseconds;
    extern const Event RealTimeMicroseconds;
    extern const Event OSIOWaitMicroseconds;
    extern const Event DiskReadElapsedMicroseconds;
    extern const Event DiskWriteElapsedMicroseconds;
    extern const Event ReadBufferFromFileDescriptorRead;
    extern const Event ReadBufferFromFileDescriptorReadBytes;
    extern const Event Seek;
    extern const Event FileOpen;
    extern const Event PageCacheHits;
    extern const Event PageCacheMisses;
    extern const Event S3ReadMicroseconds;
    extern const Event ExternalJoinWritePart;
    extern const Event ExternalJoinCompressedBytes;
}

namespace DB
{
namespace Setting
{
    extern const SettingsString log_comment;
    extern const SettingsBool distributed_aggregation_memory_efficient;
}

namespace
{

ASTPtr codecZSTD(UInt64 level)
{
    return makeASTFunction("CODEC",
        makeASTFunction("ZSTD", make_intrusive<ASTLiteral>(level)));
}

ASTPtr codecDeltaZSTD(UInt64 delta_bytes)
{
    return makeASTFunction("CODEC",
        makeASTFunction("Delta", make_intrusive<ASTLiteral>(delta_bytes)),
        makeASTFunction("ZSTD", make_intrusive<ASTLiteral>(UInt64(1))));
}

std::mutex & reportMapMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<String, std::shared_ptr<QueryAlgorithmReport>> & reportMap()
{
    static std::unordered_map<String, std::shared_ptr<QueryAlgorithmReport>> map;
    return map;
}

ContextPtr resolveQueryContext(ContextPtr context)
{
    if (!context)
        return nullptr;
    if (context->hasQueryContext())
    {
        if (auto query_context = context->getQueryContext())
            return query_context;
    }
    return context;
}

UInt64 saturatingSub(UInt64 value, UInt64 base)
{
    return value >= base ? value - base : 0;
}

}

ColumnsDescription QueryJoinLogElement::getColumnsDescription()
{
    auto lc_string = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());

    return ColumnsDescription
    {
        {"hostname", lc_string, codecZSTD(1), "Hostname of the server executing the query."},
        {"event_date", std::make_shared<DataTypeDate>(), codecDeltaZSTD(2), "Event date."},
        {"event_time", std::make_shared<DataTypeDateTime>(), codecDeltaZSTD(4), "Time when the row was written."},
        {"query_id", std::make_shared<DataTypeString>(), codecZSTD(1), "Query ID. Matches system.query_log."},
        {"initial_query_id", std::make_shared<DataTypeString>(), codecZSTD(1), "Initial query ID. Ties the entry node to shard queries."},
        {"is_initial_query", std::make_shared<DataTypeUInt8>(), codecZSTD(1), "1 on the client query, 0 on a shard query."},
        {"log_comment", std::make_shared<DataTypeString>(), codecZSTD(1), "Value of the log_comment setting, used to tag algorithm runs."},
        {"join_index", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Index of this join inside the query. 0 for query-level and distributed rows."},
        {"phase", lc_string, codecZSTD(1), "choose, build, probe, switch, distributed, or query."},
        {"requested_algorithms", lc_string, codecZSTD(1), "join_algorithm setting, in the order tried."},
        {"chosen_algorithm", lc_string, codecZSTD(1), "IJoin class constructed for this join."},
        {"final_algorithm", lc_string, codecZSTD(1), "IJoin class after a runtime switch. Equals chosen_algorithm when there was no switch."},
        {"switch_reason", lc_string, codecZSTD(1), "none, memory_limit, or another reason recorded at the switch."},
        {"build_side", lc_string, codecZSTD(1), "Side that was inserted into the join table. Hash joins build the right side."},
        {"build_rows", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Rows added to the build side."},
        {"build_bytes", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Bytes added to the build side."},
        {"probe_rows", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Rows from the probe side passed into the join."},
        {"spill_files", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Temporary files written for an external join during this interval."},
        {"spill_bytes", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Compressed bytes written for an external join during this interval."},
        {"wall_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Wall time of this phase in microseconds."},
        {"cpu_user_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "UserTimeMicroseconds delta for this phase."},
        {"cpu_system_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "SystemTimeMicroseconds delta for this phase."},
        {"cpu_wait_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "OSCPUWaitMicroseconds delta for this phase."},
        {"real_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "RealTimeMicroseconds delta. This is a sum across threads."},
        {"file_read_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "OSIOWaitMicroseconds delta. Disk wait, excluding the page cache."},
        {"file_write_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "DiskWriteElapsedMicroseconds delta."},
        {"disk_read_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "DiskReadElapsedMicroseconds delta, including the page cache."},
        {"read_ops", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Read syscalls during this phase."},
        {"read_bytes", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Bytes read from file descriptors during this phase."},
        {"seek_ops", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "lseek calls during this phase."},
        {"file_open", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Files opened during this phase."},
        {"page_cache_hits", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Userspace page cache hits during this phase."},
        {"page_cache_misses", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Userspace page cache misses during this phase."},
        {"s3_read_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "S3 GET and HEAD time during this phase."},
        {"peak_memory_bytes", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Peak process memory observed at the end of the interval."},
        {"product_mode_action", lc_string, codecZSTD(1), "local_rewrite, global_rewrite, prefer_global_in_and_join, allow, or deny."},
        {"shards_total", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Shards in the cluster before optimize_skip_unused_shards."},
        {"shards_used", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Shards the query was sent to."},
        {"processing_stage", lc_string, codecZSTD(1), "Query processing stage chosen for the distributed query."},
        {"aggregation_memory_efficient", std::make_shared<DataTypeUInt8>(), codecZSTD(1), "1 when distributed_aggregation_memory_efficient is enabled."},
        {"gpu_used", std::make_shared<DataTypeUInt8>(), codecZSTD(1), "1 when a GPU join ran. This build has no GPU join, so the value is 0."},
        {"gpu_kernel_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "GPU kernel time in microseconds. Reserved, 0 in this build."},
        {"gpu_memcpy_h2d_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Host-to-device copy time in microseconds. Reserved, 0 in this build."},
        {"gpu_memcpy_d2h_us", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Device-to-host copy time in microseconds. Reserved, 0 in this build."},
        {"gpu_peak_bytes", std::make_shared<DataTypeUInt64>(), codecZSTD(1), "Peak GPU memory in bytes. Reserved, 0 in this build."},
    };
}

void QueryJoinLogElement::appendToBlock(MutableColumns & columns) const
{
    size_t i = 0;
    columns[i++]->insert(getFQDNOrHostName());
    columns[i++]->insert(event_date);
    columns[i++]->insert(event_time);
    columns[i++]->insert(query_id);
    columns[i++]->insert(initial_query_id);
    columns[i++]->insert(is_initial_query);
    columns[i++]->insert(log_comment);
    columns[i++]->insert(join_index);
    columns[i++]->insert(phase);
    columns[i++]->insert(requested_algorithms);
    columns[i++]->insert(chosen_algorithm);
    columns[i++]->insert(final_algorithm);
    columns[i++]->insert(switch_reason);
    columns[i++]->insert(build_side);
    columns[i++]->insert(build_rows);
    columns[i++]->insert(build_bytes);
    columns[i++]->insert(probe_rows);
    columns[i++]->insert(spill_files);
    columns[i++]->insert(spill_bytes);
    columns[i++]->insert(wall_us);
    columns[i++]->insert(cpu_user_us);
    columns[i++]->insert(cpu_system_us);
    columns[i++]->insert(cpu_wait_us);
    columns[i++]->insert(real_us);
    columns[i++]->insert(file_read_us);
    columns[i++]->insert(file_write_us);
    columns[i++]->insert(disk_read_us);
    columns[i++]->insert(read_ops);
    columns[i++]->insert(read_bytes);
    columns[i++]->insert(seek_ops);
    columns[i++]->insert(file_open);
    columns[i++]->insert(page_cache_hits);
    columns[i++]->insert(page_cache_misses);
    columns[i++]->insert(s3_read_us);
    columns[i++]->insert(peak_memory_bytes);
    columns[i++]->insert(product_mode_action);
    columns[i++]->insert(shards_total);
    columns[i++]->insert(shards_used);
    columns[i++]->insert(processing_stage);
    columns[i++]->insert(aggregation_memory_efficient);
    columns[i++]->insert(gpu_used);
    columns[i++]->insert(gpu_kernel_us);
    columns[i++]->insert(gpu_memcpy_h2d_us);
    columns[i++]->insert(gpu_memcpy_d2h_us);
    columns[i++]->insert(gpu_peak_bytes);
}

QueryAlgorithmReport::ResourceSnapshot QueryAlgorithmReport::ResourceSnapshot::capture()
{
    ResourceSnapshot snapshot;
    snapshot.at = std::chrono::steady_clock::now();

    CurrentThread::updatePerformanceCounters();
    auto group = CurrentThread::getGroup();
    if (!group)
        return snapshot;

    const auto counters = group->performance_counters.getPartiallyAtomicSnapshot();
    snapshot.user_us = counters[ProfileEvents::UserTimeMicroseconds];
    snapshot.system_us = counters[ProfileEvents::SystemTimeMicroseconds];
    snapshot.wait_us = counters[ProfileEvents::OSCPUWaitMicroseconds];
    snapshot.real_us = counters[ProfileEvents::RealTimeMicroseconds];
    snapshot.io_wait_us = counters[ProfileEvents::OSIOWaitMicroseconds];
    snapshot.disk_read_us = counters[ProfileEvents::DiskReadElapsedMicroseconds];
    snapshot.disk_write_us = counters[ProfileEvents::DiskWriteElapsedMicroseconds];
    snapshot.read_ops = counters[ProfileEvents::ReadBufferFromFileDescriptorRead];
    snapshot.read_bytes = counters[ProfileEvents::ReadBufferFromFileDescriptorReadBytes];
    snapshot.seek_ops = counters[ProfileEvents::Seek];
    snapshot.file_open = counters[ProfileEvents::FileOpen];
    snapshot.page_cache_hits = counters[ProfileEvents::PageCacheHits];
    snapshot.page_cache_misses = counters[ProfileEvents::PageCacheMisses];
    snapshot.s3_read_us = counters[ProfileEvents::S3ReadMicroseconds];
    snapshot.spill_files = counters[ProfileEvents::ExternalJoinWritePart];
    snapshot.spill_bytes = counters[ProfileEvents::ExternalJoinCompressedBytes];

    Int64 peak = group->memory_tracker.getPeak();
    snapshot.peak_memory_bytes = peak > 0 ? static_cast<UInt64>(peak) : 0;
    return snapshot;
}

void QueryAlgorithmReport::fillResourceDelta(QueryJoinLogElement & elem, const ResourceSnapshot & from, const ResourceSnapshot & to)
{
    elem.wall_us = static_cast<UInt64>(std::chrono::duration_cast<std::chrono::microseconds>(to.at - from.at).count());
    elem.cpu_user_us = saturatingSub(to.user_us, from.user_us);
    elem.cpu_system_us = saturatingSub(to.system_us, from.system_us);
    elem.cpu_wait_us = saturatingSub(to.wait_us, from.wait_us);
    elem.real_us = saturatingSub(to.real_us, from.real_us);
    elem.file_read_us = saturatingSub(to.io_wait_us, from.io_wait_us);
    elem.file_write_us = saturatingSub(to.disk_write_us, from.disk_write_us);
    elem.disk_read_us = saturatingSub(to.disk_read_us, from.disk_read_us);
    elem.read_ops = saturatingSub(to.read_ops, from.read_ops);
    elem.read_bytes = saturatingSub(to.read_bytes, from.read_bytes);
    elem.seek_ops = saturatingSub(to.seek_ops, from.seek_ops);
    elem.file_open = saturatingSub(to.file_open, from.file_open);
    elem.page_cache_hits = saturatingSub(to.page_cache_hits, from.page_cache_hits);
    elem.page_cache_misses = saturatingSub(to.page_cache_misses, from.page_cache_misses);
    elem.s3_read_us = saturatingSub(to.s3_read_us, from.s3_read_us);
    elem.spill_files = saturatingSub(to.spill_files, from.spill_files);
    elem.spill_bytes = saturatingSub(to.spill_bytes, from.spill_bytes);
    elem.peak_memory_bytes = to.peak_memory_bytes;
}

std::shared_ptr<QueryAlgorithmReport> QueryAlgorithmReport::getOrCreate(ContextPtr context)
{
    context = resolveQueryContext(context);
    if (!context || context->isInternalQuery())
        return nullptr;

    const String & query_id = context->getClientInfo().current_query_id;
    if (query_id.empty())
        return nullptr;

    std::lock_guard lock(reportMapMutex());
    auto & map = reportMap();
    auto it = map.find(query_id);
    if (it != map.end())
        return it->second;

    auto report = std::make_shared<QueryAlgorithmReport>();
    report->origin = ResourceSnapshot::capture();
    report->origin_set = true;
    map.emplace(query_id, report);
    return report;
}

QueryAlgorithmReport::JoinSlot & QueryAlgorithmReport::slotFor(const void * join_key)
{
    for (auto & slot : joins)
    {
        if (slot.key == join_key)
            return slot;
    }

    JoinSlot slot;
    slot.key = join_key;
    slot.join_index = ++next_join_index;
    joins.push_back(std::move(slot));
    return joins.back();
}

void QueryAlgorithmReport::noteChosen(ContextPtr context, const void * join_key, const String & requested, const String & chosen)
{
    auto report = getOrCreate(context);
    if (!report || !join_key)
        return;

    std::lock_guard lock(report->mutex);
    auto & slot = report->slotFor(join_key);
    slot.requested_algorithms = requested;
    slot.chosen_algorithm = chosen;
    if (slot.final_algorithm.empty())
        slot.final_algorithm = chosen;
}

void QueryAlgorithmReport::noteSwitch(
    ContextPtr context, const void * join_key, const String & final_algorithm, const String & reason, UInt64 build_bytes, UInt64 build_rows)
{
    auto report = getOrCreate(context);
    if (!report || !join_key)
        return;

    std::lock_guard lock(report->mutex);
    auto & slot = report->slotFor(join_key);
    slot.final_algorithm = final_algorithm;
    slot.switch_reason = reason;
    if (build_rows > slot.build_rows)
        slot.build_rows = build_rows;
    if (build_bytes > slot.build_bytes)
        slot.build_bytes = build_bytes;
}

void QueryAlgorithmReport::addBuildRows(ContextPtr context, const void * join_key, UInt64 rows, UInt64 bytes)
{
    auto report = getOrCreate(context);
    if (!report || !join_key)
        return;

    std::lock_guard lock(report->mutex);
    auto & slot = report->slotFor(join_key);
    if (!slot.build_started)
    {
        slot.build_started = true;
        slot.build_start = ResourceSnapshot::capture();
    }
    slot.build_rows += rows;
    slot.build_bytes += bytes;
}

void QueryAlgorithmReport::finishBuild(ContextPtr context, const void * join_key)
{
    auto report = getOrCreate(context);
    if (!report || !join_key)
        return;

    std::lock_guard lock(report->mutex);
    auto & slot = report->slotFor(join_key);
    if (slot.build_finished)
        return;
    if (!slot.build_started)
        slot.build_start = report->origin;
    slot.build_end = ResourceSnapshot::capture();
    slot.build_finished = true;
}

void QueryAlgorithmReport::addProbeRows(ContextPtr context, const void * join_key, UInt64 rows)
{
    auto report = getOrCreate(context);
    if (!report || !join_key)
        return;

    std::lock_guard lock(report->mutex);
    auto & slot = report->slotFor(join_key);
    if (!slot.probe_started)
    {
        slot.probe_started = true;
        slot.probe_start = ResourceSnapshot::capture();
    }
    slot.probe_rows += rows;
}

void QueryAlgorithmReport::noteProductMode(ContextPtr context, const String & action)
{
    auto report = getOrCreate(context);
    if (!report)
        return;

    std::lock_guard lock(report->mutex);
    DistributedNote note;
    note.product_mode_action = action;
    report->distributed.push_back(std::move(note));
}

void QueryAlgorithmReport::noteShards(ContextPtr context, UInt64 shards_total, UInt64 shards_used)
{
    auto report = getOrCreate(context);
    if (!report)
        return;

    std::lock_guard lock(report->mutex);
    DistributedNote note;
    note.shards_total = shards_total;
    note.shards_used = shards_used;
    report->distributed.push_back(std::move(note));
}

void QueryAlgorithmReport::noteProcessingStage(ContextPtr context, const String & stage)
{
    auto report = getOrCreate(context);
    if (!report)
        return;

    std::lock_guard lock(report->mutex);
    DistributedNote note;
    note.processing_stage = stage;
    report->distributed.push_back(std::move(note));
}

void QueryAlgorithmReport::flush(ContextPtr context)
{
    context = resolveQueryContext(context);
    if (!context || context->isInternalQuery())
        return;

    const String query_id = context->getClientInfo().current_query_id;
    if (query_id.empty())
        return;

    std::shared_ptr<QueryAlgorithmReport> report;
    {
        std::lock_guard lock(reportMapMutex());
        auto & map = reportMap();
        auto it = map.find(query_id);
        if (it == map.end())
            return;
        report = it->second;
        map.erase(it);
    }

    auto log = context->getQueryJoinLog();
    if (!log)
        return;

    const auto now = std::chrono::system_clock::now();
    const time_t event_time = std::chrono::system_clock::to_time_t(now);
    const UInt16 event_date = DateLUT::instance().toDayNum(event_time);

    const auto & client_info = context->getClientInfo();
    const auto & settings = context->getSettingsRef();

    QueryJoinLogElement common;
    common.event_date = event_date;
    common.event_time = event_time;
    common.query_id = query_id;
    common.initial_query_id = client_info.initial_query_id;
    common.is_initial_query = client_info.query_kind == ClientInfo::QueryKind::INITIAL_QUERY;
    common.log_comment = settings[Setting::log_comment];
    common.aggregation_memory_efficient = settings[Setting::distributed_aggregation_memory_efficient] ? 1 : 0;
    common.build_side = "right";

    std::lock_guard lock(report->mutex);
    const auto end = ResourceSnapshot::capture();

    auto push = [&](QueryJoinLogElement elem)
    {
        log->add(std::move(elem));
    };

    for (auto & slot : report->joins)
    {
        if (slot.final_algorithm.empty())
            slot.final_algorithm = slot.chosen_algorithm;

        QueryJoinLogElement chosen = common;
        chosen.join_index = slot.join_index;
        chosen.phase = "choose";
        chosen.requested_algorithms = slot.requested_algorithms;
        chosen.chosen_algorithm = slot.chosen_algorithm;
        chosen.final_algorithm = slot.final_algorithm;
        chosen.switch_reason = slot.switch_reason;
        chosen.build_rows = slot.build_rows;
        chosen.build_bytes = slot.build_bytes;
        chosen.probe_rows = slot.probe_rows;
        push(chosen);

        if (slot.build_finished)
        {
            QueryJoinLogElement build = chosen;
            build.phase = "build";
            fillResourceDelta(build, slot.build_start, slot.build_end);
            push(build);
        }

        if (slot.probe_started)
        {
            QueryJoinLogElement probe = chosen;
            probe.phase = "probe";
            fillResourceDelta(probe, slot.probe_start, end);
            push(probe);
        }

        if (slot.switch_reason != "none")
        {
            QueryJoinLogElement switched = chosen;
            switched.phase = "switch";
            push(switched);
        }
    }

    for (const auto & note : report->distributed)
    {
        QueryJoinLogElement distributed = common;
        distributed.phase = "distributed";
        distributed.product_mode_action = note.product_mode_action;
        distributed.shards_total = note.shards_total;
        distributed.shards_used = note.shards_used;
        distributed.processing_stage = note.processing_stage;
        push(distributed);
    }

    if (report->joins.empty() && report->distributed.empty())
        return;

    QueryJoinLogElement summary = common;
    summary.phase = "query";
    if (report->origin_set)
        fillResourceDelta(summary, report->origin, end);
    if (!report->joins.empty())
    {
        const auto & slot = report->joins.front();
        summary.join_index = slot.join_index;
        summary.requested_algorithms = slot.requested_algorithms;
        summary.chosen_algorithm = slot.chosen_algorithm;
        summary.final_algorithm = slot.final_algorithm.empty() ? slot.chosen_algorithm : slot.final_algorithm;
        summary.switch_reason = slot.switch_reason;
        summary.build_rows = slot.build_rows;
        summary.build_bytes = slot.build_bytes;
        summary.probe_rows = slot.probe_rows;
    }
    if (!report->distributed.empty())
    {
        const auto & note = report->distributed.back();
        summary.product_mode_action = note.product_mode_action;
        summary.shards_total = note.shards_total;
        summary.shards_used = note.shards_used;
        summary.processing_stage = note.processing_stage;
    }
    push(summary);
}

}
