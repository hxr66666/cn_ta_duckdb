# 🇨🇳 cn_ta — A股技术分析 DuckDB 扩展

> 在 SQL 中进行中国 A股技术分析 —— 100+ [TA-Lib](https://ta-lib.org/) 指标、
> 基于 Eigen 的金融统计，以及 A股适配函数（涨跌停、交易日历、ST 过滤）。

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

---

## ⚡ 快速安装

```sql
-- 从本地构建加载
INSTALL 'build/release/extension/cn_ta/cn_ta.duckdb_extension';
LOAD cn_ta;

-- 或使用自托管自定义仓库（任意支持的 DuckDB 版本；未签名）
SET allow_unsigned_extensions = true;
INSTALL cn_ta FROM 'https://your-host/cn_ta';
LOAD cn_ta;
```

## 🚀 快速上手

三种方式在 `ohlc` 上计算 **SMA(14)** —— 按需选择：

```sql
-- 1️⃣ 列表形式（最短最快）：整段序列计算，返回 LIST<DOUBLE>
SELECT ct_sma(list(close ORDER BY date), 14)
FROM ohlc WHERE ticker = 'NVDA';

-- 2️⃣ 窗口有界帧（快，每个输入一行）：推荐用于看板
SELECT date, close,
       cta_sma(close, 14) OVER (ORDER BY date
                               ROWS BETWEEN 13 PRECEDING AND CURRENT ROW) AS sma_14
FROM ohlc WHERE ticker = 'NVDA';

-- 3️⃣ 窗口默认帧（SQL 最短，但 O(N²) —— 大表避免使用）
SELECT date, close,
       cta_sma(close, 14) OVER (ORDER BY date) AS sma_14
FROM ohlc WHERE ticker = 'NVDA';
```

更多示例：

```sql
-- 📐 带 STRUCT 输出的 MACD
SELECT ct_macd(list(close ORDER BY date), 12, 26, 9) FROM ohlc WHERE ticker = 'NVDA';

-- 🕯️ K线形态识别
SELECT ct_cdldoji(list(open ORDER BY date), list(high ORDER BY date),
                 list(low ORDER BY date), list(close ORDER BY date))
FROM ohlc WHERE ticker = 'NVDA';
```

---

## 🔧 函数类型

每个 TA-Lib 函数都注册为**两种形式**：

| 形式 | 前缀 | 用法 | 示例 |
|------|------|------|------|
| **标量（列表）** | `ct_` | 传入预先收集的列表，返回列表 | `ct_sma([1.0, 2.0, 3.0], 2)` |
| **聚合（窗口）** | `cta_` | 配合 `OVER()` 逐行返回结果 | `cta_sma(close, 14) OVER (ORDER BY date ...)` |

### 应该用哪种？

| | 🏎️ 标量 `ct_*` | 🧑‍💻 聚合 `cta_*` |
|---|---|---|
| **性能** | ⚡ 快 —— 全序列一遍扫描，O(N) | 🐢 较慢 —— 每个窗口帧重算，约 O(N × 窗口) |
| **易用性** | 需 `list(col ORDER BY date)` + `unnest` 回连行 | 自然 SQL —— 直接接 `OVER (PARTITION BY … ORDER BY …)` |
| **适用场景** | 回测、全历史特征生成、大数据集 | 看板、临时查询、指标与行级列混用 |

> 💡 **经验法则**：追求速度或整段序列计算时用 `ct_*`；查询读起来更像窗口函数时用 `cta_*`。
>
> **为什么有两个前缀？** DuckDB 要求标量函数和聚合函数使用不同的 SQL 名称。

---

## 📚 支持的函数（100+）

| 类别 | 数量 | 示例 |
|------|------|------|
| 🔀 重叠研究 | 8+ | `ct_sma`, `ct_ema`, `ct_wma`, `ct_dema`, `ct_tema` |
| 🏃 动量 | 15+ | `ct_rsi`, `ct_macd`, `ct_willr`, `ct_cci`, `ct_adx` |
| 🔊 成交量 | 1+ | `ct_ad` |
| 🌊 波动率 | 2+ | `ct_atr`, `ct_natr` |
| 🕯️ K线形态识别 | 49+ | `ct_cdldoji`, `ct_cdlhammer`, `ct_cdlengulfing` |
| 💰 价格变换 | 2+ | `ct_avgprice`, `ct_bop` |
| 🔄 周期指标 | 4+ | `ct_ht_dcperiod`, `ct_ht_trendline`, `ct_ht_trendmode` |
| 📏 统计 | 10+ | `ct_linearreg`, `ct_tsf`, `ct_max`, `ct_min` |
| ➗ 数学变换 | 15 | `ct_sin`, `ct_cos`, `ct_ln`, `ct_sqrt` |
| 🧮 Eigen 统计 | 30+ | `stat_*` 全套金融统计/回归/矩阵/高级指标 |
| 🔄 窗口聚合 | 9 | `stat_avg`, `stat_rolling_var`, `stat_rolling_beta` 等支持 OVER |
| ⏱️ 时间对齐窗口 | 43 | `cta_*_ts` 带时间戳自动对齐（见下文） |

---

## 🧮 Eigen 金融统计与回归 (stat_*)

基于 **Eigen**（C++ 线性代数库）实现的高级金融统计与回归函数库，共 **30+** 个函数，超越 TA-Lib 的单变量回归能力。

> 通用约定：
> - `x`, `y`, `p` 表示 `LIST<DOUBLE>`（把一列数据用 `list(...)` 或 `list_value(...)` 传入）
> - `ppy` = 年化周期数（日频 252、周频 52、月频 12），默认 252
> - 输入不足或除零等非法情况返回 `NULL`

### 一、单变量统计（标量）

| 函数 | 签名 | 使用方法 |
|------|------|----------|
| `stat_var(x)` | `(LIST) -> DOUBLE` | 样本方差。`SELECT stat_var(list(close ORDER BY date)) FROM t;` |
| `stat_stddev(x)` | `(LIST) -> DOUBLE` | 样本标准差。`SELECT stat_stddev(list(returns ORDER BY date));` |
| `stat_skew(x)` | `(LIST) -> DOUBLE` | 偏度（Fisher-Pearson），衡量分布对称性。`SELECT stat_skew(list(returns));` |
| `stat_kurtosis(x)` | `(LIST) -> DOUBLE` | 超额峰度（正态分布为 0）。`SELECT stat_kurtosis(list(returns));` |
| `stat_max_drawdown(p)` | `(LIST) -> DOUBLE` | 最大回撤（0-1 比例）。`SELECT stat_max_drawdown(list(close ORDER BY date));` |
| `stat_max_drawdown_duration(p)` | `(LIST) -> DOUBLE` | 最大回撤持续期（周期数）。`SELECT stat_max_drawdown_duration(list(close));` |
| `stat_sharpe(r, rf, ppy)` | `(LIST, DOUBLE, DOUBLE) -> DOUBLE` | 夏普比率 = (均值-无风险)/标准差×√ppy。`SELECT stat_sharpe(list(returns), 0.02, 252);` |
| `stat_sortino(r, target, ppy)` | `(LIST, DOUBLE, DOUBLE) -> DOUBLE` | Sortino 比率，只惩罚下行波动。`SELECT stat_sortino(list(returns), 0.0, 252);` |
| `stat_calmar(r, ppy)` | `(LIST, DOUBLE) -> DOUBLE` | Calmar 比率 = 年化收益/最大回撤。`SELECT stat_calmar(list(returns), 252);` |
| `stat_annual_vol(r, ppy)` | `(LIST, DOUBLE) -> DOUBLE` | 年化波动率。`SELECT stat_annual_vol(list(returns), 252);` |
| `stat_ewma_vol(r, lambda)` | `(LIST, DOUBLE) -> DOUBLE` | EWMA 波动率，lambda 默认 0.94。`SELECT stat_ewma_vol(list(returns), 0.94);` |
| `stat_var_historical(r, alpha)` | `(LIST, DOUBLE) -> DOUBLE` | 历史 VaR，alpha 默认 0.05（5% 分位）。`SELECT stat_var_historical(list(returns), 0.05);` |

### 二、双变量统计（标量）

| 函数 | 签名 | 使用方法 |
|------|------|----------|
| `stat_cov(x, y)` | `(LIST, LIST) -> DOUBLE` | 样本协方差。`SELECT stat_cov(list(x), list(y));` |
| `stat_corr(x, y)` | `(LIST, LIST) -> DOUBLE` | 皮尔逊相关系数（-1~1）。`SELECT stat_corr(list(close), list(volume));` |
| `stat_beta(x, y)` | `(LIST, LIST) -> DOUBLE` | CAPM beta = cov(x,y)/var(y)（y 为市场）。`SELECT stat_beta(list(stock_ret), list(market_ret));` |
| `stat_information_ratio(r, b, ppy)` | `(LIST, LIST, DOUBLE) -> DOUBLE` | 信息比率 = 超额收益/跟踪误差。`SELECT stat_information_ratio(list(fund), list(benchmark), 252);` |
| `stat_coint(x, y)` | `(LIST, LIST) -> DOUBLE` | Engle-Granger 协整检验，返回 ADF t 统计量（负值越大越协整）。`SELECT stat_coint(list(px), list(py));` |

### 三、回归

| 函数 | 签名 | 使用方法 |
|------|------|----------|
| `stat_regress(y, x)` | `(LIST, LIST) -> STRUCT{slope, intercept, r2, std_err}` | 单变量 OLS 回归。`SELECT stat_regress(list(y), list(x)).slope AS slope, ...r2 AS r2;` |
| `stat_ols(y, X)` | `(LIST, LIST<LIST>) -> STRUCT{coefficients, intercept, r2}` | 多变量 OLS。`SELECT stat_ols(list(y), CAST([[...],[...]] AS DOUBLE[][])).coefficients;` |

> 💡 **`std_err` 字段说明**：`stat_regress(...).std_err` 返回的是**斜率系数的标准误**
> （standard error of the slope coefficient），即 `sqrt(mse * (XᵀX)⁻¹[0,0]`，
> 用于对斜率做假设检验（t 值 = slope / std_err）。它**不是**回归残差的标准差
> （residual standard error）。如需残差标准差，可自行计算 `sqrt(ss_res / (n-2))`。

### 四、矩阵分析

| 函数 | 签名 | 使用方法 |
|------|------|----------|
| `stat_cov_matrix(X)` | `(LIST<LIST>) -> LIST<LIST>` | 样本协方差矩阵。`SELECT stat_cov_matrix(CAST([[1,2],[3,4]] AS DOUBLE[][]));` |
| `stat_pca(X)` | `(LIST<LIST>) -> STRUCT{eigenvalues, eigenvectors}` | PCA 特征分解。`SELECT stat_pca(...).eigenvalues AS evals;` |

### 五、高级指标（标量）

| 函数 | 签名 | 使用方法 |
|------|------|----------|
| `stat_garch_vol(p, ppy, alpha, beta)` | `(LIST, DOUBLE, DOUBLE, DOUBLE) -> DOUBLE` | GARCH(1,1) 条件波动率。`SELECT stat_garch_vol(list(close), 252, 0.1, 0.85);` |
| `stat_rsi(p, period)` | `(LIST, DOUBLE) -> DOUBLE` | RSI（Wilder 平滑），返回 0-100。`SELECT stat_rsi(list(close), 14);` |
| `stat_trend_strength(p)` | `(LIST) -> DOUBLE` | 趋势强度 = 线性回归 R²（0~1）。`SELECT stat_trend_strength(list(close));` |
| `stat_kalman(p, q, r)` | `(LIST, DOUBLE, DOUBLE) -> LIST` | 卡尔曼平滑去噪（标量随机游走模型），返回等长平滑序列。q=过程噪声（跟随性），r=观测噪声（平滑强度）。`SELECT stat_kalman(list(close), 0.01, 1.0);` |
| `stat_dtw(a, b, window)` | `(LIST, LIST, DOUBLE) -> STRUCT{distance, lag, similarity}` | DTW 形态匹配 + 滞后偏移检测。distance=累积距离（越小越相似），lag=最优滞后天数（正=b 滞后 a），similarity=归一化相似度 0~1。window 为 Sakoe-Chiba 带宽（<=0 表示不限）。`SELECT stat_dtw(list(close_a), list(close_b), 5);` |
| `stat_resample(p, n)` | `(LIST, INTEGER) -> LIST` | 重采样 + z-score 归一化（线性插值）。过滤 NaN 后按总体标准差归一化（消除价格量纲，不同价位的股票可直接比较形态），再线性插值到固定 n 个点，输出形态向量（均值≈0、标准差≈1），可直接 `CAST(... AS FLOAT[n])` 喂给 VSS。有效点 < 2、n < 2 或常数序列返回 NULL。`SELECT stat_resample(list(close), 128);` |

### 六、聚合/窗口版本（支持 OVER）

逐行输入，配合 `OVER (... ROWS BETWEEN n PRECEDING AND CURRENT ROW)` 得到滚动统计量。

| 函数 | 签名 | 使用方法 |
|------|------|----------|
| `stat_avg(x)` | `(DOUBLE) -> DOUBLE` | 滚动均值。`SELECT stat_avg(close) OVER (ORDER BY date ROWS BETWEEN 19 PRECEDING AND CURRENT ROW);` |
| `stat_sum(x)` | `(DOUBLE) -> DOUBLE` | 滚动求和。`SELECT stat_sum(vol) OVER (... ROWS BETWEEN 9 PRECEDING AND CURRENT ROW);` |
| `stat_min(x)` | `(DOUBLE) -> DOUBLE` | 滚动最小值。`SELECT stat_min(low) OVER (...);` |
| `stat_max(x)` | `(DOUBLE) -> DOUBLE` | 滚动最大值。`SELECT stat_max(high) OVER (...);` |
| `stat_momentum(x)` | `(DOUBLE) -> DOUBLE` | 滚动动量 (last-first)/\|first\|。`SELECT stat_momentum(close) OVER (...);` |
| `stat_rolling_var(x)` | `(DOUBLE) -> DOUBLE` | 滚动方差。`SELECT stat_rolling_var(close) OVER (...);` |
| `stat_rolling_stddev(x)` | `(DOUBLE) -> DOUBLE` | 滚动标准差。`SELECT stat_rolling_stddev(close) OVER (...);` |
| `stat_rolling_beta(x, y)` | `(DOUBLE, DOUBLE) -> DOUBLE` | 滚动 beta。`SELECT stat_rolling_beta(stock, market) OVER (...);` |
| `stat_rolling_corr(x, y)` | `(DOUBLE, DOUBLE) -> DOUBLE` | 滚动相关系数。`SELECT stat_rolling_corr(x, y) OVER (...);` |

> 说明：聚合版函数名与标量版不同（如 `stat_rolling_var` vs `stat_var`），以避免 DuckDB 标量/聚合重载冲突。

### 完整示例

```sql
-- 单变量回归：y = 2x + 0
SELECT stat_regress(list(close ORDER BY date), list(volume ORDER BY date)).slope FROM t;

-- 多变量 OLS：y = 2*x1 + 3*x2 + 1
SELECT stat_ols(list_value(6.0,17.0,34.0,57.0),
                CAST([[1.0,2.0,3.0,4.0],[1.0,4.0,9.0,16.0]] AS DOUBLE[][])).coefficients;

-- 协方差矩阵
SELECT stat_cov_matrix(CAST([[1.0,2.0,3.0,4.0],[2.0,4.0,6.0,8.0]] AS DOUBLE[][]));

-- PCA（特征分解）
SELECT stat_pca(CAST([[1.0,2.0,3.0],[2.0,4.0,6.0]] AS DOUBLE[][])).eigenvalues;

-- 滚动均值（20 日 SMA，用 OVER）
SELECT stat_avg(close) OVER (ORDER BY date ROWS BETWEEN 19 PRECEDING AND CURRENT ROW) AS sma20
FROM ohlc;

-- 滚动 beta（60 日）
SELECT stat_rolling_beta(stock_ret, market_ret)
       OVER (ORDER BY date ROWS BETWEEN 59 PRECEDING AND CURRENT ROW) AS beta60
FROM returns;
```

---

## 🇨🇳 A股适配函数 (cn_*)

面向沪深/北交所市场的特有适配函数。

| 函数 | 签名 | 使用方法 |
|------|------|----------|
| `cn_limit_pct(code, is_st)` | `(VARCHAR, BOOLEAN) -> DOUBLE` | 涨跌停幅度。`SELECT cn_limit_pct('600000', false); -- 0.1` |
| `cn_limit_price(prev_close, code, is_st)` | `(DOUBLE, VARCHAR, BOOLEAN) -> STRUCT{limit_up, limit_down}` | 涨跌停价。`SELECT cn_limit_price(10.0, '600000', false).limit_up; -- 11.0` |
| `cn_is_st(name)` | `(VARCHAR) -> BOOLEAN` | 是否 ST/*ST。`SELECT cn_is_st('ST中葡'); -- true` |
| `cn_change_pct(close, prev_close)` | `(DOUBLE, DOUBLE) -> DOUBLE` | 涨跌幅。`SELECT cn_change_pct(10.5, 10.0); -- 0.05` |
| `cn_turnover(volume, float_shares)` | `(DOUBLE, DOUBLE) -> DOUBLE` | 换手率。`SELECT cn_turnover(500000, 1000000); -- 0.5` |
| `cn_volume_ratio(volume, hist_vol)` | `(DOUBLE, LIST) -> DOUBLE` | 量比（当前量/历史均量）。`SELECT cn_volume_ratio(1000000, list_value(500000,600000,400000));` |
| `cn_pe_percentile(pe, pe_hist)` | `(DOUBLE, LIST) -> DOUBLE` | 市盈率历史分位（0-1）。`SELECT cn_pe_percentile(25, list_value(10,15,20,30,40)); -- 0.6` |
| `cn_adjust_price(close, factor)` | `(DOUBLE, DOUBLE) -> DOUBLE` | 除权除息复权价。`SELECT cn_adjust_price(10.0, 1.2); -- 12.0` |
| `cn_trading_day(date)` | `(DATE) -> BOOLEAN` | 是否交易日（排除周末+法定节假日，内置 2024-2026）。`SELECT cn_trading_day(DATE '2024-10-01'); -- false` |

**涨跌停规则**（按板块自动判断）：

| 板块 | 代码前缀 | 涨跌幅 |
|------|---------|--------|
| 沪深主板 | 60xxxx / 00xxxx | ±10%（ST ±5%） |
| 创业板 | 30xxxx | ±20% |
| 科创板 | 688xxx | ±20% |
| 北交所 | 8xxxxx / 4xxxxx / 92xxxx | ±30% |

> 注：`cn_trading_day` 内置 2024-2026 法定节假日表（春节、清明、五一、端午、中秋、国庆、元旦）。调休补班（周末上班日）需接入完整交易日历数据。

---

## ⏱️ 时间对齐窗口函数 (cta_*_ts)

`cta_*_ts` 是带**时间戳自动对齐**的窗口聚合函数，用于解决分钟 K 数据缺口导致的指标错位问题。

普通 `cta_*` 按"数组元素相对顺序"滑动，当分钟 K 存在缺口（收盘集合竞价 14:58/14:59、盘中停牌、节假日休市）时，缺口会被"挤掉"，导致指标值与日历时间错位。`cta_*_ts` 在扩展内部维护交易日 bar 网格 + 前值填充（ffill），使指标严格对齐到日历时间。

### 核心特性

1. **交易日历**：内置 2020~2026 A股法定节假日，自动排除周末、午休（11:31–12:59）、收盘竞价（14:58/14:59）。
2. **K 线周期**：可选 `bar_period` 参数（默认 1 分钟），支持 5/15/30 分钟等。
3. **多输入**：覆盖 P1/P3/P4/P5/P6/P7/P8 全部模式。

### 使用示例

```sql
-- 1 分钟对齐（默认）
SELECT ts,
       cta_sma_ts(ts, close, 20) OVER (PARTITION BY symbol ORDER BY ts) AS sma20,
       cta_ema_ts(ts, close, 20) OVER (PARTITION BY symbol ORDER BY ts) AS ema20
FROM minute_bars ORDER BY symbol, ts;

-- 5 分钟 K 对齐（第 4 个参数 bar_period=5）
SELECT cta_sma_ts(ts, close, 20, 5) OVER (PARTITION BY symbol ORDER BY ts) AS sma20_5min
FROM minute_bars;

-- 多输入：ATR（P3）、AD（P4）、均价值（P6）
SELECT cta_atr_ts(ts, high, low, close, 14)   OVER (PARTITION BY symbol ORDER BY ts) AS atr14,
       cta_ad_ts(ts, high, low, close, volume) OVER (PARTITION BY symbol ORDER BY ts) AS ad,
       cta_medprice_ts(ts, high, low)          OVER (PARTITION BY symbol ORDER BY ts) AS medprice
FROM minute_bars;
```

### 函数签名（按模式）

| 模式 | 签名 | 覆盖指标 |
|------|------|---------|
| P1 | `cta_<name>_ts(ts, value, period[, bar_period])` | sma/ema/wma/dema/tema/trima/kama/rsi/cmo/mom/roc 系列/linearreg 系列/tsf/sum/max/min 等 24 个 |
| P3 | `cta_<name>_ts(ts, high, low, close, period[, bar_period])` | atr/natr/willr/cci/adx/adxr/dx/plus_di/minus_di |
| P4 | `cta_<name>_ts(ts, high, low, close, volume[, bar_period])` | ad |
| P5 | `cta_<name>_ts(ts, open, high, low, close[, bar_period])` | avgprice/bop |
| P6 | `cta_<name>_ts(ts, high, low[, bar_period])` | medprice |
| P7 | `cta_<name>_ts(ts, high, low, close[, bar_period])` | trange/typprice/wclprice |
| P8 | `cta_<name>_ts(ts, high, low, period[, bar_period])` | midprice/plus_dm/minus_dm |

### 交易日历辅助函数

| 函数 | 签名 | 说明 |
|------|------|------|
| `cn_ta_is_trading_day(ts)` | `(TIMESTAMP) -> BOOLEAN` | 是否交易日（周一~周五 + 非节假日）。`SELECT cn_ta_is_trading_day(TIMESTAMP '2026-10-01 10:00:00'); -- false` |
| `cn_ta_set_holiday(date)` | `(DATE) -> BIGINT` | 追加一个用户自定义休市日（返回 1=新增，0=重复）。`SELECT cn_ta_set_holiday(DATE '2026-10-09');` |
| `cn_ta_clear_holidays()` | `() -> BIGINT` | 清空所有用户自定义休市日。`SELECT cn_ta_clear_holidays();` |

> 💡 **对齐语义**：缺口处用"上一个有效值"前值填充（ffill），等价于停牌/无成交期间价格不变（量化行业标准）。缺口处 volume 填 0。

---

## 🚀 懒人函数 (cn_ta_indicators)

**`cn_ta_indicators`** 是"一键生成量化宽表"的表函数：传入一个 K 线子查询，自动批量计算全部可计算的指标，输出一张宽表。

### 用法

```sql
-- 直接查询
SELECT * FROM cn_ta_indicators((
    SELECT 日期, 开盘, 最高, 最低, 收盘, 成交量, amount, outstanding_share, turnover
    FROM OHLC_xxx WHERE xxx = xx
));

-- 生成一张表（表名自定义）
CREATE TABLE my_indicators AS
SELECT * FROM cn_ta_indicators((
    SELECT 日期, 开盘, 最高, 最低, 收盘, 成交量, amount, outstanding_share, turnover
    FROM OHLC_xxx WHERE xxx = xx
));
```

### 输入列（按顺序，前 5 列必填，后 4 列可选）

| 序号 | 列名 | 类型 | 必填 | 说明 |
|------|------|------|------|------|
| 1 | 日期 | TIMESTAMP | ✅ | 时间戳 |
| 2 | 开盘 | DOUBLE | ✅ | 开盘价 |
| 3 | 最高 | DOUBLE | ✅ | 最高价 |
| 4 | 最低 | DOUBLE | ✅ | 最低价 |
| 5 | 收盘 | DOUBLE | ✅ | 收盘价 |
| 6 | 成交量 | DOUBLE | ⬜ | 成交量 |
| 7 | amount | DOUBLE | ⬜ | 成交额（用于 amount_ma 均线） |
| 8 | outstanding_share | DOUBLE | ⬜ | 流通股本（用于换手率重算） |
| 9 | turnover | DOUBLE | ⬜ | 换手率（透传） |

### 输出列（按输入列数自适应，最多 104 列）

| 类别 | 列数 | 内容 |
|------|------|------|
| 原始列 | 5~9 | 原样透传输入 |
| TA-Lib 单输出指标 | 51 | 重叠/动量/波动率/成交量/价格变换/统计（见下） |
| TA-Lib 多输出指标 | 18 | 布林带/MACD/随机指标/AROON/MINMAX/MAMA/HT 相位/HT 正弦 |
| A 股适配 | 5 | `pct_change`（涨跌幅）、`volume_ratio`（量比）、`amount_ma5/10`（成交额均线）、`turnover_calc`（换手率重算） |
| 整段统计（常量列） | 14 | `stat_var/stddev/skew/kurtosis/max_drawdown/sharpe/annual_vol/corr/beta/regress_*`（每行同值） |
| 滚动统计（窗口 20） | 7 | `roll_avg/sum/min/max/momentum/var/stddev` |

**TA-Lib 单输出指标（51 列）明细**：
- 均线：`sma_5/10/20/60`、`ema_5/10/20/60`、`wma_10`、`dema_10`、`tema_10`、`trima_10`、`kama_10`、`midpoint_10`
- 动量：`rsi_6/12/24`、`cmo_14`、`mom_10`、`roc_10`、`rocp_10`、`rocr_10`、`rocr100_10`、`trix_12`
- 回归/统计：`linearreg_14`、`linearreg_slope_14`、`linearreg_intercept_14`、`linearreg_angle_14`、`tsf_14`、`sum_10`、`max_10`、`min_10`
- 波动率：`atr_14`、`natr_14`、`willr_14`、`cci_14`、`adx_14`、`adxr_14`、`dx_14`、`plus_di_14`、`minus_di_14`
- 成交量/价格：`ad`、`avgprice`、`bop`、`medprice`、`trange`、`typprice`、`wclprice`、`midprice_14`、`plus_dm_14`、`minus_dm_14`

**TA-Lib 多输出指标（18 列）明细**：
- 布林带：`bbands_upper`、`bbands_middle`、`bbands_lower`（close, 20, 2.0, 2.0）
- MACD：`macd`、`macd_signal`、`macd_hist`（close, 12, 26, 9）
- 随机指标：`stoch_k`、`stoch_d`（HLC, 5, 3）
- AROON：`aroon_down`、`aroon_up`（HL, 14）
- 极值：`minmax_min`、`minmax_max`（close, 20）
- 自适应均线：`mama`、`fama`（close, 0.5, 0.05）
- 希尔伯特相位：`ht_inphase`、`ht_quadrature`（close）
- 希尔伯特正弦：`ht_sine`、`ht_leadsine`（close）

### 性能

采用 **in-out TableFunction + 全局累积 + TA-Lib 批量计算**（O(N)），**不是逐行窗口重算**，性能与标量 `ct_*` 函数同级，远快于逐列 `cta_*` 窗口函数。

实测（A 股分钟线）：49 只股票 96530 行 × 104 列约 0.35 秒；单只 1970 行 × 104 列约 17ms。

### ⚠️ 使用注意：按股票分别计算

`cn_ta_indicators` 是**整段序列计算，不按股票分区**。若一次传入多只股票，指标会**跨股票连续计算**（如 SMA 前 20 个值会混入前一只股票的数据），产生错误结果。

```sql
-- ✅ 正确：每只股票单独过滤
SELECT * FROM cn_ta_indicators((
    SELECT ts, open, high, low, close, volume FROM ticks WHERE symbol = '601919'
));

-- ❌ 错误：直接传全表（多只股票会串算）
SELECT * FROM cn_ta_indicators((
    SELECT ts, open, high, low, close, volume FROM ticks
));
```

---

## 🧩 多输出函数

MACD、布林带等函数返回 `LIST<STRUCT>` 类型：

```sql
-- 📐 MACD → LIST<STRUCT(macd, signal, hist)>
SELECT ct_macd(list(close ORDER BY date), 12, 26, 9) FROM ohlc;

-- 📊 BBANDS → LIST<STRUCT(upper, middle, lower)>
SELECT ct_bbands(list(close ORDER BY date), 20, 2.0, 2.0, 0) FROM ohlc;

-- 📈 STOCH → LIST<STRUCT(slowk, slowd)>
SELECT ct_stoch(list(high ORDER BY date), list(low ORDER BY date),
                list(close ORDER BY date), 5, 3, 0, 3, 0) FROM ohlc;

-- 🏹 AROON → LIST<STRUCT(aroon_down, aroon_up)>
SELECT ct_aroon(list(high ORDER BY date), list(low ORDER BY date), 14) FROM ohlc;
```

---

## 🗺️ 输入模式

| 模式 | 标量签名 | 示例 |
|------|---------|------|
| P1 | `(LIST<DOUBLE>, INTEGER)` | `ct_sma(values, period)` |
| P2 | `(LIST<DOUBLE>)` | `ct_sin(values)` |
| P3 | `(LIST<DOUBLE> x3, INTEGER)` | `ct_willr(high, low, close, period)` |
| P4 | `(LIST<DOUBLE> x4)` | `ct_ad(high, low, close, volume)` |
| P5 | `(LIST<DOUBLE> x4)` | `ct_cdldoji(open, high, low, close)` |
| P6 | `(LIST<DOUBLE> x2)` | `ct_medprice(high, low)` |
| P7 | `(LIST<DOUBLE> x3)` | `ct_typprice(high, low, close)` |
| P8 | `(LIST<DOUBLE> x2, INTEGER)` | `ct_midprice(high, low, period)` |

---

## 🖥️ 平台支持

> ✅ **DuckDB 版本：** **v1.5.2, v1.5.3, v1.5.4, v1.5.5**（每个版本一个发布分支）。自定义仓库 URL 会自动解析到你正在运行的版本。

### 支持的平台

| 平台 | 架构 |
|------|------|
| 🐧 Linux | x86_64, aarch64 |
| 🍎 macOS | x86_64, arm64 |

---

## 🛠️ 从源码构建

CMake 使用标准 `find_path`/`find_library` 查找 `talib` 和 `eigen3`。依赖安装策略按平台：

### Linux

使用系统包（推荐，可靠）：

```bash
sudo apt install ta-lib-dev libeigen3-dev
.make        # 构建
make test             # 运行测试
```

### Windows（MSVC）

使用 vcpkg（`talib` 官方 port 原生支持 Windows，解决编译困难）：

```powershell
$env:VCPKG_ROOT = "C:\vcpkg"
$env:VCPKG_TOOLCHAIN_PATH = "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
# 依赖在 vcpkg.json 声明（talib + eigen3），构建时自动安装
GEN=ninja make
```

> Windows 下 CMake 会自动链接 talib 官方 port 的库（`ta_libc`/`ta_func`/`ta_abstract`/`ta_common`）。

### 依赖策略小结

| 平台 | talib | eigen3 |
|------|-------|--------|
| Linux | 系统包 `ta-lib-dev` | 系统包 `libeigen3-dev` |
| Windows | vcpkg（官方 port 支持 Windows）| vcpkg（`eigen3` port）|
| macOS | 系统/Homebrew `ta-lib` | Homebrew `eigen` |

**前置条件：** CMake 3.5+，C++17 编译器，git

---

## 📖 文档

- [函数参考](index.md) — 所有函数及参数和类型
- [SQL Cookbook](cookbook.md) — 每个函数的示例

---

## 📄 许可证

MIT
