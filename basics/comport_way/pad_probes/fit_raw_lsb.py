# fit_raw_lsb.py - размер младшего разряда АЦП геймпада в единицах выхода и его зависимость от положения стика.
#
# Запуск: C:\anaconda3\python.exe -I fit_raw_lsb.py data\rec2
#
# Берутся "одиночные шаги": отчёт, где ось изменилась, а до и после него эта ось стояла (один разряд АЦП).
# Модель калибровки геймпада: выход = u * G(r), G(r) = 1 + c * (r / 32768)^2, u - "сырые" координаты (уровень * lsb).
# Тогда шаг по оси X при одном разряде: lsb * (1 + c * (r^2 + 2 * x^2) / 32768^2). Подгонка МНК по обеим осям.
# Ещё печатает: соотношение "перекрёстного" изменения другой оси и долю полок чётной длины.
import sys

import numpy as np


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    path = sys.argv[1] + "_board.csv"
    with open(path, "r", encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        rows = [line.strip().split(",") for line in f if line.strip()]
    x = np.array([float(r[head.index("rx")]) for r in rows])
    y = np.array([float(r[head.index("ry")]) for r in rows])
    dx, dy = np.diff(x), np.diff(y)
    feats, steps = [], []
    for v, d, o in ((x, dx, y), (y, dy, x)):
        i = np.where((d[1:-1] != 0) & (d[:-2] == 0) & (d[2:] == 0))[0] + 1
        s = np.abs(d[i])
        pos, other = v[i], o[i]
        keep = (s >= 20) & (s <= 200)
        r2 = (pos[keep] ** 2 + other[keep] ** 2 + 2.0 * pos[keep] ** 2) / 32768.0 ** 2
        feats.append(r2)
        steps.append(s[keep])
        print(f"одиночных шагов {int(keep.sum())}: |d| p5 {np.percentile(s[keep], 5):.0f}, медиана {np.median(s[keep]):.0f}, p95 {np.percentile(s[keep], 95):.0f}, "
              f"min {s[keep].min():.0f}, max {s[keep].max():.0f}")
        near = keep & (np.hypot(pos, other) < 6000)
        far = keep & (np.hypot(pos, other) > 24000)
        if near.sum() > 5:
            print(f"   у центра (r < 6000): медиана {np.median(s[near]):.0f} (шагов {int(near.sum())})")
        if far.sum() > 5:
            print(f"   у края (r > 24000): медиана {np.median(s[far]):.0f} (шагов {int(far.sum())})")
    f_all, s_all = np.concatenate(feats), np.concatenate(steps)
    # два разряда за отчёт дают вдвое больший шаг: оставить нижнюю "ветку" (не больше 1.5 медианы)
    low = s_all <= 1.5 * np.median(s_all)
    a = np.vstack([np.ones(low.sum()), f_all[low]]).T
    coef, *_ = np.linalg.lstsq(a, s_all[low], rcond=None)
    lsb, c = coef[0], coef[1] / coef[0]
    res = s_all[low] - a @ coef
    print(f"подгонка: lsb = {lsb:.2f} единиц у центра, c = {c:.3f} (шаг = lsb * (1 + c * (r^2 + 2 x^2) / 32768^2)); остаток СКО {np.std(res):.2f}, точек {int(low.sum())}")
    print(f"   то есть уровней на ось около {65536.0 / lsb:.0f} (2^{np.log2(65536.0 / lsb):.2f})")
    act = (dx != 0) | (dy != 0)
    runs, n = [], 0
    for v in act:
        if not v:
            n += 1
        elif n:
            runs.append(n)
            n = 0
    runs = np.array(runs)
    short = runs[runs <= 16]
    print(f"полки длиной до 16: нечётных {int((short % 2 == 1).sum())}, чётных {int((short % 2 == 0).sum())} ({100.0 * (short % 2 == 0).mean():.1f} %)")


if __name__ == "__main__":
    main()
