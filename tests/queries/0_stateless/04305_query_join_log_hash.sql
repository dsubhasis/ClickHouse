SELECT a.id
FROM
(
    SELECT number AS id FROM numbers(3)
) AS a
INNER JOIN
(
    SELECT number AS id FROM numbers(3)
) AS b ON a.id = b.id
ORDER BY a.id
SETTINGS join_algorithm = 'hash', log_comment = 'query_join_log_probe';

SYSTEM FLUSH LOGS query_join_log;

SELECT if(count() > 0, 'choose', 'missing')
FROM system.query_join_log
WHERE log_comment = 'query_join_log_probe' AND phase = 'choose' AND chosen_algorithm = 'HashJoin';

SELECT if(count() > 0, 'build', 'missing')
FROM system.query_join_log
WHERE log_comment = 'query_join_log_probe' AND phase = 'build' AND build_rows = 3;

SELECT if(count() > 0, 'probe', 'missing')
FROM system.query_join_log
WHERE log_comment = 'query_join_log_probe' AND phase = 'probe' AND probe_rows = 3;

SELECT if(count() > 0, 'query', 'missing')
FROM system.query_join_log
WHERE log_comment = 'query_join_log_probe' AND phase = 'query' AND gpu_used = 0 AND final_algorithm = 'HashJoin';
