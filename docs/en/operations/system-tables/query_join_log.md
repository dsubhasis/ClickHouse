---
description: 'System table with per-query join algorithm decisions and file, CPU, and GPU phase counters.'
keywords: ['system table', 'query_join_log', 'join']
sidebar_label: 'query_join_log'
sidebar_position: 80
slug: /operations/system-tables/query_join_log
title: 'system.query_join_log'
doc_type: 'reference'
---

import SystemTableCloud from '@site/docs/_snippets/_system_table_cloud.md';

<SystemTableCloud/>

## Description {#description}

Contains one row per join phase for queries that executed a join or a distributed join rewrite. Use it to compare join algorithms on the same query: which class ran, whether it spilled, and how much CPU and file time that phase took.

GPU columns are reserved. This build has no GPU join, so `gpu_used`, `gpu_kernel_us`, `gpu_memcpy_h2d_us`, `gpu_memcpy_d2h_us`, and `gpu_peak_bytes` are `0`.

The table is created when `query_join_log` is present in the server configuration. To force a flush, use [SYSTEM FLUSH LOGS](/sql-reference/statements/system#flush-logs).

Tag a run with `log_comment` so the rows can be selected after the query finishes:

```sql
SELECT *
FROM system.query_join_log
WHERE log_comment = 'q5_parallel_hash' AND phase = 'query'
```

## Phases {#phases}

- `choose` records the `join_algorithm` setting and the `IJoin` class that was constructed.
- `build` records rows and bytes inserted into the right side, plus file and CPU deltas for that interval.
- `probe` records rows from the left side and the file and CPU deltas until the query finished.
- `switch` is written when `parallel_hash` or `auto` moves to `GraceHashJoin` or `PartialMergeJoin` because of the memory limit.
- `distributed` records `distributed_product_mode` rewrites, shard counts after `optimize_skip_unused_shards`, and the processing stage.
- `query` is one summary row for the whole query.

## Columns {#columns}

- `query_id`, `initial_query_id`, `is_initial_query` tie an entry-node query to its shard queries.
- `requested_algorithms` is the setting. `chosen_algorithm` is the class that was built. `final_algorithm` is the class after a runtime switch.
- `build_rows`, `build_bytes`, `probe_rows` show which side was large. Hash joins build the right table.
- `file_read_us` is real disk wait (`OSIOWaitMicroseconds`). `disk_read_us` includes the page cache. `file_write_us` is the spill and write time.
- `cpu_user_us`, `cpu_system_us`, `cpu_wait_us`, and `real_us` are deltas of the query's process counters. `real_us` is a sum across threads.
- `page_cache_hits` and `page_cache_misses` show whether the run was warm.
- `spill_files` and `spill_bytes` are external join traffic during the interval.
- `product_mode_action` is `local_rewrite`, `global_rewrite`, `prefer_global_in_and_join`, `allow`, or `deny`.
- `shards_total` and `shards_used` show shard skipping. `processing_stage` is the stage returned for the distributed query.
- `aggregation_memory_efficient` is `1` when `distributed_aggregation_memory_efficient` is enabled.
- `peak_memory_bytes` is the process peak at the end of the interval.
