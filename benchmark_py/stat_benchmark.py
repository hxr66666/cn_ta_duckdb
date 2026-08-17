"""
stat_benchmark
==============

对比测试：Python (numpy/pandas) vs DuckDB `cn_ta` 扩展的 **Eigen 金融统计与回归**
函数族（`stat_*`），校验**结果一致性**并对比**性能**。

`stat_*` 是标量函数，输入 `LIST<DOUBLE>`，返回标量或 STRUCT。参考实现用
numpy/pandas 逐项计算，与扩展结果对比。

用法:
    uv run stat_benchmark.py
    uv run stat_benchmark.py --ext /abs/path/cn_ta.duckdb_extension
    uv run stat_benchmark.py --n 10000   # 序列长度（用于性能对比）
"""

from __future__ import annotations

import argparse
import statistics
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXT = ROOT / "build" / "release" / "extension" / "cn_ta" / "cn_ta.duckdb_extension"


# --------------------------------------------------------------------------- #
# 参考实现（numpy/pandas）
# --------------------------------------------------------------------------- #
def ref_var(x: np.ndarray) -> float:
    return float(np.var(x, ddof=1))


def ref_stddev(x: np.ndarray) -> float:
    return float(np.std(x, ddof=1))


def ref_skew(x: np.ndarray) -> float:
    # 矩法偏度（与扩展一致：m3 / m2^1.5，中心矩除以 n）
    mean = x.mean()
    m2 = np.mean((x - mean) ** 2)
    m3 = np.mean((x - mean) ** 3)
    return float(m3 / m2 ** 1.5) if m2 != 0 else float("nan")


def ref_kurtosis(x: np.ndarray) -> float:
    # 矩法超额峰度（与扩展一致：m4 / m2^2 - 3）
    mean = x.mean()
    m2 = np.mean((x - mean) ** 2)
    m4 = np.mean((x - mean) ** 4)
    return float(m4 / m2 ** 2 - 3.0) if m2 != 0 else float("nan")


def ref_max_drawdown(p: np.ndarray) -> float:
    # 回撤幅度（正值，与扩展一致：(peak - p) / peak）
    peak = np.maximum.accumulate(p)
    dd = (peak - p) / peak
    return float(dd.max())


def ref_sharpe(r: np.ndarray, rf: float, ppy: float) -> float:
    excess = r - rf
    if np.std(excess, ddof=1) == 0:
        return float("nan")
    return float(excess.mean() / np.std(excess, ddof=1) * np.sqrt(ppy))


def ref_annual_vol(r: np.ndarray, ppy: float) -> float:
    return float(np.std(r, ddof=1) * np.sqrt(ppy))


def ref_cov(x: np.ndarray, y: np.ndarray) -> float:
    return float(np.cov(x, y, ddof=1)[0, 1])


def ref_corr(x: np.ndarray, y: np.ndarray) -> float:
    return float(np.corrcoef(x, y)[0, 1])


def ref_beta(x: np.ndarray, y: np.ndarray) -> float:
    # beta = cov(x,y)/var(y)
    return float(np.cov(x, y, ddof=1)[0, 1] / np.var(y, ddof=1))


def ref_regress(y: np.ndarray, x: np.ndarray) -> dict:
    # 单变量 OLS: y = slope*x + intercept
    A = np.vstack([x, np.ones(len(x))]).T
    coef, _, _, _ = np.linalg.lstsq(A, y, rcond=None)
    slope, intercept = coef[0], coef[1]
    y_hat = slope * x + intercept
    ss_res = np.sum((y - y_hat) ** 2)
    ss_tot = np.sum((y - np.mean(y)) ** 2)
    r2 = 1 - ss_res / ss_tot if ss_tot != 0 else float("nan")
    n = len(x)
    # 斜率系数的标准误 = sqrt(mse * (AtA^-1)[0,0])，与扩展 std_err 口径一致
    mse = ss_res / (n - 2) if n > 2 else float("nan")
    AtA = A.T @ A
    AtA_inv = np.linalg.inv(AtA)
    std_err = float(np.sqrt(mse * AtA_inv[0, 0])) if n > 2 else float("nan")
    return {"slope": float(slope), "intercept": float(intercept), "r2": float(r2), "std_err": std_err}


# --------------------------------------------------------------------------- #
# DuckDB 连接
# --------------------------------------------------------------------------- #
def setup_duckdb(ext_path: str):
    import duckdb

    con = duckdb.connect(config={"allow_unsigned_extensions": True})
    if not Path(ext_path).exists():
        raise FileNotFoundError(f"扩展未找到: {ext_path}")
    con.execute(f"LOAD '{ext_path}';")
    return con


def to_list_sql(arr: np.ndarray) -> str:
    """把 numpy 数组转成 DuckDB list_value(...) 字面量。"""
    return "list_value(" + ",".join(repr(float(v)) for v in arr) + ")"


def compare(name: str, got, expected, tol: float = 1e-9) -> None:
    if got is None or expected is None or (isinstance(expected, float) and np.isnan(expected)):
        ok = got is None or (got is not None and np.isnan(got))
    else:
        ok = abs(float(got) - float(expected)) < tol
    status = "OK" if ok else "DIFF"
    print(f"  {name:28s} 扩展={got!s:>18s}  参考={expected!s:>18s}  [{status}]")


