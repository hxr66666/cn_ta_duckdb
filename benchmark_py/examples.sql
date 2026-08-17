-- ============================================================
-- cn_ta 扩展 SQL 示例（带中文注释）
-- ============================================================
-- 运行方式：
--   LOAD 'build/release/extension/cn_ta/cn_ta.duckdb_extension';
--   （本文件可直接用 duckdb CLI 或 benchmark_py 的 examples.py 运行）
--
-- 说明：
--   * ct_*   标量函数，输入 LIST<DOUBLE>，一次性算完整段序列
--   * cta_*  聚合/窗口函数，配合 OVER() 逐行输出
--   * cta_*_ts  带时间戳自动对齐的窗口函数（缺口 ffill）
--   * stat_* Eigen 金融统计与回归
--   * cn_*   A股适配函数
--
-- 完整覆盖全部函数：运行 `uv run --directory ./benchmark_py examples`
-- ============================================================

-- ------------------------------------------------------------
-- 0. 示例数据表（OHLCV + 时间戳）
-- ------------------------------------------------------------
CREATE OR REPLACE TABLE ohlc AS
SELECT * FROM (VALUES
    ('2026-08-05 09:31:00'::TIMESTAMP, 10.0, 11.2,  9.8, 10.5, 120000.0),
    ('2026-08-05 09:32:00'::TIMESTAMP, 10.5, 11.8, 10.2, 11.3, 135000.0),
    ('2026-08-05 09:33:00'::TIMESTAMP, 11.3, 12.1, 11.0, 11.8,  98000.0),
    ('2026-08-05 09:34:00'::TIMESTAMP, 11.8, 12.5, 11.5, 12.2, 145000.0),
    ('2026-08-05 09:35:00'::TIMESTAMP, 12.2, 13.0, 11.9, 12.7, 167000.0),
    ('2026-08-06 09:31:00'::TIMESTAMP, 12.7, 13.4, 12.3, 13.1, 112000.0),
    ('2026-08-06 09:32:00'::TIMESTAMP, 13.1, 13.8, 12.8, 13.5, 130000.0),
    ('2026-08-06 09:33:00'::TIMESTAMP, 13.5, 14.2, 13.1, 13.9, 118000.0),
    ('2026-08-06 09:34:00'::TIMESTAMP, 13.9, 14.6, 13.5, 14.3, 155000.0),
    ('2026-08-06 09:35:00'::TIMESTAMP, 14.3, 15.0, 13.9, 14.8, 142000.0)
) AS t(ts, open, high, low, close, volume);


-- ------------------------------------------------------------
-- 一、重叠研究指标（Overlap Studies）—— ct_* 标量形式
-- 用法：list(col ORDER BY ts) 把整列收集成 LIST 传入，返回 LIST
-- ------------------------------------------------------------

-- SMA 简单移动平均（序列, 周期）
SELECT ct_sma(list(close ORDER BY ts), 5) AS sma5 FROM ohlc;

-- EMA 指数移动平均
SELECT ct_ema(list(close ORDER BY ts), 5) AS ema5 FROM ohlc;

-- WMA 加权移动平均 / DEMA 双指数 / TEMA 三指数 / TRIMA 三角
SELECT ct_wma(list(close ORDER BY ts), 5),
       ct_dema(list(close ORDER BY ts), 5),
       ct_tema(list(close ORDER BY ts), 5),
       ct_trima(list(close ORDER BY ts), 5) FROM ohlc;

-- KAMA 自适应移动平均
SELECT ct_kama(list(close ORDER BY ts), 5) FROM ohlc;

-- MAMA 自适应均线（序列, 快慢因子 0.5, 0.05）→ STRUCT(mama, fama)
SELECT ct_mama(list(close ORDER BY ts), 0.5, 0.05) FROM ohlc;

-- 中点（序列, 周期）
SELECT ct_midpoint(list(close ORDER BY ts), 5) FROM ohlc;

-- 线性回归（序列, 周期）及变体
SELECT ct_linearreg(list(close ORDER BY ts), 5),
       ct_linearreg_slope(list(close ORDER BY ts), 5),
       ct_linearreg_intercept(list(close ORDER BY ts), 5),
       ct_linearreg_angle(list(close ORDER BY ts), 5) FROM ohlc;

-- 时间序列预测 TSF
SELECT ct_tsf(list(close ORDER BY ts), 5) FROM ohlc;


