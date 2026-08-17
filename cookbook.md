# cn_ta DuckDB 扩展 — SQL Cookbook

每个类别一个示例。窗口函数（`cta_`）为默认；标量（`ct_`）作为替代方案展示。

---

## 示例数据

```sql
CREATE TABLE ohlc AS
SELECT * FROM (VALUES
    ('2024-01-01'::DATE, 10.0, 11.2,  9.8, 10.5, 120000),
    ('2024-01-02'::DATE, 10.5, 11.8, 10.2, 11.3, 135000),
    ('2024-01-03'::DATE, 11.3, 12.1, 11.0, 11.8,  98000),
    ('2024-01-04'::DATE, 11.8, 12.5, 11.5, 12.2, 145000),
    ('2024-01-05'::DATE, 12.2, 13.0, 11.9, 12.7, 167000),
    ('2024-01-06'::DATE, 12.7, 13.4, 12.3, 13.1, 112000),
    ('2024-01-07'::DATE, 13.1, 13.8, 12.8, 13.5, 130000),
    ('2024-01-08'::DATE, 13.5, 14.2, 13.1, 13.9, 118000),
    ('2024-01-09'::DATE, 13.9, 14.6, 13.5, 14.3, 155000),
    ('2024-01-10'::DATE, 14.3, 15.0, 13.9, 14.8, 142000),
    ('2024-01-11'::DATE, 14.8, 15.3, 14.2, 15.0, 138000),
    ('2024-01-12'::DATE, 15.0, 15.8, 14.7, 15.4, 160000),
    ('2024-01-13'::DATE, 15.4, 16.1, 15.0, 15.8, 175000),
    ('2024-01-14'::DATE, 15.8, 16.5, 15.3, 16.2, 190000),
    ('2024-01-15'::DATE, 16.2, 16.9, 15.8, 16.6, 165000),
    ('2024-01-16'::DATE, 16.6, 17.2, 16.1, 16.9, 148000),
    ('2024-01-17'::DATE, 16.9, 17.6, 16.5, 17.3, 155000),
    ('2024-01-18'::DATE, 17.3, 18.0, 16.8, 17.7, 172000),
    ('2024-01-19'::DATE, 17.7, 18.4, 17.2, 18.1, 168000),
    ('2024-01-20'::DATE, 18.1, 18.8, 17.6, 18.5, 182000)
) AS t(ts, open, high, low, close, volume);
```

---

## 趋势 — SMA 金叉/死叉信号

```sql
-- 窗口：5 周期与 10 周期 SMA，检测金叉/死叉
WITH signals AS (
    SELECT ts, close,
           cta_sma(close, 5)  OVER w AS sma_5,
           cta_sma(close, 10) OVER w AS sma_10
    FROM ohlc
    WINDOW w AS (ORDER BY ts)
)
SELECT ts, close, round(sma_5, 2) AS sma_5, round(sma_10, 2) AS sma_10,
       CASE
           WHEN sma_5 > sma_10 AND lag(sma_5) OVER (ORDER BY ts) <= lag(sma_10) OVER (ORDER BY ts)
               THEN '金叉'
           WHEN sma_5 < sma_10 AND lag(sma_5) OVER (ORDER BY ts) >= lag(sma_10) OVER (ORDER BY ts)
               THEN '死叉'
       END AS signal
FROM signals
WHERE sma_10 IS NOT NULL;
```

标量替代方案：

```sql
SELECT unnest(ct_sma((SELECT list(close ORDER BY ts) FROM ohlc), 5)) AS sma_5;
```

---

## 动量 — RSI 超买/超卖

```sql
-- 窗口：14 周期 RSI 与信号区间
SELECT ts, close,
       round(cta_rsi(close, 14) OVER (ORDER BY ts), 2) AS rsi,
       CASE
           WHEN cta_rsi(close, 14) OVER (ORDER BY ts) >= 70 THEN '超买'
           WHEN cta_rsi(close, 14) OVER (ORDER BY ts) <= 30 THEN '超卖'
           ELSE '中性'
       END AS signal
FROM ohlc;
```

标量替代方案：

```sql
SELECT unnest(ct_rsi((SELECT list(close ORDER BY ts) FROM ohlc), 14)) AS rsi;
```

---

## 动量（多输出）— 随机指标（Stochastic Oscillator）

```sql
-- 窗口：随机指标 %K/%D 与超买超卖区间
SELECT ts, close, s.slowk, s.slowd,
       CASE
           WHEN s.slowk > 80 THEN '超买'
           WHEN s.slowk < 20 THEN '超卖'
           ELSE '中性'
       END AS signal
FROM (
    SELECT ts, close,
           cta_stoch(high, low, close, 5, 3, 0, 3, 0) OVER (ORDER BY ts) AS s
    FROM ohlc
)
WHERE s.slowk IS NOT NULL;
```

