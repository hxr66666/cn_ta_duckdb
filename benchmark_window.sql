-- 窗口聚合性能基准（量化 O(N×window) 瓶颈与环形缓冲优化效果）
-- 用法: duckdb -unsigned < benchmark_window.sql
-- 数据: 30万行
-- 对比: window >> period 时白名单局部窗口函数应显著加速（SMA window=1000: ~312ms -> ~53ms）

LOAD 'build/release/extension/cn_ta/cn_ta.duckdb_extension';

CREATE TABLE t AS SELECT range::DOUBLE AS price, range AS rn FROM range(300000);

.print ''
.print '=== 1) 白名单函数 SMA: 环形缓冲限制 state 累积长度，window>>period 时显著加速 ==='
.print 'window=20,  period=20'
EXPLAIN ANALYZE SELECT ct_sma(price, 20) OVER (ORDER BY rn ROWS BETWEEN 19 PRECEDING AND CURRENT ROW) AS s FROM t;
.print 'window=100, period=20'
EXPLAIN ANALYZE SELECT ct_sma(price, 20) OVER (ORDER BY rn ROWS BETWEEN 99 PRECEDING AND CURRENT ROW) AS s FROM t;
.print 'window=1000, period=20  (优化前 312ms)'
EXPLAIN ANALYZE SELECT ct_sma(price, 20) OVER (ORDER BY rn ROWS BETWEEN 999 PRECEDING AND CURRENT ROW) AS s FROM t;

.print ''
.print '=== 2) 计算重的白名单函数 LINEARREG / TSF ==='
.print 'window=1000, period=20  (LINEARREG 优化前 313ms)'
EXPLAIN ANALYZE SELECT ct_linearreg(price, 20) OVER (ORDER BY rn ROWS BETWEEN 999 PRECEDING AND CURRENT ROW) AS s FROM t;
.print 'window=1000, period=20  (TSF)'
EXPLAIN ANALYZE SELECT ct_tsf(price, 20) OVER (ORDER BY rn ROWS BETWEEN 999 PRECEDING AND CURRENT ROW) AS s FROM t;

.print ''
.print '=== 3) 对照: scalar t_sma 整列一次算 (O(N)) ==='
EXPLAIN ANALYZE SELECT ct_sma(list(price ORDER BY rn), 20) FROM t;

.print ''
.print '=== 4) 非白名单 EMA: 不被截断，保持原逻辑（window 大时仍随 window 增长） ==='
.print 'window=1000, period=20'
EXPLAIN ANALYZE SELECT ct_ema(price, 20) OVER (ORDER BY rn ROWS BETWEEN 999 PRECEDING AND CURRENT ROW) AS s FROM t;