-- ------------------------------------------------------------
-- 二、动量指标（Momentum）—— cta_* 窗口形式
-- 用法：配合 OVER (ORDER BY ts) 逐行输出
-- ------------------------------------------------------------

-- RSI 相对强弱（close, 周期 14）
SELECT ts, close, cta_rsi(close, 14) OVER (ORDER BY ts) AS rsi14 FROM ohlc;

-- CMO 钱德动量震荡 / MOM 动量 / ROC 变动率 / ROCP / ROCR / ROCR100
SELECT ts, cta_cmo(close, 14) OVER (ORDER BY ts),
           cta_mom(close, 10) OVER (ORDER BY ts),
           cta_roc(close, 10)  OVER (ORDER BY ts),
           cta_rocp(close, 10) OVER (ORDER BY ts),
           cta_rocr(close, 10) OVER (ORDER BY ts),
           cta_rocr100(close, 10) OVER (ORDER BY ts) FROM ohlc;

-- TRIX 三重指数平滑
SELECT cta_trix(close, 15) OVER (ORDER BY ts) FROM ohlc;

-- WILLR 威廉指标（high, low, close, 周期）
SELECT cta_willr(high, low, close, 14) OVER (ORDER BY ts) FROM ohlc;

-- CCI 顺势指标（high, low, close, 周期）
SELECT cta_cci(high, low, close, 20) OVER (ORDER BY ts) FROM ohlc;

-- ADX / ADXR / DX / +DI / -DI（high, low, close, 周期）
SELECT cta_adx(high, low, close, 14) OVER (ORDER BY ts),
       cta_adxr(high, low, close, 14) OVER (ORDER BY ts),
       cta_dx(high, low, close, 14) OVER (ORDER BY ts),
       cta_plus_di(high, low, close, 14) OVER (ORDER BY ts),
       cta_minus_di(high, low, close, 14) OVER (ORDER BY ts) FROM ohlc;

-- MACD（close, 快 12, 慢 26, 信号 9）→ STRUCT(macd, signal, hist)
SELECT ts, (cta_macd(close, 12, 26, 9) OVER (ORDER BY ts)).macd  AS macd,
           (cta_macd(close, 12, 26, 9) OVER (ORDER BY ts)).signal AS signal,
           (cta_macd(close, 12, 26, 9) OVER (ORDER BY ts)).hist AS hist FROM ohlc;

-- STOCH 随机指标（h,l,c, 快K 5, 慢K 3, 慢K平滑 0, 慢D 3, 慢D平滑 0）
SELECT (cta_stoch(high, low, close, 5, 3, 0, 3, 0) OVER (ORDER BY ts)).slowk,
       (cta_stoch(high, low, close, 5, 3, 0, 3, 0) OVER (ORDER BY ts)).slowd FROM ohlc;

-- AROON（high, low, 周期）→ STRUCT(aroon_down, aroon_up)
SELECT (cta_aroon(high, low, 14) OVER (ORDER BY ts)).aroon_down,
       (cta_aroon(high, low, 14) OVER (ORDER BY ts)).aroon_up FROM ohlc;


-- ------------------------------------------------------------
-- 三、成交量指标（Volume）
-- ------------------------------------------------------------

-- AD 累积派发线（high, low, close, volume）—— 无周期
SELECT ts, cta_ad(high, low, close, volume) OVER (ORDER BY ts) AS ad_line FROM ohlc;


-- ------------------------------------------------------------
-- 四、波动率指标（Volatility）
-- ------------------------------------------------------------

-- ATR 平均真实波幅（high, low, close, 周期）
SELECT cta_atr(high, low, close, 14) OVER (ORDER BY ts) FROM ohlc;

-- NATR 归一化 ATR
SELECT cta_natr(high, low, close, 14) OVER (ORDER BY ts) FROM ohlc;

-- TRANGE 真实波幅（high, low, close）—— 无周期
SELECT cta_trange(high, low, close) OVER (ORDER BY ts) FROM ohlc;


-- ------------------------------------------------------------
-- 五、价格变换（Price Transform）
-- ------------------------------------------------------------

-- AVGPRICE 均价（open, high, low, close）=(O+H+L+C)/4
SELECT cta_avgprice(open, high, low, close) OVER (ORDER BY ts) FROM ohlc;