---

## 趋势（多输出）— MACD 信号线交叉

```sql
-- 窗口：MACD 与柱状图方向
SELECT ts, close, round(m.macd, 4) AS macd, round(m.signal, 4) AS signal, round(m.hist, 4) AS hist,
       CASE WHEN m.hist > 0 THEN '看涨' ELSE '看跌' END AS direction
FROM (
    SELECT ts, close,
           cta_macd(close, 12, 26, 9) OVER (ORDER BY ts) AS m
    FROM ohlc
)
WHERE m.macd IS NOT NULL;
```

---

## 成交量 — 柴金累积/派发线（Chaikin A/D Line）

```sql
-- 窗口：累积 A/D 线（无周期参数）
SELECT ts, close, volume,
       round(cta_ad(high, low, close, volume) OVER (ORDER BY ts), 0) AS ad_line
FROM ohlc;
```

---

## 波动率 — 布林带挤压检测

```sql
-- 窗口：布林带与带宽（用于挤压检测）
SELECT ts, close,
       round(b.upper, 2) AS upper, round(b.middle, 2) AS middle, round(b.lower, 2) AS lower,
       round((b.upper - b.lower) / b.middle * 100, 2) AS bandwidth_pct
FROM (
    SELECT ts, close,
           cta_bbands(close, 10, 2.0, 2.0, 0) OVER (ORDER BY ts) AS b
    FROM ohlc
)
WHERE b.upper IS NOT NULL;
```

---

## 价格变换 — 典型价格（Typical Price）

```sql
-- 窗口：典型价格作为单一合成序列
SELECT ts, round(cta_typprice(high, low, close) OVER (ORDER BY ts), 2) AS typical_price
FROM ohlc;
```

---

## 周期 — 希尔伯特变换趋势模式

```sql
-- 窗口：检测市场处于趋势还是震荡
SELECT ts, close,
       cta_ht_trendmode(close) OVER (ORDER BY ts) AS mode,
       CASE cta_ht_trendmode(close) OVER (ORDER BY ts)
           WHEN 1 THEN '趋势'
           WHEN 0 THEN '震荡'
       END AS regime
FROM ohlc;
```

---

## 统计 — 线性回归趋势方向

```sql
-- 窗口：斜率正负决定趋势方向
SELECT ts, close,
       round(cta_linearreg_slope(close, 10) OVER (ORDER BY ts), 4) AS slope,
       CASE WHEN cta_linearreg_slope(close, 10) OVER (ORDER BY ts) > 0
            THEN '上升' ELSE '下降'
       END AS trend
FROM ohlc;
```

---

## 形态识别 — 扫描多种 K 线形态

形态函数返回 `100`（看涨）、`-100`（看跌）或 `0`（无形态）。

```sql
-- 窗口：扫描常见反转形态
SELECT ts, open, high, low, close,
       cta_cdlhammer(open, high, low, close)    OVER (ORDER BY ts) AS hammer,
       cta_cdlengulfing(open, high, low, close) OVER (ORDER BY ts) AS engulfing,
       cta_cdldoji(open, high, low, close)      OVER (ORDER BY ts) AS doji
FROM ohlc;
```

---

## 数学变换 — 对数收益

```sql
-- 标量：计算收盘价的对数
SELECT unnest(ct_ln((SELECT list(close ORDER BY ts) FROM ohlc))) AS ln_close;
```

---

## 技巧

**NULL 处理** — 前 `period - 1` 行返回 NULL（lookback）。用 `WHERE col IS NOT NULL` 过滤。

**多输出字段访问** — 用子查询访问结构体字段：

```sql
SELECT m.macd, m.signal FROM (
    SELECT cta_macd(close, 12, 26, 9) OVER (ORDER BY ts) AS m FROM ohlc
);
```

**均线类型常量** — 像 `cta_bbands` 这类函数接受 `ma_type`：

| 值 | 类型 |
|----|------|
| 0 | SMA |
| 1 | EMA |
| 2 | WMA |
| 3 | DEMA |
| 4 | TEMA |
| 5 | TRIMA |
| 6 | KAMA |
| 7 | MAMA |
| 8 | T3 |

---

## 时间对齐窗口函数 (cta_*_ts)

针对分钟 K 数据缺口（收盘竞价 14:58/14:59、盘中停牌、节假日）导致指标错位的问题，
`cta_*_ts` 在扩展内部维护交易日 bar 网格 + 前值填充，使指标严格对齐日历时间。

