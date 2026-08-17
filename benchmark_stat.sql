-- stat_* 金融统计函数性能基准
-- 用法: duckdb -unsigned < benchmark_stat.sql
-- 数据: 100 万行合成序列

LOAD 'build/release/extension/cn_ta/cn_ta.duckdb_extension';

CREATE TABLE series AS
SELECT (sin(i * 0.01) * 10 + i * 0.001 + 100) AS price,
       i AS rn
FROM range(1000000) t(i);

-- 用 LIST 聚合整列后传给 stat_* 标量函数
CREATE TABLE agg AS SELECT list(price ORDER BY rn) AS px FROM series;
CREATE TABLE agg2 AS SELECT list(price ORDER BY rn) AS px, list(price * 2 ORDER BY rn) AS py FROM series;

.print ''
.print '=== 1) 单变量统计 (100万元素 LIST) ==='
.print 'stat_var'
EXPLAIN ANALYZE SELECT stat_var(px) FROM agg;
.print 'stat_stddev'
EXPLAIN ANALYZE SELECT stat_stddev(px) FROM agg;
.print 'stat_max_drawdown'
EXPLAIN ANALYZE SELECT stat_max_drawdown(px) FROM agg;
.print 'stat_sharpe'
EXPLAIN ANALYZE SELECT stat_sharpe(px, 0.0, 252.0) FROM agg;
.print 'stat_skew'
EXPLAIN ANALYZE SELECT stat_skew(px) FROM agg;
.print 'stat_kurtosis'
EXPLAIN ANALYZE SELECT stat_kurtosis(px) FROM agg;
.print 'stat_garch_vol'
EXPLAIN ANALYZE SELECT stat_garch_vol(px, 252.0, 0.1, 0.85) FROM agg;

.print ''
.print '=== 2) 双变量统计 ==='
.print 'stat_corr'
EXPLAIN ANALYZE SELECT stat_corr(px, py) FROM agg2;
.print 'stat_beta'
EXPLAIN ANALYZE SELECT stat_beta(px, py) FROM agg2;
.print 'stat_coint'
EXPLAIN ANALYZE SELECT stat_coint(px, py) FROM agg2;

.print ''
.print '=== 3) 回归 ==='
.print 'stat_regress'
EXPLAIN ANALYZE SELECT stat_regress(px, py) FROM agg2;
.print 'stat_ols (多变量)'
EXPLAIN ANALYZE SELECT stat_ols(px, list_value(px, py)) FROM agg2;

.print ''
.print '=== 4) 矩阵分析 ==='
.print 'stat_pca (3 变量)'
EXPLAIN ANALYZE SELECT stat_pca(list_value(px, py, list_transform(px, x -> x * x))) FROM agg2;

.print ''
.print '=== 5) 窗口聚合 stat_avg OVER (30万行, 滑窗100) ==='
CREATE TABLE win AS
SELECT sin(rn * 0.01) * 10 + 100 AS price, rn
FROM range(300000) t(rn);
EXPLAIN ANALYZE SELECT stat_avg(price) OVER (ORDER BY rn ROWS BETWEEN 99 PRECEDING AND CURRENT ROW) AS avg FROM win;
EXPLAIN ANALYZE SELECT stat_rolling_var(price) OVER (ORDER BY rn ROWS BETWEEN 99 PRECEDING AND CURRENT ROW) AS var FROM win;
EXPLAIN ANALYZE SELECT stat_rolling_beta(price, price) OVER (ORDER BY rn ROWS BETWEEN 99 PRECEDING AND CURRENT ROW) AS beta FROM win;