-- BOP 均势（open, high, low, close）
SELECT cta_bop(open, high, low, close) OVER (ORDER BY ts) FROM ohlc;

-- MEDPRICE 中价（high, low）=(H+L)/2
SELECT cta_medprice(high, low) OVER (ORDER BY ts) FROM ohlc;

-- TYPPRICE 典型价（high, low, close）=(H+L+C)/3
SELECT cta_typprice(high, low, close) OVER (ORDER BY ts) FROM ohlc;

-- WCLPRICE 加权收盘价（high, low, close）=(H+L+2C)/4
SELECT cta_wclprice(high, low, close) OVER (ORDER BY ts) FROM ohlc;

-- MIDPRICE 区间中价（high, low, 周期）
SELECT cta_midprice(high, low, 14) OVER (ORDER BY ts) FROM ohlc;

-- 布尔带 BBANDS（close, 周期, 上偏 2.0, 下偏 2.0, MA类型 0=SMA）
SELECT (cta_bbands(close, 20, 2.0, 2.0, 0) OVER (ORDER BY ts)).upper,
       (cta_bbands(close, 20, 2.0, 2.0, 0) OVER (ORDER BY ts)).middle,
       (cta_bbands(close, 20, 2.0, 2.0, 0) OVER (ORDER BY ts)).lower FROM ohlc;


-- ------------------------------------------------------------
-- 六、周期指标（Cycle）—— 希尔伯特变换
-- ------------------------------------------------------------

-- HT 趋势线 / 主周期 / 相位 / 趋势模式
SELECT cta_ht_trendline(close) OVER (ORDER BY ts),
       cta_ht_dcperiod(close) OVER (ORDER BY ts),
       cta_ht_dcphase(close) OVER (ORDER BY ts),
       cta_ht_trendmode(close) OVER (ORDER BY ts) FROM ohlc;

-- HT 相量（inphase, quadrature）/ 正弦（sine, leadsine）
SELECT (cta_ht_phasor(close) OVER (ORDER BY ts)).inphase,
       (cta_ht_phasor(close) OVER (ORDER BY ts)).quadrature,
       (cta_ht_sine(close) OVER (ORDER BY ts)).sine,
       (cta_ht_sine(close) OVER (ORDER BY ts)).leadsine FROM ohlc;


-- ------------------------------------------------------------
-- 七、统计函数（Statistic）—— 极值/索引
-- ------------------------------------------------------------

-- MAX / MIN / SUM / MAXINDEX / MININDEX / MINMAX
SELECT cta_max(close, 10) OVER (ORDER BY ts),
       cta_min(close, 10) OVER (ORDER BY ts),
       cta_sum(close, 10) OVER (ORDER BY ts),
       cta_maxindex(close, 10) OVER (ORDER BY ts),
       cta_minindex(close, 10) OVER (ORDER BY ts) FROM ohlc;

-- MINMAX → STRUCT(min, max)
SELECT (cta_minmax(close, 10) OVER (ORDER BY ts)).min,
       (cta_minmax(close, 10) OVER (ORDER BY ts)).max FROM ohlc;


-- ------------------------------------------------------------
-- 八、数学变换（Math Transform）—— ct_* 标量
-- ------------------------------------------------------------

-- 三角函数 / 双曲函数 / 对数 / 指数 / 取整 / 开方
SELECT ct_sin(list(close ORDER BY ts)),
       ct_cos(list(close ORDER BY ts)),
       ct_tan(list(close ORDER BY ts)),
       ct_asin(list(close ORDER BY ts)),
       ct_acos(list(close ORDER BY ts)),
       ct_atan(list(close ORDER BY ts)),
       ct_sinh(list(close ORDER BY ts)),
       ct_cosh(list(close ORDER BY ts)),
       ct_tanh(list(close ORDER BY ts)),
       ct_ln(list(close ORDER BY ts)),
       ct_log10(list(close ORDER BY ts)),
       ct_exp(list(close ORDER BY ts)),
       ct_sqrt(list(close ORDER BY ts)),
       ct_ceil(list(close ORDER BY ts)),
       ct_floor(list(close ORDER BY ts)) FROM ohlc;


-- ------------------------------------------------------------
-- 九、K线形态识别（Pattern Recognition）—— cta_* 返回 INT
-- 返回值：+100 看涨，-100 看跌，0 无形态
-- ------------------------------------------------------------

