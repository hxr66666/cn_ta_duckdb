"""
test_stat_resample
==================

stat_resample 的快速验证脚本（仅覆盖本次改动，不跑全量测试套件）。

用法:
    uv run --directory benchmark_py test_stat_resample.py

前置: 先运行 scripts/build_linux.sh 重建 release 扩展（包含新函数）。
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import duckdb
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXT = ROOT / "build" / "release" / "extension" / "cn_ta" / "cn_ta.duckdb_extension"


# --------------------------------------------------------------------------- #
# 参考实现（与 C++ StatResample 同算法：
#   过滤 NaN -> 总体标准差 z-score -> 线性插值到 n 点）
# --------------------------------------------------------------------------- #
def ref_resample(x: np.ndarray, n: int):
    valid = x[~np.isnan(x)]
    if valid.size < 2 or n < 2:
        return None
    sd = valid.std(ddof=0)  # 总体标准差 ÷N
    if sd == 0.0:
        return None
    norm = (valid - valid.mean()) / sd
    if norm.size == n:
        return norm
    m = float(norm.size)
    pos = np.arange(n, dtype=float) * (m - 1.0) / (n - 1.0)
    return np.interp(pos, np.arange(norm.size, dtype=float), norm)


def to_list_sql(x: np.ndarray) -> str:
    parts = ["NULL" if math.isnan(v) else repr(float(v)) for v in x]
    return "list_value(" + ",".join(parts) + ")"


def check(name: str, got, expected, tol: float = 1e-9) -> bool:
    if expected is None:
        ok = got is None
    else:
        got_arr = np.asarray(got, dtype=float)
        exp_arr = np.asarray(expected, dtype=float)
        ok = got_arr.shape == exp_arr.shape and np.allclose(got_arr, exp_arr, atol=tol)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    if not ok:
        print(f"        got      = {got}")
        print(f"        expected = {expected}")
    return ok


def main() -> int:
    if not DEFAULT_EXT.exists():
        print(f"扩展未找到: {DEFAULT_EXT}\n请先运行 scripts/build_linux.sh 重建扩展。", file=sys.stderr)
        return 2
    con = duckdb.connect(config={"allow_unsigned_extensions": True})
    con.execute(f"LOAD '{DEFAULT_EXT}';")

    def q(sql: str):
        return con.execute(sql).fetchone()[0]

    ok = True
    arr6 = np.arange(100.0, 106.0)

    # 1. 输出长度 == n（多组 n，含 n > 有效点数）
    for n in (2, 3, 5, 6, 128):
        got = q(f"SELECT len(stat_resample({to_list_sql(arr6)}, {n}))")
        ok = check(f"输出长度 == {n}", got, n) and ok

    # 2. 数值一致性：与参考实现逐点对比（含 n==有效点数特判 n=6）
    for n in (3, 4, 5, 6):
        got = q(f"SELECT stat_resample({to_list_sql(arr6)}, {n})")
        ok = check(f"[100..105] n={n} 与参考一致", got, ref_resample(arr6, n)) and ok

    # 3. 插值后均值≈0、stddev≈1（插值会略微改变分布，容差断言）
    got = con.execute(
        "SELECT abs(list_avg(stat_resample(list_value(100.0,101.0,102.0,103.0,104.0,105.0), 5))), "
        "abs(list_stddev_pop(stat_resample(list_value(100.0,101.0,102.0,103.0,104.0,105.0), 5))) - 1.0"
    ).fetchone()
    ok = check("插值后 |mean| < 1e-9", got[0], 0.0, tol=1e-9) and ok
    ok = check("插值后 |stddev-1| < 0.05", got[1], 0.0, tol=0.05) and ok

    # 4. n == 有效点数：严格 mean=0 / stddev=1
    got = con.execute(
        "SELECT abs(list_avg(stat_resample(list_value(100.0,101.0,102.0,103.0,104.0,105.0), 6))), "
        "abs(list_stddev_pop(stat_resample(list_value(100.0,101.0,102.0,103.0,104.0,105.0), 6))) - 1.0"
    ).fetchone()
    ok = check("n==点数 |mean| < 1e-9", got[0], 0.0, tol=1e-9) and ok
    ok = check("n==点数 |stddev-1| < 1e-9", got[1], 0.0, tol=1e-9) and ok

    # 5. 不同价位相同形态：z-score 消除量纲
    arr1000 = np.arange(1000.0, 1060.0, 10.0)
    ga = q(f"SELECT stat_resample({to_list_sql(arr6)}, 4)")
    gb = q(f"SELECT stat_resample({to_list_sql(arr1000)}, 4)")
    ok = check("不同价位形态一致", ga, gb) and ok

    # 6. NULL 元素过滤：[1, NULL, 2, NULL, 3] -> 有效 [1,2,3]
    arr_nan = np.array([1.0, np.nan, 2.0, np.nan, 3.0])
    got = q(f"SELECT stat_resample({to_list_sql(arr_nan)}, 3)")
    ok = check("NULL 元素过滤后归一化", got, ref_resample(arr_nan, 3)) and ok

    # 7. 边界 -> NULL：常数序列 / n<2 / 单点 / 空序列 / n 为 NULL / 序列为 NULL
    edges = [
        ("常数序列", "SELECT stat_resample(list_value(5.0,5.0,5.0), 3)"),
        ("n=1", "SELECT stat_resample(list_value(1.0,2.0,3.0), 1)"),
        ("单点", "SELECT stat_resample(list_value(7.0), 3)"),
        ("空序列", "SELECT stat_resample(CAST([] AS DOUBLE[]), 3)"),
        ("n 为 NULL", "SELECT stat_resample(list_value(1.0,2.0,3.0), NULL)"),
        ("序列为 NULL", "SELECT stat_resample(NULL, 3)"),
    ]
    for name, sql in edges:
        ok = check(f"边界: {name} -> NULL", q(sql), None) and ok

    # 8. VSS 核心用例：输出可直接 CAST 成 FLOAT[5]
    got = q(
        "SELECT len(CAST(stat_resample(list_value(100.0,101.0,102.0,103.0,104.0,105.0), 5) AS FLOAT[5]))"
    )
    ok = check("CAST AS FLOAT[5] 长度", got, 5) and ok

    con.close()
    print("\n" + ("全部通过 ✅" if ok else "存在失败 ❌"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
