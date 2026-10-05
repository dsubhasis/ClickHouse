#pragma once

#include <Interpreters/SystemLog.h>
#include <Core/NamesAndTypes.h>
#include <Core/NamesAndAliases.h>
#include <Storages/ColumnsDescription.h>
#include <Interpreters/Context_fwd.h>

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>


namespace DB
{

/// One row in system.query_join_log. GPU columns stay zero until a GPU join exists.
struct QueryJoinLogElement
{
    UInt16 event_date{};
    time_t event_time{};

    String query_id;
    String initial_query_id;
    UInt8 is_initial_query = 0;
    String log_comment;

    UInt64 join_index = 0;
    String phase;
    String requested_algorithms;
    String chosen_algorithm;
    String final_algorithm;
    String switch_reason;
    String build_side;

    UInt64 build_rows = 0;
    UInt64 build_bytes = 0;
    UInt64 probe_rows = 0;
    UInt64 spill_files = 0;
    UInt64 spill_bytes = 0;

    UInt64 wall_us = 0;
    UInt64 cpu_user_us = 0;
    UInt64 cpu_system_us = 0;
    UInt64 cpu_wait_us = 0;
    UInt64 real_us = 0;
    UInt64 file_read_us = 0;
    UInt64 file_write_us = 0;
    UInt64 disk_read_us = 0;
    UInt64 read_ops = 0;
    UInt64 read_bytes = 0;
    UInt64 seek_ops = 0;
    UInt64 file_open = 0;
    UInt64 page_cache_hits = 0;
    UInt64 page_cache_misses = 0;
    UInt64 s3_read_us = 0;
    UInt64 peak_memory_bytes = 0;

    String product_mode_action;
    UInt64 shards_total = 0;
    UInt64 shards_used = 0;
    String processing_stage;
    UInt8 aggregation_memory_efficient = 0;

    UInt8 gpu_used = 0;
    UInt64 gpu_kernel_us = 0;
    UInt64 gpu_memcpy_h2d_us = 0;
    UInt64 gpu_memcpy_d2h_us = 0;
    UInt64 gpu_peak_bytes = 0;

    static std::string name() { return "QueryJoinLog"; }
    static ColumnsDescription getColumnsDescription();
    static NamesAndAliases getNamesAndAliases() { return {}; }
    void appendToBlock(MutableColumns & columns) const;
};

class QueryJoinLog : public SystemLog<QueryJoinLogElement>
{
    using SystemLog<QueryJoinLogElement>::SystemLog;
};

/// Per-query collector for join algorithm tests. Keyed by query_id.
/// Records which algorithm ran, whether it switched, and file/CPU deltas per phase.
class QueryAlgorithmReport
{
public:
    static void noteChosen(ContextPtr context, const void * join_key, const String & requested, const String & chosen);
    static void noteSwitch(ContextPtr context, const void * join_key, const String & final_algorithm, const String & reason, UInt64 build_bytes, UInt64 build_rows);
    static void addBuildRows(ContextPtr context, const void * join_key, UInt64 rows, UInt64 bytes);
    static void finishBuild(ContextPtr context, const void * join_key);
    static void addProbeRows(ContextPtr context, const void * join_key, UInt64 rows);
    static void noteProductMode(ContextPtr context, const String & action);
    static void noteShards(ContextPtr context, UInt64 shards_total, UInt64 shards_used);
    static void noteProcessingStage(ContextPtr context, const String & stage);

    /// Write collected rows into system.query_join_log and drop the collector.
    static void flush(ContextPtr context);

private:
    struct ResourceSnapshot
    {
        UInt64 user_us = 0;
        UInt64 system_us = 0;
        UInt64 wait_us = 0;
        UInt64 real_us = 0;
        UInt64 io_wait_us = 0;
        UInt64 disk_read_us = 0;
        UInt64 disk_write_us = 0;
        UInt64 read_ops = 0;
        UInt64 read_bytes = 0;
        UInt64 seek_ops = 0;
        UInt64 file_open = 0;
        UInt64 page_cache_hits = 0;
        UInt64 page_cache_misses = 0;
        UInt64 s3_read_us = 0;
        UInt64 spill_files = 0;
        UInt64 spill_bytes = 0;
        UInt64 peak_memory_bytes = 0;
        std::chrono::steady_clock::time_point at{};

        static ResourceSnapshot capture();
    };

    struct JoinSlot
    {
        const void * key = nullptr;
        UInt64 join_index = 0;
        String requested_algorithms;
        String chosen_algorithm;
        String final_algorithm;
        String switch_reason = "none";
        UInt64 build_rows = 0;
        UInt64 build_bytes = 0;
        UInt64 probe_rows = 0;
        bool build_started = false;
        bool build_finished = false;
        bool probe_started = false;
        ResourceSnapshot build_start;
        ResourceSnapshot build_end;
        ResourceSnapshot probe_start;
    };

    struct DistributedNote
    {
        String product_mode_action;
        UInt64 shards_total = 0;
        UInt64 shards_used = 0;
        String processing_stage;
    };

    static std::shared_ptr<QueryAlgorithmReport> getOrCreate(ContextPtr context);
    JoinSlot & slotFor(const void * join_key);

    static void fillResourceDelta(QueryJoinLogElement & elem, const ResourceSnapshot & from, const ResourceSnapshot & to);

    std::mutex mutex;
    UInt64 next_join_index = 0;
    std::vector<JoinSlot> joins;
    std::vector<DistributedNote> distributed;
    ResourceSnapshot origin;
    bool origin_set = false;
};

}