# --------------------------------------------------------------------------- #
# 一致性测试
# --------------------------------------------------------------------------- #
def test_consistency(con) -> None:
    print("\n=== stat_* 结果一致性 (numpy/pandas vs cn_ta) ===")

    rng = np.random.default_rng(42)
    x = rng.normal(0, 1, 100)
    y = 2.0 * x + 1.0 + rng.normal(0, 0.5, 100)  # y = 2x + 1 + 噪声
    prices = 100 * np.exp(np.cumsum(rng.normal(0, 0.01, 200)))  # 随机游走价格
    returns = rng.normal(0.001, 0.02, 252)

    xs, ys = to_list_sql(x), to_list_sql(y)
    ps = to_list_sql(prices)
    rs = to_list_sql(returns)

    # 单变量统计
    compare("stat_var", con.execute(f"SELECT stat_var({xs})").fetchone()[0], ref_var(x))
    compare("stat_stddev", con.execute(f"SELECT stat_stddev({xs})").fetchone()[0], ref_stddev(x))
    compare("stat_skew", con.execute(f"SELECT stat_skew({xs})").fetchone()[0], ref_skew(x))
    compare("stat_kurtosis", con.execute(f"SELECT stat_kurtosis({xs})").fetchone()[0], ref_kurtosis(x))
    compare("stat_max_drawdown", con.execute(f"SELECT stat_max_drawdown({ps})").fetchone()[0], ref_max_drawdown(prices))
    compare("stat_sharpe", con.execute(f"SELECT stat_sharpe({rs}, 0.02, 252)").fetchone()[0], ref_sharpe(returns, 0.02, 252))
    compare("stat_annual_vol", con.execute(f"SELECT stat_annual_vol({rs}, 252)").fetchone()[0], ref_annual_vol(returns, 252))

    # 双变量统计
    compare("stat_cov", con.execute(f"SELECT stat_cov({xs}, {ys})").fetchone()[0], ref_cov(x, y))
    compare("stat_corr", con.execute(f"SELECT stat_corr({xs}, {ys})").fetchone()[0], ref_corr(x, y))
    compare("stat_beta", con.execute(f"SELECT stat_beta({ys}, {xs})").fetchone()[0], ref_beta(y, x))

    # 回归
    ref = ref_regress(y, x)
    row = con.execute(f"SELECT stat_regress({ys}, {xs}).slope, stat_regress({ys}, {xs}).intercept, "
                      f"stat_regress({ys}, {xs}).r2, stat_regress({ys}, {xs}).std_err").fetchone()
    compare("stat_regress.slope", row[0], ref["slope"], 1e-6)
    compare("stat_regress.intercept", row[1], ref["intercept"], 1e-6)
    compare("stat_regress.r2", row[2], ref["r2"], 1e-6)
    compare("stat_regress.std_err", row[3], ref["std_err"], 1e-6)


# --------------------------------------------------------------------------- #
# 性能测试
# --------------------------------------------------------------------------- #
def test_performance(con, n: int) -> None:
    print(f"\n=== stat_* 性能对比 (序列长度 {n}) ===")
    print("  (扩展耗时 = SQL 内 list() 聚合 + 计算，numpy 耗时 = 纯数组计算)")

    rng = np.random.default_rng(7)
    x = rng.normal(0, 1, n)
    y = 2.0 * x + 1.0 + rng.normal(0, 0.5, n)

    # 先把数据导入 DuckDB 内存表，真实场景下 stat_* 的数据本就在库里
    import pandas as pd
    con.execute("DROP TABLE IF EXISTS bench_data")
    df = pd.DataFrame({"x": x, "y": y})
    con.register("bench_df", df)
    con.execute("CREATE TABLE bench_data AS SELECT * FROM bench_df")

    def bench_sql(fn, repeats=20):
        fn()  # 预热
        ts = []
        for _ in range(repeats):
            t0 = time.perf_counter()
            fn()
            ts.append(time.perf_counter() - t0)
        return statistics.mean(ts), min(ts)

    def bench_numpy(fn, repeats=1000):
        fn()  # 预热
        ts = []
        for _ in range(repeats):
            t0 = time.perf_counter()
            fn()
            ts.append(time.perf_counter() - t0)
        return statistics.mean(ts), min(ts)

    cases = [
        ("stat_var", lambda: con.execute("SELECT stat_var(list(x)) FROM bench_data").fetchone(), lambda: ref_var(x)),
        ("stat_stddev", lambda: con.execute("SELECT stat_stddev(list(x)) FROM bench_data").fetchone(), lambda: ref_stddev(x)),
        ("stat_corr", lambda: con.execute("SELECT stat_corr(list(x), list(y)) FROM bench_data").fetchone(), lambda: ref_corr(x, y)),
        ("stat_regress", lambda: con.execute("SELECT stat_regress(list(y), list(x)) FROM bench_data").fetchone(), lambda: ref_regress(y, x)),
    ]

    print(f"  {'函数':18s} {'扩展(ms)':>12s} {'numpy(ms)':>12s} {'扩展/numpy':>10s}")
    for name, sql_fn, np_fn in cases:
        s_mean, _ = bench_sql(sql_fn)
        n_mean, _ = bench_numpy(np_fn)
        ratio = s_mean / n_mean if n_mean > 0 else float("inf")
        print(f"  {name:18s} {s_mean*1000:12.3f} {n_mean*1000:12.3f} {ratio:9.2f}x")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ext", default=str(DEFAULT_EXT))
    ap.add_argument("--n", type=int, default=10000)
    args = ap.parse_args()

    con = setup_duckdb(args.ext)
    test_consistency(con)
    test_performance(con, args.n)
    print("\n完成。")


if __name__ == "__main__":
    main()
