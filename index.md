# DuckDB TA-Lib Extension — Function Reference

This extension wraps [TA-Lib](https://ta-lib.org/) technical analysis functions for use inside DuckDB SQL queries.

## Function Forms

Every indicator is available in two forms:

- **Scalar** (`ct_` prefix): accepts `LIST<DOUBLE>` inputs, returns a `LIST` of the same length (leading lookback positions are `NULL`).
- **Window / Aggregate** (`cta_` prefix): accepts plain scalar columns and is used with a window `OVER (...)` clause, returning one value per row.

### Return-type notes

- Most functions return `DOUBLE`. Pattern recognition functions return `INTEGER` (`100` = bullish, `-100` = bearish, `0` = no pattern).
- Multi-output window functions return `STRUCT` — access fields with dot notation: `cta_macd(...).macd`.
- Multi-output scalar functions return `LIST<STRUCT>` — unpack with `unnest()`.

---

## Function Index

Categories per [Wikipedia: Technical indicator](https://en.wikipedia.org/wiki/Technical_indicator).

| Category | Scalar | Window | Description |
|----------|--------|--------|-------------|
| **[Trend](https://en.wikipedia.org/wiki/Technical_indicator#Trend)** — identify price direction | | | |
| Trend | `ct_sma` | `cta_sma` | Simple Moving Average |
| Trend | `ct_ema` | `cta_ema` | Exponential Moving Average |
| Trend | `ct_wma` | `cta_wma` | Weighted Moving Average |
| Trend | `ct_dema` | `cta_dema` | Double Exponential Moving Average |
| Trend | `ct_tema` | `cta_tema` | Triple Exponential Moving Average |
| Trend | `ct_trima` | `cta_trima` | Triangular Moving Average |
| Trend | `ct_kama` | `cta_kama` | Kaufman Adaptive Moving Average |
| Trend | `ct_mama` | `cta_mama` | MESA Adaptive Moving Average |
| Trend | `ct_macd` | `cta_macd` | MACD |
| Trend | `ct_adx` | `cta_adx` | Average Directional Movement Index |
| Trend | `ct_adxr` | `cta_adxr` | Average Directional Movement Rating |
| Trend | `ct_dx` | `cta_dx` | Directional Movement Index |
| Trend | `ct_plus_di` | `cta_plus_di` | Plus Directional Indicator |
| Trend | `ct_minus_di` | `cta_minus_di` | Minus Directional Indicator |
| Trend | `ct_plus_dm` | `cta_plus_dm` | Plus Directional Movement |
| Trend | `ct_minus_dm` | `cta_minus_dm` | Minus Directional Movement |
| Trend | `ct_aroon` | `cta_aroon` | Aroon |
| Trend | `ct_midpoint` | `cta_midpoint` | MidPoint over period |
| Trend | `ct_midprice` | `cta_midprice` | Midpoint Price over period |
| **[Momentum](https://en.wikipedia.org/wiki/Technical_indicator#Momentum)** — measure speed of price change | | | |
| Momentum | `ct_rsi` | `cta_rsi` | Relative Strength Index |
| Momentum | `ct_stoch` | `cta_stoch` | Stochastic Oscillator |
| Momentum | `ct_cmo` | `cta_cmo` | Chande Momentum Oscillator |
| Momentum | `ct_mom` | `cta_mom` | Momentum |
| Momentum | `ct_roc` | `cta_roc` | Rate of Change |
| Momentum | `ct_rocp` | `cta_rocp` | Rate of Change (Percentage) |
| Momentum | `ct_rocr` | `cta_rocr` | Rate of Change (Ratio) |
| Momentum | `ct_rocr100` | `cta_rocr100` | Rate of Change (Ratio x100) |
| Momentum | `ct_trix` | `cta_trix` | Triple Smooth EMA Rate of Change |
| Momentum | `ct_willr` | `cta_willr` | Williams' %R |
| Momentum | `ct_cci` | `cta_cci` | Commodity Channel Index |
| Momentum | `ct_bop` | `cta_bop` | Balance of Power |
| **[Volume](https://en.wikipedia.org/wiki/Technical_indicator#Volume)** — measure buying/selling pressure | | | |
| Volume | `ct_ad` | `cta_ad` | Chaikin A/D Line |
| **[Volatility](https://en.wikipedia.org/wiki/Technical_indicator#Volatility)** — measure price fluctuation | | | |
| Volatility | `ct_atr` | `cta_atr` | Average True Range |
| Volatility | `ct_natr` | `cta_natr` | Normalized Average True Range |
| Volatility | `ct_trange` | `cta_trange` | True Range |
| Volatility | `ct_bbands` | `cta_bbands` | Bollinger Bands |
| Volatility | `ct_minmax` | `cta_minmax` | Min/Max over period |
| **[Cycle](https://en.wikipedia.org/wiki/Technical_indicator)** — detect periodic patterns | | | |
| Cycle | `ct_ht_dcperiod` | `cta_ht_dcperiod` | Dominant Cycle Period |
| Cycle | `ct_ht_dcphase` | `cta_ht_dcphase` | Dominant Cycle Phase |
| Cycle | `ct_ht_trendline` | `cta_ht_trendline` | Instantaneous Trendline |
| Cycle | `ct_ht_trendmode` | `cta_ht_trendmode` | Trend vs Cycle Mode |
| Cycle | `ct_ht_phasor` | `cta_ht_phasor` | Hilbert Transform — Phasor |
| Cycle | `ct_ht_sine` | `cta_ht_sine` | Hilbert Transform — SineWave |
| **Price Transform** — derive synthetic price series | | | |
| Price | `ct_avgprice` | `cta_avgprice` | Average Price (O+H+L+C)/4 |
| Price | `ct_medprice` | `cta_medprice` | Median Price (H+L)/2 |
| Price | `ct_typprice` | `cta_typprice` | Typical Price (H+L+C)/3 |
| Price | `ct_wclprice` | `cta_wclprice` | Weighted Close (H+L+2C)/4 |
| **Statistics** — statistical measures over rolling windows | | | |
| Stats | `ct_linearreg` | `cta_linearreg` | Linear Regression |
| Stats | `ct_linearreg_angle` | `cta_linearreg_angle` | Linear Regression Angle |
| Stats | `ct_linearreg_intercept` | `cta_linearreg_intercept` | Linear Regression Intercept |
| Stats | `ct_linearreg_slope` | `cta_linearreg_slope` | Linear Regression Slope |
| Stats | `ct_tsf` | `cta_tsf` | Time Series Forecast |
| Stats | `ct_sum` | `cta_sum` | Summation |
| Stats | `ct_max` | `cta_max` | Highest Value over period |
| Stats | `ct_min` | `cta_min` | Lowest Value over period |
| Stats | `ct_maxindex` | `cta_maxindex` | Index of Highest Value |
| Stats | `ct_minindex` | `cta_minindex` | Index of Lowest Value |
| **Math Transform** (scalar only) — element-wise math | | | |
| Math | `ct_acos` | — | Arc Cosine |
| Math | `ct_asin` | — | Arc Sine |
| Math | `ct_atan` | — | Arc Tangent |
| Math | `ct_ceil` | — | Ceiling |
| Math | `ct_cos` | — | Cosine |
| Math | `ct_cosh` | — | Hyperbolic Cosine |
| Math | `ct_exp` | — | Exponential (e^x) |
| Math | `ct_floor` | — | Floor |
| Math | `ct_ln` | — | Natural Logarithm |
| Math | `ct_log10` | — | Base-10 Logarithm |
| Math | `ct_sin` | — | Sine |
| Math | `ct_sinh` | — | Hyperbolic Sine |
| Math | `ct_sqrt` | — | Square Root |
| Math | `ct_tan` | — | Tangent |
| Math | `ct_tanh` | — | Hyperbolic Tangent |
| **[Pattern Recognition](https://en.wikipedia.org/wiki/Candlestick_pattern)** (scalar only) — candlestick patterns | | | |
| Pattern | `ct_cdl2crows` | — | Two Crows |
| Pattern | `ct_cdl3blackcrows` | — | Three Black Crows |
| Pattern | `ct_cdl3inside` | — | Three Inside Up/Down |
| Pattern | `ct_cdl3linestrike` | — | Three-Line Strike |
| Pattern | `ct_cdl3outside` | — | Three Outside Up/Down |
| Pattern | `ct_cdl3starsinsouth` | — | Three Stars In The South |
| Pattern | `ct_cdl3whitesoldiers` | — | Three Advancing White Soldiers |
| Pattern | `ct_cdladvanceblock` | — | Advance Block |
| Pattern | `ct_cdlbelthold` | — | Belt-hold |
| Pattern | `ct_cdlbreakaway` | — | Breakaway |
| Pattern | `ct_cdlclosingmarubozu` | — | Closing Marubozu |
| Pattern | `ct_cdlconcealbabyswall` | — | Concealing Baby Swallow |
| Pattern | `ct_cdlcounterattack` | — | Counterattack |
| Pattern | `ct_cdldoji` | — | Doji |
| Pattern | `ct_cdldojistar` | — | Doji Star |
| Pattern | `ct_cdldragonflydoji` | — | Dragonfly Doji |
| Pattern | `ct_cdlengulfing` | — | Engulfing Pattern |
| Pattern | `ct_cdlgapsidesidewhite` | — | Gap Side-by-Side White Lines |
| Pattern | `ct_cdlgravestonedoji` | — | Gravestone Doji |
| Pattern | `ct_cdlhammer` | — | Hammer |
| Pattern | `ct_cdlhangingman` | — | Hanging Man |
| Pattern | `ct_cdlharami` | — | Harami Pattern |
| Pattern | `ct_cdlharamicross` | — | Harami Cross Pattern |
| Pattern | `ct_cdlhighwave` | — | High-Wave Candle |
| Pattern | `ct_cdlhikkake` | — | Hikkake Pattern |
| Pattern | `ct_cdlhikkakemod` | — | Modified Hikkake Pattern |
| Pattern | `ct_cdlhomingpigeon` | — | Homing Pigeon |
| Pattern | `ct_cdlidentical3crows` | — | Identical Three Crows |
| Pattern | `ct_cdlinneck` | — | In-Neck Pattern |
| Pattern | `ct_cdlinvertedhammer` | — | Inverted Hammer |
| Pattern | `ct_cdlkicking` | — | Kicking |
| Pattern | `ct_cdlkickingbylength` | — | Kicking (by longer marubozu) |
| Pattern | `ct_cdlladderbottom` | — | Ladder Bottom |
| Pattern | `ct_cdllongleggeddoji` | — | Long Legged Doji |
| Pattern | `ct_cdllongline` | — | Long Line Candle |
| Pattern | `ct_cdlmarubozu` | — | Marubozu |
| Pattern | `ct_cdlmatchinglow` | — | Matching Low |
| Pattern | `ct_cdlonneck` | — | On-Neck Pattern |
| Pattern | `ct_cdlpiercing` | — | Piercing Pattern |
| Pattern | `ct_cdlrickshawman` | — | Rickshaw Man |
| Pattern | `ct_cdlrisefall3methods` | — | Rising/Falling Three Methods |
| Pattern | `ct_cdlseparatinglines` | — | Separating Lines |
| Pattern | `ct_cdlshootingstar` | — | Shooting Star |
| Pattern | `ct_cdlshortline` | — | Short Line Candle |
| Pattern | `ct_cdlspinningtop` | — | Spinning Top |
| Pattern | `ct_cdlstalledpattern` | — | Stalled Pattern |
| Pattern | `ct_cdlsticksandwich` | — | Stick Sandwich |
| Pattern | `ct_cdltakuri` | — | Takuri (long lower shadow Doji) |
| Pattern | `ct_cdltasukigap` | — | Tasuki Gap |
| Pattern | `ct_cdlthrusting` | — | Thrusting Pattern |
| Pattern | `ct_cdltristar` | — | Tristar Pattern |
| Pattern | `ct_cdlunique3river` | — | Unique 3 River |
| Pattern | `ct_cdlupsidegap2crows` | — | Upside Gap Two Crows |
| Pattern | `ct_cdlxsidegap3methods` | — | Upside/Downside Gap Three Methods |

---

## Function Signatures

| # | Function | Signature | Return |
|---|----------|-----------|--------|
| | **Trend** | | |
| 1 | `ct_sma` / `cta_sma` | (values, period) | DOUBLE |
| 2 | `ct_ema` / `cta_ema` | (values, period) | DOUBLE |
| 3 | `ct_wma` / `cta_wma` | (values, period) | DOUBLE |
| 4 | `ct_dema` / `cta_dema` | (values, period) | DOUBLE |
| 5 | `ct_tema` / `cta_tema` | (values, period) | DOUBLE |
| 6 | `ct_trima` / `cta_trima` | (values, period) | DOUBLE |
| 7 | `ct_kama` / `cta_kama` | (values, period) | DOUBLE |
| 8 | `ct_mama` / `cta_mama` | (values, fast_limit, slow_limit) | STRUCT(mama, fama) |
| 9 | `ct_macd` / `cta_macd` | (values, fast_period, slow_period, signal_period) | STRUCT(macd, signal, hist) |
| 10 | `ct_adx` / `cta_adx` | (high, low, close, period) | DOUBLE |
| 11 | `ct_adxr` / `cta_adxr` | (high, low, close, period) | DOUBLE |
| 12 | `ct_dx` / `cta_dx` | (high, low, close, period) | DOUBLE |
| 13 | `ct_plus_di` / `cta_plus_di` | (high, low, close, period) | DOUBLE |
| 14 | `ct_minus_di` / `cta_minus_di` | (high, low, close, period) | DOUBLE |
| 15 | `ct_plus_dm` / `cta_plus_dm` | (high, low, period) | DOUBLE |
| 16 | `ct_minus_dm` / `cta_minus_dm` | (high, low, period) | DOUBLE |
| 17 | `ct_aroon` / `cta_aroon` | (high, low, period) | STRUCT(aroon_down, aroon_up) |
| 18 | `ct_midpoint` / `cta_midpoint` | (values, period) | DOUBLE |
| 19 | `ct_midprice` / `cta_midprice` | (high, low, period) | DOUBLE |
| | **Momentum** | | |
| 20 | `ct_rsi` / `cta_rsi` | (values, period) | DOUBLE |
| 21 | `ct_stoch` / `cta_stoch` | (high, low, close, fastK, slowK, slowKMA, slowD, slowDMA) | STRUCT(slowk, slowd) |
| 22 | `ct_cmo` / `cta_cmo` | (values, period) | DOUBLE |
| 23 | `ct_mom` / `cta_mom` | (values, period) | DOUBLE |
| 24 | `ct_roc` / `cta_roc` | (values, period) | DOUBLE |
| 25 | `ct_rocp` / `cta_rocp` | (values, period) | DOUBLE |
| 26 | `ct_rocr` / `cta_rocr` | (values, period) | DOUBLE |
| 27 | `ct_rocr100` / `cta_rocr100` | (values, period) | DOUBLE |
| 28 | `ct_trix` / `cta_trix` | (values, period) | DOUBLE |
| 29 | `ct_willr` / `cta_willr` | (high, low, close, period) | DOUBLE |
| 30 | `ct_cci` / `cta_cci` | (high, low, close, period) | DOUBLE |
| 31 | `ct_bop` / `cta_bop` | (open, high, low, close) | DOUBLE |
| | **Volume** | | |
| 32 | `ct_ad` / `cta_ad` | (high, low, close, volume) | DOUBLE |
| | **Volatility** | | |
| 33 | `ct_atr` / `cta_atr` | (high, low, close, period) | DOUBLE |
| 34 | `ct_natr` / `cta_natr` | (high, low, close, period) | DOUBLE |
| 35 | `ct_trange` / `cta_trange` | (high, low, close) | DOUBLE |
| 36 | `ct_bbands` / `cta_bbands` | (values, period, devup, devdn, matype) | STRUCT(upper, middle, lower) |
| 37 | `ct_minmax` / `cta_minmax` | (values, period) | STRUCT(min, max) |
| | **Cycle** | | |
| 38 | `ct_ht_dcperiod` / `cta_ht_dcperiod` | (values) | DOUBLE |
| 39 | `ct_ht_dcphase` / `cta_ht_dcphase` | (values) | DOUBLE |
| 40 | `ct_ht_trendline` / `cta_ht_trendline` | (values) | DOUBLE |
| 41 | `ct_ht_trendmode` / `cta_ht_trendmode` | (values) | INTEGER |
| 42 | `ct_ht_phasor` / `cta_ht_phasor` | (values) | STRUCT(inphase, quadrature) |
| 43 | `ct_ht_sine` / `cta_ht_sine` | (values) | STRUCT(sine, leadsine) |
| | **Price Transform** | | |
| 44 | `ct_avgprice` / `cta_avgprice` | (open, high, low, close) | DOUBLE |
| 45 | `ct_medprice` / `cta_medprice` | (high, low) | DOUBLE |
| 46 | `ct_typprice` / `cta_typprice` | (high, low, close) | DOUBLE |
| 47 | `ct_wclprice` / `cta_wclprice` | (high, low, close) | DOUBLE |
| | **Statistics** | | |
| 48 | `ct_linearreg` / `cta_linearreg` | (values, period) | DOUBLE |
| 49 | `ct_linearreg_angle` / `cta_linearreg_angle` | (values, period) | DOUBLE |
| 50 | `ct_linearreg_intercept` / `cta_linearreg_intercept` | (values, period) | DOUBLE |
| 51 | `ct_linearreg_slope` / `cta_linearreg_slope` | (values, period) | DOUBLE |
| 52 | `ct_tsf` / `cta_tsf` | (values, period) | DOUBLE |
| 53 | `ct_sum` / `cta_sum` | (values, period) | DOUBLE |
| 54 | `ct_max` / `cta_max` | (values, period) | DOUBLE |
| 55 | `ct_min` / `cta_min` | (values, period) | DOUBLE |
| 56 | `ct_maxindex` / `cta_maxindex` | (values, period) | INTEGER |
| 57 | `ct_minindex` / `cta_minindex` | (values, period) | INTEGER |
| | **Math Transform** (scalar only) | | |
| 58 | `ct_acos` | (values) | DOUBLE |
| 59 | `ct_asin` | (values) | DOUBLE |
| 60 | `ct_atan` | (values) | DOUBLE |
| 61 | `ct_ceil` | (values) | DOUBLE |
| 62 | `ct_cos` | (values) | DOUBLE |
| 63 | `ct_cosh` | (values) | DOUBLE |
| 64 | `ct_exp` | (values) | DOUBLE |
| 65 | `ct_floor` | (values) | DOUBLE |
| 66 | `ct_ln` | (values) | DOUBLE |
| 67 | `ct_log10` | (values) | DOUBLE |
| 68 | `ct_sin` | (values) | DOUBLE |
| 69 | `ct_sinh` | (values) | DOUBLE |
| 70 | `ct_sqrt` | (values) | DOUBLE |
| 71 | `ct_tan` | (values) | DOUBLE |
| 72 | `ct_tanh` | (values) | DOUBLE |
| | **Pattern Recognition** (scalar only) | | |
| 73–126 | `ct_cdl*` | (open, high, low, close) | INTEGER |

---

## Function Details

### Trend

1. **SMA** [Tulip](https://tulipindicators.org/sma): Simple Moving Average
$SMA = \frac{1}{n}\sum_{i=0}^{n-1} x_{t-i}$

2. **EMA** [Tulip](https://tulipindicators.org/ema): Exponential Moving Average
$EMA_t = \alpha \cdot x_t + (1 - \alpha) \cdot EMA_{t-1}, \quad \alpha = \frac{2}{n+1}$

3. **WMA** [Tulip](https://tulipindicators.org/wma): Weighted Moving Average
$WMA = \frac{\sum_{i=0}^{n-1}(n-i) \cdot x_{t-i}}{\sum_{i=1}^{n} i}$

4. **DEMA** [Tulip](https://tulipindicators.org/dema): Double Exponential Moving Average
$DEMA = 2 \cdot EMA(x, n) - EMA(EMA(x, n), n)$

5. **TEMA** [Tulip](https://tulipindicators.org/tema): Triple Exponential Moving Average
$TEMA = 3E_1 - 3E_2 + E_3, \quad E_1 = EMA,\ E_2 = EMA(E_1),\ E_3 = EMA(E_2)$

6. **TRIMA** [Tulip](https://tulipindicators.org/trima): Triangular Moving Average
$TRIMA = SMA(SMA(x, m), m), \quad m = \lceil(n+1)/2\rceil$

7. **KAMA** [Tulip](https://tulipindicators.org/kama): Kaufman Adaptive Moving Average
$KAMA_t = KAMA_{t-1} + SC^2 \cdot (x_t - KAMA_{t-1}), \quad SC = ER \cdot (\alpha_f - \alpha_s) + \alpha_s$

8. **MAMA** [Wikipedia](https://en.wikipedia.org/wiki/Hilbert_transform): MESA Adaptive Moving Average
$MAMA_t = MAMA_{t-1} + \alpha_t(x_t - MAMA_{t-1}); \quad FAMA_t = FAMA_{t-1} + 0.5\alpha_t(MAMA_t - FAMA_{t-1})$

9. **MACD** [Tulip](https://tulipindicators.org/macd): Moving Average Convergence/Divergence
$MACD = EMA(x, f) - EMA(x, s); \quad Signal = EMA(MACD, p); \quad Hist = MACD - Signal$

10. **ADX** [Tulip](https://tulipindicators.org/adx): Average Directional Index
$ADX = SMA(DX, n)$

11. **ADXR** [Tulip](https://tulipindicators.org/adxr): Average Directional Movement Rating
$ADXR = \frac{ADX_t + ADX_{t-n}}{2}$

12. **DX** [Tulip](https://tulipindicators.org/dx): Directional Movement Index
$DX = \frac{|+DI - (-DI)|}{+DI + (-DI)} \times 100$

13. **PLUS_DI** [Tulip](https://tulipindicators.org/di): Plus Directional Indicator
$+DI = \frac{Smooth(+DM, n)}{ATR(n)} \times 100$

14. **MINUS_DI** [Tulip](https://tulipindicators.org/di): Minus Directional Indicator
$-DI = \frac{Smooth(-DM, n)}{ATR(n)} \times 100$

15. **PLUS_DM** [Tulip](https://tulipindicators.org/dm): Plus Directional Movement
$+DM = H_t - H_{t-1} \text{ if } > 0 \text{ and } > (L_{t-1} - L_t), \text{ else } 0$

16. **MINUS_DM** [Tulip](https://tulipindicators.org/dm): Minus Directional Movement
$-DM = L_{t-1} - L_t \text{ if } > 0 \text{ and } > (H_t - H_{t-1}), \text{ else } 0$

17. **AROON** [Tulip](https://tulipindicators.org/aroon): Aroon Indicator
$Up = \frac{n - \text{bars since } H_n}{n} \times 100; \quad Down = \frac{n - \text{bars since } L_n}{n} \times 100$

18. **MIDPOINT** [Wikipedia](https://en.wikipedia.org/wiki/Mid_price): Midpoint over period
$MID = \frac{\max(x, n) + \min(x, n)}{2}$

19. **MIDPRICE** [Wikipedia](https://en.wikipedia.org/wiki/Mid_price): Midpoint Price over period
$MIDPRICE = \frac{H_{highest}(n) + L_{lowest}(n)}{2}$

### Momentum

20. **RSI** [Tulip](https://tulipindicators.org/rsi): Relative Strength Index
$RSI = 100 - \frac{100}{1 + \frac{\overline{gain}}{\overline{loss}}}$

21. **STOCH** [Tulip](https://tulipindicators.org/stoch): Stochastic Oscillator
$\%K = \frac{C - L_n}{H_n - L_n} \times 100; \quad \%D = SMA(\%K)$

22. **CMO** [Tulip](https://tulipindicators.org/cmo): Chande Momentum Oscillator
$CMO = \frac{\sum up - \sum down}{\sum up + \sum down} \times 100$

23. **MOM** [Tulip](https://tulipindicators.org/mom): Momentum
$MOM = x_t - x_{t-n}$

24. **ROC** [Tulip](https://tulipindicators.org/roc): Rate of Change
$ROC = \frac{x_t - x_{t-n}}{x_{t-n}} \times 100$

25. **ROCP** [Tulip](https://tulipindicators.org/roc): Rate of Change (Percentage)
$ROCP = \frac{x_t - x_{t-n}}{x_{t-n}}$

26. **ROCR** [Tulip](https://tulipindicators.org/rocr): Rate of Change (Ratio)
$ROCR = \frac{x_t}{x_{t-n}}$

27. **ROCR100** [Tulip](https://tulipindicators.org/rocr): Rate of Change (Ratio x100)
$ROCR100 = \frac{x_t}{x_{t-n}} \times 100$

28. **TRIX** [Tulip](https://tulipindicators.org/trix): Triple Smooth EMA Rate of Change
$TRIX = \frac{E_3(t) - E_3(t-1)}{E_3(t-1)} \times 100, \quad E_3 = EMA(EMA(EMA(x)))$

29. **WILLR** [Tulip](https://tulipindicators.org/willr): Williams %R
$\%R = \frac{H_n - C}{H_n - L_n} \times (-100)$

30. **CCI** [Tulip](https://tulipindicators.org/cci): Commodity Channel Index
$CCI = \frac{TP - SMA(TP, n)}{0.015 \cdot MD}, \quad TP = \frac{H+L+C}{3}$

31. **BOP** [Tulip](https://tulipindicators.org/bop): Balance of Power
$BOP = \frac{C - O}{H - L}$

### Volume

32. **AD** [Tulip](https://tulipindicators.org/ad): Chaikin A/D Line
$AD = \sum \frac{(C - L) - (H - C)}{H - L} \times V$

### Volatility

33. **ATR** [Tulip](https://tulipindicators.org/atr): Average True Range
$ATR = Wilder\_Smooth(TR, n)$

34. **NATR** [Tulip](https://tulipindicators.org/natr): Normalized Average True Range
$NATR = \frac{ATR}{C} \times 100$

35. **TRANGE** [Tulip](https://tulipindicators.org/tr): True Range
$TR = \max(H - L,\ |H - C_{t-1}|,\ |L - C_{t-1}|)$

36. **BBANDS** [Tulip](https://tulipindicators.org/bbands): Bollinger Bands
$Mid = SMA(x, n); \quad Upper = Mid + k\sigma; \quad Lower = Mid - k\sigma$

37. **MINMAX** [Tulip](https://tulipindicators.org/min): Min/Max over period
$\min(x_{t-n+1}, \ldots, x_t)$ and $\max(x_{t-n+1}, \ldots, x_t)$ in single pass

### Price Transform

38. **AVGPRICE** [Tulip](https://tulipindicators.org/avgprice): Average Price
$AVGPRICE = \frac{O + H + L + C}{4}$

39. **MEDPRICE** [Tulip](https://tulipindicators.org/medprice): Median Price
$MEDPRICE = \frac{H + L}{2}$

40. **TYPPRICE** [Tulip](https://tulipindicators.org/typprice): Typical Price
$TYPPRICE = \frac{H + L + C}{3}$

41. **WCLPRICE** [Tulip](https://tulipindicators.org/wcprice): Weighted Close Price
$WCLPRICE = \frac{H + L + 2C}{4}$

### Cycle

42. **HT_DCPERIOD** [Wikipedia](https://en.wikipedia.org/wiki/Hilbert_transform): Dominant Cycle Period
Hilbert Transform extracts dominant cycle length from price data.

43. **HT_DCPHASE** [Wikipedia](https://en.wikipedia.org/wiki/Hilbert_transform): Dominant Cycle Phase
Phase angle $\phi$ of the dominant cycle in degrees.

44. **HT_TRENDLINE** [Wikipedia](https://en.wikipedia.org/wiki/Hilbert_transform): Instantaneous Trendline
Hilbert-smoothed trend component of the price series.

45. **HT_TRENDMODE** [Wikipedia](https://en.wikipedia.org/wiki/Hilbert_transform): Trend vs Cycle Mode
Returns $1$ if trending, $0$ if in cycle mode.

46. **HT_PHASOR** [Wikipedia](https://en.wikipedia.org/wiki/Hilbert_transform): Hilbert Transform — Phasor
In-phase $I$ and quadrature $Q$ components of dominant cycle.

47. **HT_SINE** [Wikipedia](https://en.wikipedia.org/wiki/Hilbert_transform): Hilbert Transform — SineWave
$sine = \sin(\phi); \quad leadsine = \sin(\phi + \pi/4)$ where $\phi$ = dominant cycle phase

### Statistics

40. **LINEARREG** [Tulip](https://tulipindicators.org/linreg): Linear Regression
$\hat{y}_t = b_0 + b_1 \cdot t$

41. **LINEARREG_ANGLE** [Tulip](https://tulipindicators.org/linreg): Linear Regression Angle
$\theta = \arctan(b_1)$ in degrees

42. **LINEARREG_INTERCEPT** [Tulip](https://tulipindicators.org/linregintercept): Linear Regression Intercept
$b_0$ of best-fit line $y = b_0 + b_1 t$

43. **LINEARREG_SLOPE** [Tulip](https://tulipindicators.org/linregslope): Linear Regression Slope
$b_1 = \frac{n\sum ty - \sum t \sum y}{n\sum t^2 - (\sum t)^2}$

44. **TSF** [Tulip](https://tulipindicators.org/tsf): Time Series Forecast
$TSF = b_0 + b_1 \cdot (n + 1)$

45. **SUM** [Tulip](https://tulipindicators.org/sum): Rolling Summation
$SUM = \sum_{i=0}^{n-1} x_{t-i}$

46. **MAX** [Tulip](https://tulipindicators.org/max): Highest Value over period
$MAX = \max(x_t, x_{t-1}, \ldots, x_{t-n+1})$

47. **MIN** [Tulip](https://tulipindicators.org/min): Lowest Value over period
$MIN = \min(x_t, x_{t-1}, \ldots, x_{t-n+1})$

48. **MAXINDEX** [Tulip](https://tulipindicators.org/max): Index of Highest Value
Position $i$ where $x_i = \max(x)$ within the window.

49. **MININDEX** [Tulip](https://tulipindicators.org/min): Index of Lowest Value
Position $i$ where $x_i = \min(x)$ within the window.

### Math Transform

50. **ACOS** [Wikipedia](https://en.wikipedia.org/wiki/Inverse_trigonometric_functions): Arc Cosine
$y = \cos^{-1}(x)$

51. **ASIN** [Wikipedia](https://en.wikipedia.org/wiki/Inverse_trigonometric_functions): Arc Sine
$y = \sin^{-1}(x)$

52. **ATAN** [Wikipedia](https://en.wikipedia.org/wiki/Inverse_trigonometric_functions): Arc Tangent
$y = \tan^{-1}(x)$

53. **CEIL** [Wikipedia](https://en.wikipedia.org/wiki/Floor_and_ceiling_functions): Ceiling
$y = \lceil x \rceil$

54. **COS** [Wikipedia](https://en.wikipedia.org/wiki/Trigonometric_functions): Cosine
$y = \cos(x)$

55. **COSH** [Wikipedia](https://en.wikipedia.org/wiki/Hyperbolic_functions): Hyperbolic Cosine
$y = \cosh(x) = \frac{e^x + e^{-x}}{2}$

56. **EXP** [Wikipedia](https://en.wikipedia.org/wiki/Exponential_function): Exponential
$y = e^x$

57. **FLOOR** [Wikipedia](https://en.wikipedia.org/wiki/Floor_and_ceiling_functions): Floor
$y = \lfloor x \rfloor$

58. **LN** [Wikipedia](https://en.wikipedia.org/wiki/Natural_logarithm): Natural Logarithm
$y = \ln(x)$

59. **LOG10** [Wikipedia](https://en.wikipedia.org/wiki/Common_logarithm): Base-10 Logarithm
$y = \log_{10}(x)$

60. **SIN** [Wikipedia](https://en.wikipedia.org/wiki/Trigonometric_functions): Sine
$y = \sin(x)$

61. **SINH** [Wikipedia](https://en.wikipedia.org/wiki/Hyperbolic_functions): Hyperbolic Sine
$y = \sinh(x) = \frac{e^x - e^{-x}}{2}$

62. **SQRT** [Wikipedia](https://en.wikipedia.org/wiki/Square_root): Square Root
$y = \sqrt{x}$

63. **TAN** [Wikipedia](https://en.wikipedia.org/wiki/Trigonometric_functions): Tangent
$y = \tan(x)$

64. **TANH** [Wikipedia](https://en.wikipedia.org/wiki/Hyperbolic_functions): Hyperbolic Tangent
$y = \tanh(x) = \frac{e^x - e^{-x}}{e^x + e^{-x}}$

### Pattern Recognition

All candlestick patterns take $(O, H, L, C)$ and return $+100$ (bullish), $-100$ (bearish), or $0$ (no pattern).
See [Candlestick pattern (Wikipedia)](https://en.wikipedia.org/wiki/Candlestick_pattern).

65. **CDL2CROWS** [Investopedia](https://www.investopedia.com/terms/t/twocrows.asp): Two Crows — bearish reversal, two black candles gapping above uptrend
66. **CDL3BLACKCROWS** [Wikipedia](https://en.wikipedia.org/wiki/Three_black_crows): Three Black Crows — three consecutive long bearish candles
67. **CDL3INSIDE** [Investopedia](https://www.investopedia.com/terms/t/three-inside-updown.asp): Three Inside Up/Down — reversal confirmed by third candle
68. **CDL3LINESTRIKE** [Investopedia](https://www.investopedia.com/terms/t/three-line-strike.asp): Three-Line Strike — three same-direction then strike candle
69. **CDL3OUTSIDE** [Investopedia](https://www.investopedia.com/terms/t/three-outside-updown.asp): Three Outside Up/Down — engulfing confirmed by third candle
70. **CDL3STARSINSOUTH** [Investopedia](https://www.investopedia.com/terms/t/three-stars-south.asp): Three Stars In The South — declining bearish, shrinking bodies
71. **CDL3WHITESOLDIERS** [Wikipedia](https://en.wikipedia.org/wiki/Three_white_soldiers): Three White Soldiers — three consecutive long bullish candles
72. **CDLADVANCEBLOCK** [Investopedia](https://www.investopedia.com/terms/a/advance-block.asp): Advance Block — three bullish with shrinking bodies
73. **CDLBELTHOLD** [Investopedia](https://www.investopedia.com/terms/b/belt-hold.asp): Belt-hold — long candle opening at its extreme
74. **CDLBREAKAWAY** [Investopedia](https://www.investopedia.com/terms/b/breakaway-gap.asp): Breakaway — gap then reversal closing the gap
75. **CDLCLOSINGMARUBOZU** [Investopedia](https://www.investopedia.com/terms/m/marubozu.asp): Closing Marubozu — no shadow on closing side
76. **CDLCONCEALBABYSWALL** [Investopedia](https://www.investopedia.com/terms/c/concealing-baby-swallow.asp): Concealing Baby Swallow — four-candle bearish pattern
77. **CDLCOUNTERATTACK** [Investopedia](https://www.investopedia.com/terms/c/counterattack.asp): Counterattack — two opposite candles, same close
78. **CDLDOJI** [Wikipedia](https://en.wikipedia.org/wiki/Doji): Doji — $|O - C| \approx 0$
79. **CDLDOJISTAR** [Investopedia](https://www.investopedia.com/terms/d/doji.asp): Doji Star — doji gapping from previous candle
80. **CDLDRAGONFLYDOJI** [Investopedia](https://www.investopedia.com/terms/d/dragonfly-doji.asp): Dragonfly Doji — long lower shadow, no upper
81. **CDLENGULFING** [Investopedia](https://www.investopedia.com/terms/b/bullishengulfingpattern.asp): Engulfing — second body fully contains first
82. **CDLGAPSIDESIDEWHITE** [Investopedia](https://www.investopedia.com/terms/u/upside-gap-two-crows.asp): Gap Side-by-Side White — two similar candles after gap
83. **CDLGRAVESTONEDOJI** [Investopedia](https://www.investopedia.com/terms/g/gravestone-doji.asp): Gravestone Doji — long upper shadow, no lower
84. **CDLHAMMER** [Wikipedia](https://en.wikipedia.org/wiki/Hammer_(candlestick_pattern)): Hammer — small body, lower shadow $\geq 2\times$ body
85. **CDLHANGINGMAN** [Wikipedia](https://en.wikipedia.org/wiki/Hanging_man_(candlestick_pattern)): Hanging Man — hammer in uptrend (bearish)
86. **CDLHARAMI** [Wikipedia](https://en.wikipedia.org/wiki/Harami_(candlestick_pattern)): Harami — second candle inside first's body
87. **CDLHARAMICROSS** [Investopedia](https://www.investopedia.com/terms/h/haramicross.asp): Harami Cross — harami where second is doji
88. **CDLHIGHWAVE** [Investopedia](https://www.investopedia.com/terms/l/long-legged-doji.asp): High-Wave Candle — very long shadows, small body
89. **CDLHIKKAKE** [Investopedia](https://www.investopedia.com/terms/h/hikkakepattern.asp): Hikkake — inside bar breakout failure
90. **CDLHIKKAKEMOD** [Investopedia](https://www.investopedia.com/terms/h/hikkakepattern.asp): Modified Hikkake — confirmed by subsequent action
91. **CDLHOMINGPIGEON** [Investopedia](https://www.investopedia.com/terms/h/homing-pigeon.asp): Homing Pigeon — two bearish, second inside first
92. **CDLIDENTICAL3CROWS** [Investopedia](https://www.investopedia.com/terms/i/identical-three-crows.asp): Identical Three Crows — each opens at prior close
93. **CDLINNECK** [Investopedia](https://www.investopedia.com/terms/i/inneck-pattern.asp): In-Neck — small bullish closing near prior low
94. **CDLINVERTEDHAMMER** [Investopedia](https://www.investopedia.com/terms/i/invertedhammer.asp): Inverted Hammer — small body, long upper shadow
95. **CDLKICKING** [Investopedia](https://www.investopedia.com/terms/k/kicking-pattern.asp): Kicking — two opposing marubozu with gap
96. **CDLKICKINGBYLENGTH** [Investopedia](https://www.investopedia.com/terms/k/kicking-pattern.asp): Kicking by Length — direction by longer marubozu
97. **CDLLADDERBOTTOM** [Investopedia](https://www.investopedia.com/terms/l/ladder-bottom.asp): Ladder Bottom — four bearish then bullish reversal
98. **CDLLONGLEGGEDDOJI** [Investopedia](https://www.investopedia.com/terms/l/long-legged-doji.asp): Long Legged Doji — very long both shadows
99. **CDLLONGLINE** [Investopedia](https://www.investopedia.com/terms/l/long-line-candle.asp): Long Line Candle — unusually long body
100. **CDLMARUBOZU** [Investopedia](https://www.investopedia.com/terms/m/marubozu.asp): Marubozu — no shadows (pure body)
101. **CDLMATCHINGLOW** [Investopedia](https://www.investopedia.com/terms/m/matching-low.asp): Matching Low — two bearish with same close
102. **CDLONNECK** [Investopedia](https://www.investopedia.com/terms/o/on-neck-pattern.asp): On-Neck — bullish closing at prior low
103. **CDLPIERCING** [Wikipedia](https://en.wikipedia.org/wiki/Candlestick_pattern): Piercing — bullish closing above midpoint of prior bearish
104. **CDLRICKSHAWMAN** [Investopedia](https://www.investopedia.com/terms/r/rickshaw-man.asp): Rickshaw Man — long-legged doji, body at center
105. **CDLRISEFALL3METHODS** [Investopedia](https://www.investopedia.com/terms/r/rising-three-methods.asp): Rising/Falling Three Methods — continuation pattern
106. **CDLSEPARATINGLINES** [Investopedia](https://www.investopedia.com/terms/s/separating-lines.asp): Separating Lines — opposite candles, same open
107. **CDLSHOOTINGSTAR** [Wikipedia](https://en.wikipedia.org/wiki/Shooting_star_(candlestick_pattern)): Shooting Star — inverted hammer in uptrend
108. **CDLSHORTLINE** [Investopedia](https://www.investopedia.com/terms/s/short-line-candle.asp): Short Line Candle — unusually short body
109. **CDLSPINNINGTOP** [Wikipedia](https://en.wikipedia.org/wiki/Spinning_top_(candlestick_pattern)): Spinning Top — small body with both shadows
110. **CDLSTALLEDPATTERN** [Investopedia](https://www.investopedia.com/terms/s/stalled-pattern.asp): Stalled Pattern — three bullish, third small
111. **CDLSTICKSANDWICH** [Investopedia](https://www.investopedia.com/terms/s/stick-sandwich.asp): Stick Sandwich — same-close sandwiching opposite
112. **CDLTAKURI** [Investopedia](https://www.investopedia.com/terms/t/takuri.asp): Takuri — doji with very long lower shadow
113. **CDLTASUKIGAP** [Investopedia](https://www.investopedia.com/terms/t/tasuki-gap.asp): Tasuki Gap — partial-fill candle after gap
114. **CDLTHRUSTING** [Investopedia](https://www.investopedia.com/terms/t/thrusting-pattern.asp): Thrusting — bullish closing below midpoint of prior
115. **CDLTRISTAR** [Investopedia](https://www.investopedia.com/terms/t/tristar.asp): Tristar — three dojis with middle gapping
116. **CDLUNIQUE3RIVER** [Investopedia](https://www.investopedia.com/terms/u/unique-three-river.asp): Unique 3 River — three-candle bullish reversal
117. **CDLUPSIDEGAP2CROWS** [Investopedia](https://www.investopedia.com/terms/u/upside-gap-two-crows.asp): Upside Gap Two Crows — two bearish gapping above
118. **CDLXSIDEGAP3METHODS** [Investopedia](https://www.investopedia.com/terms/u/updown-gap-sidebyside-white-lines.asp): Gap Three Methods — opposite candle closing gap

---

## Usage Patterns

### Scalar form — aggregate over a group

```sql
-- Compute 14-period RSI for each symbol
SELECT
    symbol,
    ct_rsi(list(close ORDER BY ts), 14) AS rsi_series
FROM prices
GROUP BY symbol;
```

### Window form — row-by-row with OVER()

```sql
-- Attach current RSI value to every row
SELECT
    symbol,
    ts,
    close,
    cta_rsi(close, 14) OVER (PARTITION BY symbol ORDER BY ts) AS rsi
FROM prices;
```

### Unpacking multi-output structs (scalar)

```sql
-- Unpack MACD struct fields from scalar form
SELECT
    symbol,
    ts,
    r.macd,
    r.signal,
    r.hist
FROM (
    SELECT symbol, unnest(ct_macd(list(close ORDER BY ts), 12, 26, 9)) AS r
    FROM prices
    GROUP BY symbol
);
```

### Multi-output window form — STRUCT per row

```sql
-- MACD with dot-notation field access
SELECT symbol, ts, close, m.macd, m.signal, m.hist
FROM (
    SELECT *, cta_macd(close, 12, 26, 9) OVER (PARTITION BY symbol ORDER BY ts) AS m
    FROM prices
);

-- Bollinger Bands
SELECT ts, b.upper, b.middle, b.lower
FROM (
    SELECT *, cta_bbands(close, 20, 2.0, 2.0, 0) OVER (ORDER BY ts) AS b
    FROM prices
);
```

### MA type reference

Several functions accept an integer `ma_type` parameter:

| Value | Type |
|-------|------|
| 0 | SMA |
| 1 | EMA |
| 2 | WMA |
| 3 | DEMA |
| 4 | TEMA |
| 5 | TRIMA |
| 6 | KAMA |
| 7 | MAMA |
| 8 | T3 |