### 基本用法

```sql
-- 1 分钟 K 对齐（默认）
SELECT ts,
       cta_sma_ts(ts, close, 20) OVER (PARTITION BY symbol ORDER BY ts) AS sma20
FROM minute_bars;

-- 5 分钟 K 对齐（第 4 个参数 bar_period）
SELECT cta_sma_ts(ts, close, 20, 5) OVER (PARTITION BY symbol ORDER BY ts) AS sma20_5min
FROM minute_bars;
```

### 多输入指标

```sql
-- ATR（P3: high/low/close + period）
SELECT cta_atr_ts(ts, high, low, close, 14) OVER (PARTITION BY symbol ORDER BY ts) AS atr14
FROM minute_bars;

-- AD 累积（P4: high/low/close/volume）
SELECT cta_ad_ts(ts, high, low, close, volume) OVER (PARTITION BY symbol ORDER BY ts) AS ad
FROM minute_bars;

-- 均价值（P6: high/low）
SELECT cta_medprice_ts(ts, high, low) OVER (PARTITION BY symbol ORDER BY ts) AS med
FROM minute_bars;
```

### 交易日历

```sql
-- 判断是否交易日
SELECT cn_ta_is_trading_day(TIMESTAMP '2026-10-01 10:00:00');  -- false（国庆）

-- 追加自定义休市日（如临时休市）
SELECT cn_ta_set_holiday(DATE '2026-10-09');

-- 清空自定义休市日
SELECT cn_ta_clear_holidays();
```

> 内置 2020~2026 A股法定节假日；缺口处前值填充（价格不变），volume 填 0。

---

## 金融统计与回归 (stat_*)

基于 Eigen 的高级金融统计，输入为 `LIST<DOUBLE>`，返回标量或 STRUCT。

### 单变量统计

```sql
-- 样本方差 / 标准差
SELECT stat_var(list(close ORDER BY ts)), stat_stddev(list(close ORDER BY ts)) FROM ohlc;

-- 偏度 / 峰度
SELECT stat_skew(list(close ORDER BY ts)), stat_kurtosis(list(close ORDER BY ts)) FROM ohlc;

-- 最大回撤（0-1 比例）与持续期
SELECT stat_max_drawdown(list(close ORDER BY ts)),
       stat_max_drawdown_duration(list(close ORDER BY ts)) FROM ohlc;

-- 夏普比率（收益率、无风险利率、年化周期数）
SELECT stat_sharpe(list_returns, 0.02, 252) FROM returns_table;

-- 年化波动率 / EWMA 波动率 / 历史 VaR
SELECT stat_annual_vol(list_returns, 252),
       stat_ewma_vol(list_returns, 0.94),
       stat_var_historical(list_returns, 0.05) FROM returns_table;
```

### 双变量统计

```sql
-- 协方差 / 相关系数 / beta
SELECT stat_cov(list(x), list(y)),
       stat_corr(list(close ORDER BY ts), list(volume ORDER BY ts)),
       stat_beta(list(stock_ret), list(market_ret)) FROM ohlc;

-- 信息比率 / 协整检验
SELECT stat_information_ratio(list(fund), list(benchmark), 252),
       stat_coint(list(px), list(py));
```

### 回归与矩阵

```sql
-- 单变量 OLS 回归：y = slope*x + intercept
SELECT stat_regress(list(y), list(x)).slope,
       stat_regress(list(y), list(x)).intercept,
       stat_regress(list(y), list(x)).r2 FROM t;
```

> `stat_regress(...).std_err` 返回**斜率系数的标准误**（`sqrt(mse * (XᵀX)⁻¹[0,0])`），
> 用于对斜率做假设检验（t 值 = slope / std_err），**不是**回归残差的标准差。

-- 多变量 OLS
SELECT stat_ols(list(y), CAST([[1.0,2.0,3.0],[4.0,5.0,6.0]] AS DOUBLE[][])).coefficients;

-- 协方差矩阵 / PCA
SELECT stat_cov_matrix(CAST([[1.0,2.0,3.0],[2.0,4.0,6.0]] AS DOUBLE[][]));
SELECT stat_pca(CAST([[1.0,2.0,3.0],[2.0,4.0,6.0]] AS DOUBLE[][])).eigenvalues;
```

### 高级指标

```sql
-- GARCH 条件波动率 / 趋势强度
SELECT stat_garch_vol(list(close ORDER BY ts), 252, 0.1, 0.85),
       stat_trend_strength(list(close ORDER BY ts)) FROM ohlc;