-- 十字星 / 锤子线 / 吞没形态
SELECT ts, cta_cdldoji(open, high, low, close) OVER (ORDER BY ts),
           cta_cdlhammer(open, high, low, close) OVER (ORDER BY ts),
           cta_cdlengulfing(open, high, low, close) OVER (ORDER BY ts) FROM ohlc;

-- 其余形态函数同理（完整清单见 examples.py 自动覆盖）：
-- cdl2crows / cdl3blackcrows / cdl3inside / cdl3linestrike / cdl3outside
-- cdl3starsinsouth / cdl3whitesoldiers / cdladvanceblock / cdlbelthold
-- cdlbreakaway / cdlclosingmarubozu / cdlconcealbabyswall / cdlcounterattack
-- cdldojistar / cdldragonflydoji / cdlgapsidesidewhite / cdlgravestonedoji
-- cdlhangingman / cdlharami / cdlharamicross / cdlhighwave / cdlhikkake
-- cdlhikkakemod / cdlhomingpigeon / cdlidentical3crows / cdlinneck
-- cdlinvertedhammer / cdlkicking / cdlkickingbylength / cdlladderbottom
-- cdllongleggeddoji / cdllongline / cdlmarubozu / cdlmatchinglow / cdlonneck
-- cdlpiercing / cdlrickshawman / cdlrisefall3methods / cdlseparatinglines
-- cdlshootingstar / cdlshortline / cdlspinningtop / cdlstalledpattern
-- cdlsticksandwich / cdltakuri / cdltasukigap / cdlthrusting / cdltristar
-- cdlunique3river / cdlupsidegap2crows / cdlxsidegap3methods


-- ------------------------------------------------------------
-- 十、时间对齐窗口函数（cta_*_ts）—— 缺口自动 ffill
-- 用法：第一参数是 ts，其余同 cta_*，可选末尾 bar_period
-- ------------------------------------------------------------

-- 1 分钟对齐（默认）
SELECT ts, cta_sma_ts(ts, close, 5) OVER (ORDER BY ts) FROM ohlc;

-- 5 分钟 K 对齐（第 4 个参数 bar_period=5）
SELECT cta_sma_ts(ts, close, 5, 5) OVER (ORDER BY ts) FROM ohlc;

-- 多输入时间对齐：ATR（P3）
SELECT cta_atr_ts(ts, high, low, close, 14) OVER (ORDER BY ts) FROM ohlc;

-- 交易日历辅助函数
SELECT cn_ta_is_trading_day(TIMESTAMP '2026-10-01 10:00:00') AS is_trading_day;  -- false（国庆）
SELECT cn_ta_set_holiday(DATE '2026-10-09') AS set_result;                       -- 追加休市日
SELECT cn_ta_clear_holidays() AS cleared_count;                                  -- 清空


-- ------------------------------------------------------------
-- 十一、Eigen 金融统计与回归（stat_*）
-- ------------------------------------------------------------

-- 单变量统计（LIST 输入）
SELECT stat_var(list(close ORDER BY ts))         AS var_,
       stat_stddev(list(close ORDER BY ts))      AS stddev,
       stat_skew(list(close ORDER BY ts))        AS skew,
       stat_kurtosis(list(close ORDER BY ts))    AS kurt,
       stat_max_drawdown(list(close ORDER BY ts)) AS mdd FROM ohlc;

-- 夏普比率 / 年化波动率 / 历史 VaR（收益率序列, 无风险利率, 年化周期）
SELECT stat_sharpe(list(close ORDER BY ts), 0.02, 252),
       stat_annual_vol(list(close ORDER BY ts), 252),
       stat_var_historical(list(close ORDER BY ts), 0.05) FROM ohlc;

-- EWMA 波动率 / GARCH 波动率 / Sortino / Calmar
SELECT stat_ewma_vol(list(close ORDER BY ts), 0.94),
       stat_garch_vol(list(close ORDER BY ts), 252, 0.1, 0.85),
       stat_sortino(list(close ORDER BY ts), 0.0, 252),
       stat_calmar(list(close ORDER BY ts), 252) FROM ohlc;

-- 双变量统计
SELECT stat_cov(list(close ORDER BY ts), list(volume ORDER BY ts)),
       stat_corr(list(close ORDER BY ts), list(volume ORDER BY ts)),
       stat_beta(list(close ORDER BY ts), list(volume ORDER BY ts)),
       stat_coint(list(close ORDER BY ts), list(volume ORDER BY ts)) FROM ohlc;

