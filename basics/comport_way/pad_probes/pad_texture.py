# pad_texture.py - панель PadTexture19: описатели текстуры потока стика геймпада на 1 кГц.
# Эталон на Python: по нему сверяется реализация в makcu_gamepad_renderer.h (PadTexture19).
# Аналог ABCurves/abcurves/texture.py (Texture19), но для абсолютной координаты стика.
#
# Вход: ряд положений (x, y) по одному на отчёт (1 мс). Считается по приращениям d = diff(положение).
#   mag  = max(|dx|, |dy|)       - "есть ли изменение" (полка, если 0)
#   norm = sqrt(dx^2 + dy^2)     - величина шага
#
# 19 пунктов (пункты 7 и 18 парные, всего 21 число) + delta_ac_lag2 сверх панели: у живого геймпада и у
# интерполятора платы приращения чередуются с периодом 2 отчёта, и лаг 2 показывает это лучше лагов 1 и 4.
import math

import numpy as np

NAMES = (
    "hold_rate",                 # 1  доля отчётов без изменения
    "step_grid_adherence",       # 2  доля ненулевых приращений, кратных шагу сетки Q
    "active_delta_mean_log1p",   # 3
    "active_delta_max_log1p",    # 4
    "active_delta_p90_log1p",    # 5
    "active_delta_p99_log1p",    # 6
    "sign_flip_rate_x",          # 7a доля смен знака среди ненулевых dx
    "sign_flip_rate_y",          # 7b
    "hold_run_mean_log1p",       # 8  средняя длина полки
    "active_run_mean_log1p",     # 9  средняя длина отрезка с изменениями
    "hold_run_std_log1p",        # 10
    "hold_run_p95_log1p",        # 11
    "bp_4_8_hz",                 # 12 доля мощности спектра norm в полосе
    "bp_8_14_hz",                # 13
    "bp_18_28_hz",               # 14
    "bp_30_120_hz",              # 15
    "bp_120_500_hz",             # 16
    "hf_power_log1p",            # 17 мощность 30..500 Гц на отчёт
    "delta_ac_lag1",             # 18a автокорреляция norm
    "delta_ac_lag4",             # 18b
    "quantized_step_entropy",    # 19 энтропия шагов |d|/Q по двоичным разрядам
    "delta_ac_lag2",             # сверх панели
)
BANDS = ((4.0, 8.0), (8.0, 14.0), (18.0, 28.0), (30.0, 120.0), (120.0, 500.0))
ENTROPY_BINS = 16


def _runs(active):
    hold, act = [], []
    if len(active) == 0:
        return hold, act
    cur, n = bool(active[0]), 1
    for v in active[1:]:
        if bool(v) == cur:
            n += 1
        else:
            (act if cur else hold).append(n)
            cur, n = bool(v), 1
    (act if cur else hold).append(n)
    return hold, act


def _ac(v, lag):
    if len(v) < lag + 4:
        return 0.0
    a = v[:-lag] - np.mean(v[:-lag])
    b = v[lag:] - np.mean(v[lag:])
    den = float(np.sqrt(np.sum(a * a) * np.sum(b * b)))
    return float(np.sum(a * b) / den) if den >= 1e-9 else 0.0


def features(x, y, q=1):
    """Панель для одного окна положений (x, y), по отчёту на мс. Возвращает массив len(NAMES)."""
    dx = np.diff(np.asarray(x, dtype=np.float64))
    dy = np.diff(np.asarray(y, dtype=np.float64))
    n = len(dx)
    out = np.zeros(len(NAMES))
    if n < 8:
        return out
    mag = np.maximum(np.abs(dx), np.abs(dy))
    norm = np.hypot(dx, dy)
    active = mag > 0
    an = norm[active]
    out[0] = 1.0 - float(np.mean(active))
    nz = np.concatenate([np.abs(dx[dx != 0]), np.abs(dy[dy != 0])])
    out[1] = float(np.mean(np.mod(nz, q) == 0)) if len(nz) else 1.0
    if len(an):
        out[2] = math.log1p(float(np.mean(an)))
        out[3] = math.log1p(float(np.max(an)))
        out[4] = math.log1p(float(np.percentile(an, 90)))
        out[5] = math.log1p(float(np.percentile(an, 99)))
    for i, d in enumerate((dx, dy)):
        s = np.sign(d)
        s = s[s != 0]
        out[6 + i] = float(np.mean(s[1:] * s[:-1] < 0)) if len(s) > 1 else 0.0
    hold, act = _runs(active)
    out[8] = math.log1p(float(np.mean(hold))) if hold else 0.0
    out[9] = math.log1p(float(np.mean(act))) if act else 0.0
    out[10] = math.log1p(float(np.std(hold))) if len(hold) >= 2 else 0.0
    out[11] = math.log1p(float(np.percentile(hold, 95))) if hold else 0.0
    power = np.abs(np.fft.rfft(norm - np.mean(norm))) ** 2
    freq = np.fft.rfftfreq(n, d=1e-3)
    total = max(float(np.sum(power[1:])), 1e-9)
    for i, (lo, hi) in enumerate(BANDS):
        out[12 + i] = float(np.sum(power[(freq >= lo) & (freq < hi)])) / total
    out[17] = math.log1p(float(np.sum(power[(freq >= 30.0) & (freq < 500.0)])) / n)
    out[18] = _ac(norm, 1)
    out[19] = _ac(norm, 4)
    steps = np.rint(norm / q).astype(np.int64)
    bins = np.where(steps <= 0, 0, 1 + np.floor(np.log2(np.maximum(steps, 1))).astype(np.int64))
    hist = np.bincount(np.clip(bins, 0, ENTROPY_BINS - 1), minlength=ENTROPY_BINS).astype(np.float64)
    p = hist / max(float(hist.sum()), 1.0)
    p = p[p > 0]
    out[20] = float(-np.sum(p * np.log(p)) / math.log(ENTROPY_BINS))
    out[21] = _ac(norm, 2)
    out[~np.isfinite(out)] = 0.0
    return out


def windows(x, y, q=1, size=512, min_active=0.05):
    """Панель по непересекающимся окнам; окна почти без движения (активных отчётов меньше min_active) пропускаются."""
    rows = []
    for i in range(0, len(x) - size, size):
        f = features(x[i:i + size + 1], y[i:i + size + 1], q)
        if 1.0 - f[0] >= min_active:
            rows.append(f)
    return np.array(rows) if rows else np.zeros((0, len(NAMES)))


def w1(a, b):
    """Расстояние Вассерштейна W1 по каждому описателю между двумя наборами окон (сетка 199 квантилей)."""
    grid = np.linspace(0.0, 1.0, 201)[1:-1]
    return np.array([float(np.mean(np.abs(np.quantile(a[:, c], grid) - np.quantile(b[:, c], grid)))) for c in range(len(NAMES))])