```

### 滚动统计（窗口版，支持 OVER）

```sql
-- 滚动均值（20 日 SMA）
SELECT stat_avg(close) OVER (ORDER BY ts ROWS BETWEEN 19 PRECEDING AND CURRENT ROW) AS sma20
FROM ohlc;

-- 滚动方差 / 标准差
SELECT stat_rolling_var(close) OVER (ORDER BY ts ROWS BETWEEN 19 PRECEDING AND CURRENT ROW),
       stat_rolling_stddev(close) OVER (ORDER BY ts ROWS BETWEEN 19 PRECEDING AND CURRENT ROW)
FROM ohlc;

-- 滚动 beta / 相关系数
SELECT stat_rolling_beta(stock, market) OVER (ORDER BY ts ROWS BETWEEN 59 PRECEDING AND CURRENT ROW),
       stat_rolling_corr(stock, market) OVER (ORDER BY ts ROWS BETWEEN 59 PRECEDING AND CURRENT ROW)
FROM returns_table;
```

> 完整 `stat_*` 函数清单与签名见 [README](README.md#-eigen-金融统计与回归-stat_)。

---

## 懒人函数 (cn_ta_indicators)

`cn_ta_indicators` 一键生成量化宽表：传入一个 K 线子查询，自动计算全部 TA-Lib 技术指标（单输出 + 多输出）+ A 股适配 + Eigen 统计（共最多 104 列）。

### 基本用法

```sql
-- 直接查询（输出 104 列宽表）
SELECT * FROM cn_ta_indicators((
    SELECT ts, open, high, low, close, volume, amount, outstanding_share, turnover
    FROM minute_bars WHERE symbol = '601919'
));

-- 生成一张表（表名自定义）
CREATE TABLE ind_601919 AS
SELECT * FROM cn_ta_indicators((
    SELECT ts, open, high, low, close, volume, amount, outstanding_share, turnover
    FROM minute_bars WHERE symbol = '601919'
));
```

### 最小输入（仅 5 列必填）

```sql
-- 只有 OHLC，输出 74 列（无 volume/amount 相关指标）
SELECT * FROM cn_ta_indicators((
    SELECT ts, open, high, low, close FROM minute_bars
));
```

### 输出的指标列（自动计算）

```sql
-- 均线：sma_5/10/20/60, ema_5/10/20/60, wma_10, dema_10, tema_10, trima_10, kama_10
-- 动量：rsi_6/12/24, cmo_14, mom_10, roc_10, rocp_10, rocr_10, rocr100_10, trix_12
-- 波动率：atr_14, natr_14, willr_14, cci_14, adx_14, adxr_14, dx_14, plus_di_14, minus_di_14
-- 价格：ad, avgprice, bop, medprice, trange, typprice, wclprice, midprice_14
-- 多输出：bbands_upper/middle/lower（布林带）, macd/macd_signal/macd_hist（MACD）,
--         stoch_k/stoch_d（随机指标）, aroon_down/aroon_up（AROON）,
--         minmax_min/minmax_max, mama/fama, ht_inphase/ht_quadrature, ht_sine/ht_leadsine
-- A股：pct_change（涨跌幅）, volume_ratio（量比）, amount_ma5/10（成交额均线）, turnover_calc（换手率）
-- 整段统计：stat_var, stat_stddev, stat_skew, stat_kurtosis, stat_max_drawdown,
--          stat_sharpe, stat_annual_vol, stat_corr_close_vol, stat_beta,
--          stat_regress_slope/intercept/r2/std_err, stat_trend_strength
-- 滚动统计（窗口20）：roll_avg_20, roll_sum_20, roll_min_20, roll_max_20,
--                    roll_momentum_20, roll_var_20, roll_stddev_20
```

### ⚠️ 按股票分别计算

`cn_ta_indicators` 不按股票分区，一次只应传**一只股票**的数据，否则指标会跨股票串算：

```sql
-- ✅ 正确：WHERE 过滤到单只股票
SELECT * FROM cn_ta_indicators((
    SELECT ts, open, high, low, close, volume FROM minute_bars WHERE symbol = '601919'
));
```

### 查询指定指标列

```sql
-- 只取需要的列
SELECT ts, close, sma_20, rsi_12, atr_14, pct_change, stat_sharpe
FROM cn_ta_indicators((
    SELECT ts, open, high, low, close, volume, amount, outstanding_share, turnover
    FROM minute_bars WHERE symbol = '601919'
));
```

> 性能：in-out TableFunction + TA-Lib 批量计算（O(N)），非逐行窗口重算，性能最优。
