# analyze_cal_model.py - какая калибровка у геймпада: от чего зависит размер шага АЦП и перекрёстный сдвиг оси.
#
#   C:\anaconda3\python.exe -I analyze_cal_model.py data\rec2
#
# Читает PREFIX_board.csv (снимки 0x54: t_us, rx, ry). Берёт ОДИНОЧНЫЕ шаги оси: отчёт i изменил ось A, а в отчётах
# i-3..i-1 и i+1..i+3 ось A стояла. Такой шаг - ровно один разряд АЦП (при медленном движении).
#   1. Размер шага |dA| по своей координате |A| и по другой |B|: если шаг зависит только от своей оси - калибровка
#      по осям; если от радиуса - радиальная.
#   2. Что происходит с другой осью B в том же отчёте, когда B сама стоит (до и после): dB / dA по квадрантам.
#      Радиальная модель out = u * (1 + c * r^2) даёт dB / dA = 2c * A * B / R^2 / g (знак = знак A * B).
import sys

import numpy as np

sys.stdout.reconfigure(encoding="utf-8")
prefix = sys.argv[1]
d = np.genfromtxt(prefix + "_board.csv", delimiter=",", names=True)
x = d["rx"].astype(np.int64)
y = d["ry"].astype(np.int64)
n = len(x)
dx = np.diff(x)
dy = np.diff(y)


def single_steps(da):
    """Индексы i (в массиве приращений), где ось шагнула, а 3 отчёта до и после стояла."""
    nz = da != 0
    ok = nz.copy()
    for k in (1, 2, 3):
        ok[k:] &= ~nz[:-k]
        ok[:-k] &= ~nz[k:]
    ok[:3] = False
    ok[-3:] = False
    return np.nonzero(ok)[0]


def table(name, vals, keys, edges, label):
    print(f"   {name} по {label}:")
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (keys >= lo) & (keys < hi)
        if m.sum() >= 5:
            v = vals[m]
            print(f"      {lo:6d}..{hi:6d}: шагов {m.sum():5d}, медиана {np.median(v):6.1f}, p10 {np.percentile(v, 10):6.1f}, p90 {np.percentile(v, 90):6.1f}")


edges = [0, 4000, 8000, 12000, 16000, 20000, 24000, 28000, 33000]
for an, a, b, da, db in (("X", x, y, dx, dy), ("Y", y, x, dy, dx)):
    idx = single_steps(da)
    step = np.abs(da[idx]).astype(float)
    step_ok = step < 200  # одиночный разряд, не скачок
    idx, step = idx[step_ok], step[step_ok]
    own = np.abs(a[idx])
    oth = np.abs(b[idx])
    rad = np.hypot(a[idx], b[idx])
    print(f"== ось {an}: одиночных шагов {len(idx)}, медиана {np.median(step):.1f}")
    table("шаг", step, own, edges, f"|{an}| (своя ось)")
    table("шаг", step, oth, edges, "|другая ось|")
    table("шаг", step, rad.astype(np.int64), edges, "радиусу")
    # своя ось в узкой полосе, меняется другая: разделяет "по осям" и "радиальную"
    for lo, hi in ((0, 6000), (10000, 16000)):
        m = (own >= lo) & (own < hi)
        if m.sum() >= 20:
            print(f"   своя ось {lo}..{hi}:")
            table("   шаг", step[m], oth[m], edges, "|другая ось|")
    # перекрёстный сдвиг: другая ось стояла 3 отчёта до и после по своим шагам (кроме этого отчёта)
    nzb = db != 0
    quiet = np.ones(len(idx), bool)
    for k in (1, 2, 3):
        quiet &= ~nzb[idx - k] & ~nzb[idx + k]
    ii = idx[quiet]
    ratio = db[ii] / da[ii]
    pred = 2.0 * a[ii] * b[ii] / (32768.0**2)  # радиальная модель: dB/dA = c * pred (без g)
    print(f"   перекрёстный сдвиг: шагов с тихой другой осью {len(ii)}; другая ось изменилась в {np.mean(db[ii] != 0) * 100:.1f} %")
    print(f"      dB/dA: медиана {np.median(ratio):+.4f}, p10 {np.percentile(ratio, 10):+.4f}, p90 {np.percentile(ratio, 90):+.4f}")
    for qn, m in (("A>0,B>0", (a[ii] > 0) & (b[ii] > 0)), ("A>0,B<0", (a[ii] > 0) & (b[ii] < 0)), ("A<0,B>0", (a[ii] < 0) & (b[ii] > 0)), ("A<0,B<0", (a[ii] < 0) & (b[ii] < 0))):
        if m.sum() >= 5:
            print(f"      квадрант {qn}: шагов {m.sum():5d}, dB/dA медиана {np.median(ratio[m]):+.4f}, среднее {ratio[m].mean():+.4f}")
    if len(ii) > 20:
        c = np.sum(ratio * pred) / max(1e-12, np.sum(pred * pred))
        res = ratio - c * pred
        k = ratio.mean()
        print(f"      радиальная модель: c = {c:.4f}, остаток СКО {res.std():.4f}; постоянная связь k = {k:+.4f}, остаток СКО {(ratio - k).std():.4f}; СКО самого dB/dA {ratio.std():.4f}")
        # зависимость от произведения координат
        pe = [-1.0, -0.5, -0.2, -0.05, 0.05, 0.2, 0.5, 1.0]
        for lo, hi in zip(pe[:-1], pe[1:]):
            m = (pred / 2 >= lo) & (pred / 2 < hi)
            if m.sum() >= 5:
                print(f"         A*B/R^2 {lo:+.2f}..{hi:+.2f}: шагов {m.sum():5d}, dB/dA медиана {np.median(ratio[m]):+.4f}")