-- 信息比率（基金收益, 基准收益, 年化周期）
SELECT stat_information_ratio(list(close ORDER BY ts), list(volume ORDER BY ts), 252) FROM ohlc;

-- 回归（单变量 OLS）→ STRUCT(slope, intercept, r2, std_err)
SELECT (stat_regress(list(close ORDER BY ts), list(volume ORDER BY ts))).slope,
       (stat_regress(list(close ORDER BY ts), list(volume ORDER BY ts))).intercept,
       (stat_regress(list(close ORDER BY ts), list(volume ORDER BY ts))).r2 FROM ohlc;

-- 多变量 OLS（y, X 矩阵）→ STRUCT(coefficients, intercept, r2)
SELECT stat_ols(list_value(6.0, 17.0, 34.0, 57.0),
                CAST([[1.0, 2.0, 3.0, 4.0], [1.0, 4.0, 9.0, 16.0]] AS DOUBLE[][])) AS ols;

-- 协方差矩阵 / PCA
SELECT stat_cov_matrix(CAST([[1.0, 2.0, 3.0, 4.0], [2.0, 4.0, 6.0, 8.0]] AS DOUBLE[][])) AS cov_mat;
SELECT (stat_pca(CAST([[1.0, 2.0, 3.0], [2.0, 4.0, 6.0]] AS DOUBLE[][]))).eigenvalues AS evals;

-- 趋势强度 / RSI
SELECT stat_trend_strength(list(close ORDER BY ts)),
       stat_rsi(list(close ORDER BY ts), 14) FROM ohlc;

-- 滚动统计（窗口版，支持 OVER + ROWS）
SELECT ts, stat_avg(close) OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS rolling_avg,
           stat_sum(close) OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS rolling_sum,
           stat_min(low)   OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS rolling_min,
           stat_max(high)  OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS rolling_max,
           stat_momentum(close) OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS momentum,
           stat_rolling_var(close) OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS rolling_var,
           stat_rolling_stddev(close) OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS rolling_std,
           stat_rolling_beta(close, volume) OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS rolling_beta,
           stat_rolling_corr(close, volume) OVER (ORDER BY ts ROWS BETWEEN 4 PRECEDING AND CURRENT ROW) AS rolling_corr
FROM ohlc;


-- ------------------------------------------------------------
-- 十二、A股适配函数（cn_*）
-- ------------------------------------------------------------

-- 涨跌停幅度（代码, 是否ST）
SELECT cn_limit_pct('600000', false) AS limit_pct;   -- 0.1（主板）
SELECT cn_limit_pct('300001', false) AS limit_pct;   -- 0.2（创业板）
SELECT cn_limit_pct('688001', false) AS limit_pct;   -- 0.2（科创板）
SELECT cn_limit_pct('830001', false) AS limit_pct;   -- 0.3（北交所）

-- 涨跌停价（昨收, 代码, 是否ST）→ STRUCT(limit_up, limit_down)
SELECT (cn_limit_price(10.0, '600000', false)).limit_up,
       (cn_limit_price(10.0, '600000', false)).limit_down;

-- 是否 ST
SELECT cn_is_st('ST中葡') AS is_st;   -- true
SELECT cn_is_st('贵州茅台') AS is_st; -- false

-- 涨跌幅（现价, 昨收）
SELECT cn_change_pct(10.5, 10.0) AS pct;   -- 0.05

-- 换手率（成交量, 流通股本）
SELECT cn_turnover(500000.0, 1000000.0) AS turnover;   -- 0.5

-- 量比（当前量, 历史均量列表）
SELECT cn_volume_ratio(1000000.0, list_value(500000.0, 600000.0, 400000.0)) AS vol_ratio;

-- 市盈率分位（当前PE, PE历史列表）
SELECT cn_pe_percentile(25.0, list_value(10.0, 15.0, 20.0, 30.0, 40.0)) AS pe_pct;  -- 0.6

-- 复权价（收盘价, 复权因子）
SELECT cn_adjust_price(10.0, 1.2) AS adj_price;   -- 12.0

-- 是否交易日（日期）
SELECT cn_trading_day(DATE '2026-10-01') AS is_trading_day;  -- false（国庆）
