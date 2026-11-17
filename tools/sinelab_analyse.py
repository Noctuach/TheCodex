#!/usr/bin/env python3
"""Analysis for CodexSineLab output.

    python3 tools/sinelab_analyse.py <outdir>

Reads the per-suite CSVs written by CodexSineLab, draws error-vs-frequency plots per suite (plots/<suite>.png),
and cross-checks a random 10 % subset of the C++ expected values with independent numpy implementations
(RBJ / Butterworth EQ bands, the nested polyphase half-band oversampler response from os_coeffs.json, the
distortion DC high-pass, the WarmSine Fourier coefficients with a 2^20-point FFT and the compressor's static curve).
Writes python_crosscheck.txt and appends its result to summary.txt.
"""
import csv
import json
import math
import os
import random
import re
import sys
from collections import defaultdict

import numpy as np

random.seed(1)


def load(outdir):
    rows = defaultdict(list)
    for name in sorted(os.listdir(outdir)):
        if name.endswith('.csv'):
            with open(os.path.join(outdir, name)) as fh:
                for r in csv.DictReader(fh):
                    rows[r['module']].append(r)
    return rows


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return float('nan')


# ---------------------------------------------------------------- plots
def plots(rows, outdir):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    pdir = os.path.join(outdir, 'plots')
    os.makedirs(pdir, exist_ok=True)
    made = []
    for suite, rs in rows.items():
        groups = defaultdict(list)
        for r in rs:
            f, e = fnum(r['f']), fnum(r['err'])
            if f > 0 and math.isfinite(e):
                groups[r['check']].append((f, e, r['pass']))
        groups = {k: v for k, v in groups.items() if len(v) >= 8}
        if not groups:
            continue
        keys = sorted(groups)[:16]
        n = len(keys)
        cols = 4 if n > 4 else n
        nr = (n + cols - 1) // cols
        fig, axes = plt.subplots(nr, cols, figsize=(4.2 * cols, 3.0 * nr), squeeze=False)
        for ax, k in zip(axes.flat, keys):
            pts = groups[k]
            for status, colour in (('PASS', '#2a6fdb'), ('INFO', '#888888'), ('FAIL', '#d62728')):
                sel = [(f, e) for f, e, p in pts if p == status]
                if sel:
                    ax.scatter([a for a, _ in sel], [b for _, b in sel], s=4, c=colour, label=status, alpha=0.7, linewidths=0)
            ax.set_xscale('log')
            ax.set_title(k, fontsize=7)
            ax.tick_params(labelsize=6)
            ax.grid(True, alpha=0.3)
        for ax in list(axes.flat)[n:]:
            ax.axis('off')
        axes.flat[0].legend(fontsize=6, markerscale=3)
        fig.suptitle(f'{suite}: error vs frequency (Hz)', fontsize=9)
        fig.tight_layout()
        path = os.path.join(pdir, f'{suite}.png')
        fig.savefig(path, dpi=110)
        plt.close(fig)
        made.append(path)
    return made


# ---------------------------------------------------------------- independent expected values
def biquad_h(c, w):
    z1 = np.exp(-1j * w)
    b0, b1, b2, a1, a2 = c
    return (b0 + b1 * z1 + b2 * z1 * z1) / (1 + a1 * z1 + a2 * z1 * z1)


def rbj(kind, f, fs, q=0.707, g=0.0):
    f = min(max(f, 5.0), 0.49 * fs)
    q = max(0.05, q)
    w0 = 2 * math.pi * f / fs
    cw, sw = math.cos(w0), math.sin(w0)
    al = sw / (2 * q)
    A = 10 ** (g / 40)
    if kind == 'lp':
        b, a = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2], [1 + al, -2 * cw, 1 - al]
    elif kind == 'hp':
        b, a = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2], [1 + al, -2 * cw, 1 - al]
    elif kind == 'bell':
        b, a = [1 + al * A, -2 * cw, 1 - al * A], [1 + al / A, -2 * cw, 1 - al / A]
    elif kind == 'notch':
        b, a = [1, -2 * cw, 1], [1 + al, -2 * cw, 1 - al]
    elif kind == 'bp':
        b, a = [al, 0, -al], [1 + al, -2 * cw, 1 - al]
    elif kind == 'ap':
        b, a = [1 - al, -2 * cw, 1 + al], [1 + al, -2 * cw, 1 - al]
    elif kind in ('ls', 'hs'):
        sq = 2 * math.sqrt(A) * al
        if kind == 'ls':
            b = [A * ((A + 1) - (A - 1) * cw + sq), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sq)]
            a = [(A + 1) + (A - 1) * cw + sq, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sq]
        else:
            b = [A * ((A + 1) + (A - 1) * cw + sq), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sq)]
            a = [(A + 1) - (A - 1) * cw + sq, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sq]
    else:
        raise ValueError(kind)
    return [b[0] / a[0], b[1] / a[0], b[2] / a[0], a[1] / a[0], a[2] / a[0]]


def first_order(hp, f, fs):
    K = math.tan(math.pi * min(max(f, 5.0), 0.49 * fs) / fs)
    a1 = (K - 1) / (K + 1)
    return [1 / (1 + K), -1 / (1 + K), 0, a1, 0] if hp else [K / (1 + K), K / (1 + K), 0, a1, 0]


def butter(hp, order, fc, userq, fs):
    if order <= 1:
        return [first_order(hp, fc, fs)]
    if order == 2:
        return [rbj('hp' if hp else 'lp', fc, fs, userq)]
    out = []
    if order % 2:
        out.append(first_order(hp, fc, fs))
        qs = [1 / (2 * math.cos(k * math.pi / order)) for k in range(1, (order - 1) // 2 + 1)]
    else:
        qs = [1 / (2 * math.cos((2 * k + 1) * math.pi / (2 * order))) for k in range(order // 2)]
    return out + [rbj('hp' if hp else 'lp', fc, fs, q) for q in qs]


SLOPES = [6, 12, 18, 24, 36, 48, 72, 96]


def eq_band(kind, f, g, q, slope, fs):
    f = min(max(f, 5.0), 0.49 * fs)
    q = max(0.05, q)
    order = slope // 6
    if kind == 'Low Cut':
        return butter(True, order, f, q, fs)
    if kind == 'High Cut':
        return butter(False, order, f, q, fs)
    if kind == 'Bell':
        return [rbj('bell', f, fs, q, g)]
    if kind == 'Notch':
        return [rbj('notch', f, fs, q)]
    if kind == 'Low Shelf':
        return [rbj('ls', f, fs, q, g)]
    if kind == 'High Shelf':
        return [rbj('hs', f, fs, q, g)]
    if kind == 'All Pass':
        return [rbj('ap', f, fs, q)]
    if kind == 'Tilt Shelf':
        return [rbj('ls', f, fs, q * 0.7, -0.5 * g), rbj('hs', f, fs, q * 0.7, 0.5 * g)]
    if kind == 'Flat Tilt':
        out = []
        for m in (1 / 16, 1, 16):
            out += [rbj('ls', f * m, fs, 0.5, -g / 6), rbj('hs', f * m, fs, 0.5, g / 6)]
        return out
    if kind == 'Band Pass':
        if order <= 2:
            return [rbj('bp', f, fs, q)]
        bw = 2 ** (1 / (2 * q))
        side = min(8, (order + 1) // 2)
        cs = butter(True, side, f / bw, 0.70710678, fs) + butter(False, side, f * bw, 0.70710678, fs)
        m = abs(np.prod([biquad_h(c, 2 * math.pi * f / fs) for c in cs]))
        if m > 1e-6:
            cs[0] = [cs[0][0] / m, cs[0][1] / m, cs[0][2] / m, cs[0][3], cs[0][4]]
        return cs
    raise ValueError(kind)


def check_eq(rows):
    out = []
    rs = [r for r in rows.get('eq', []) if r['check'] == 'zl.mag']
    sub = random.sample(rs, max(1, len(rs) // 10)) if rs else []
    worst = 0.0
    for r in sub:
        m = re.match(r'(.+?) f=([-\d.e+]+) g=([-\d.e+]+) q=([-\d.e+]+)(?: slope=(\d+)dB)?', r['params'])
        kind, f, g, q = m.group(1), float(m.group(2)), float(m.group(3)), float(m.group(4))
        slope = int(m.group(5)) if m.group(5) else 24
        fs, fr = float(r['rate']), float(r['f'])
        h = np.prod([biquad_h(c, 2 * math.pi * fr / fs) for c in eq_band(kind, f, g, q, slope, fs)])
        worst = max(worst, abs(20 * math.log10(abs(h)) - float(r['expected'])))
    out.append(('eq zl.mag expected (RBJ / Butterworth, numpy)', len(sub), worst, 1e-6))
    return out


def ap_chain(a, zi):
    p = np.ones_like(zi)
    for al in a:
        p = p * (al + zi) / (1 + al * zi)
    return p


def os_g(stages, w, level=0):
    if level >= len(stages):
        return np.ones_like(w, dtype=complex)
    s = stages[level]
    tot = 0
    for k in (0, 1):
        th = (w + 2 * math.pi * k) / 2
        z1 = np.exp(-1j * th)
        z2 = z1 * z1
        hu = ap_chain(s['upD'], z2) + z1 * ap_chain(s['upE'], z2)
        hd = 0.5 * (ap_chain(s['dnD'], z2) + z1 * ap_chain(s['dnE'], z2))
        tot = tot + hu * os_g(stages, th, level + 1) * hd
    return 0.5 * tot


def check_os(rows, outdir):
    path = os.path.join(outdir, 'os_coeffs.json')
    if not os.path.exists(path):
        return []
    co = json.load(open(path))
    rs = [r for r in rows.get('os', []) if r['check'] == 'linear.mag']
    sub = random.sample(rs, max(1, len(rs) // 10)) if rs else []
    worst = 0.0
    for r in sub:
        st = {2: 1, 4: 2, 8: 3, 16: 4}[int(re.search(r'factor=(\d+)', r['params']).group(1))]
        w = np.array([2 * math.pi * float(r['f']) / float(r['rate'])])
        g = os_g(co[str(st)], w)[0]
        worst = max(worst, abs(20 * math.log10(abs(g)) - float(r['expected'])))
    return [('os linear.mag expected (nested polyphase G0, numpy)', len(sub), worst, 1e-6)]


def check_dc(rows):
    rs = [r for r in rows.get('dist', []) if r['check'] == '3e.dc_filter.mag']
    sub = random.sample(rs, max(1, len(rs) // 10)) if rs else []
    worst = 0.0
    for r in sub:
        fs = float(r['rate'])
        R = float(np.float32(math.exp(-2 * math.pi * 10 / fs)))
        z = np.exp(-2j * math.pi * float(r['f']) / fs)
        h = (1 - z) / (1 - R * z)
        worst = max(worst, abs(20 * math.log10(abs(h)) - float(r['expected'])))
    return [('dist DC high-pass expected (numpy)', len(sub), worst, 1e-9)]


def check_warm(rows):
    rs = [r for r in rows.get('osc', []) if r['check'] == 'warmsine.harmonics']
    if not rs:
        return []
    L = 1 << 20
    th = 2 * np.pi * np.arange(L) / L
    c = 2 * np.abs(np.fft.rfft(np.tanh(2 * np.sin(th)) / np.tanh(2.0))) / L
    worst = 0.0
    sub = random.sample(rs, max(1, len(rs) // 10))
    for r in sub:
        m = int(re.search(r'm=(\d+)', r['params']).group(1))
        worst = max(worst, abs(20 * math.log10(c[m]) - float(r['expected'])))
    return [('osc WarmSine Fourier coefficients (2^20-point numpy FFT vs C++ 2^16)', len(sub), worst, 1e-6)]


STYLES = ['Clean', 'Punch', 'Smooth', 'Bus', 'Mastering', 'Classic', 'Vari-Mu', 'Opto', 'T4 Opto (LA-3A)', 'Pumping', 'TTM (Up + Down)']


def gc(x, thr, ratio, knee, rng, direction, style):
    R, W = max(1.0, ratio), max(0.0, knee)
    if style in (2, 7):
        W += 6
    if style == 8:
        R, W = 3.0, 14.0
    if style == 6:
        R = 1 + (R - 1) * min(2.5, max(0.2, 1 + (x - thr) / 18))
    s = 1 - 1 / R
    d_ = 2 if style == 10 else direction
    down = up = 0.0
    if d_ != 1:
        d = x - thr
        if W > 0 and abs(d) <= W / 2:
            down = -s * (d + W / 2) ** 2 / (2 * W)
        elif d > 0:
            down = -s * d
    if d_ != 0:
        d = thr - x
        if W > 0 and abs(d) <= W / 2:
            up = s * (d + W / 2) ** 2 / (2 * W)
        elif d > 0:
            up = s * d
        up *= min(1.0, max(0.0, (x + 80) / 20))
    return min(rng, max(-rng, down + up))


def check_comp(rows):
    rs = [r for r in rows.get('comp', []) if r['check'] == '6a.static_curve_measured']
    sub = random.sample(rs, max(1, len(rs) // 10)) if rs else []
    worst = 0.0
    for r in sub:
        p = r['params']
        style = STYLES.index(re.search(r'style=(.+?) thr=', p).group(1))
        v = {k: float(re.search(k + r'=([-\d.e+]+)', p).group(1)) for k in ('thr', 'ratio', 'knee', 'dir', 'range', 'level')}
        e = gc(v['level'], v['thr'], v['ratio'], v['knee'], v['range'], int(v['dir']), style)
        worst = max(worst, abs(e - float(r['expected'])))
    return [('comp static-curve expected (Python gain computer)', len(sub), worst, 1e-9)]


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    rows = load(outdir)
    lines = ['Python cross-check of the expected values (random 10 % subsets):']
    ok = True
    for name, n, worst, tol in check_eq(rows) + check_os(rows, outdir) + check_dc(rows) + check_warm(rows) + check_comp(rows):
        status = 'PASS' if worst <= tol else 'FAIL'
        ok = ok and status == 'PASS'
        lines.append(f'  [{status}] {name}: {n} rows, worst |C++ - Python| = {worst:.3g} (tol {tol:g})')
    # failure breakdown of the largest failing groups
    lines.append('')
    lines.append('Failures by suite / check:')
    for suite, rs in rows.items():
        cnt = defaultdict(lambda: [0, 0])
        for r in rs:
            if r['pass'] in ('PASS', 'FAIL'):
                cnt[r['check']][0] += 1
                cnt[r['check']][1] += r['pass'] == 'FAIL'
        for k, (n, f) in sorted(cnt.items()):
            if f:
                lines.append(f'  {suite:5s} {k}: {f}/{n} failed')
    # 3d breakdown by oversampling and f0
    d3 = [r for r in rows.get('dist', []) if r['check'].startswith('3d.harmonics(') and r['pass'] in ('PASS', 'FAIL')]
    if d3:
        lines.append('')
        lines.append('dist 3d harmonic checks, failures by OS and f0 (48 kHz):')
        tab = defaultdict(lambda: [0, 0])
        for r in d3:
            if float(r['rate']) != 48000.0:
                continue
            os_ = re.search(r'os=(\S+)', r['params']).group(1)
            f0 = round(float(r['f']), -1)
            tab[(os_, f0)][0] += 1
            tab[(os_, f0)][1] += r['pass'] == 'FAIL'
        for (os_, f0), (n, f) in sorted(tab.items()):
            lines.append(f'  os={os_:4s} f0={f0:7.0f} Hz: {f}/{n} failed')
    made = plots(rows, outdir)
    lines.append('')
    lines.append('Plots: ' + ', '.join(made))
    text = '\n'.join(lines) + '\n'
    print(text)
    open(os.path.join(outdir, 'python_crosscheck.txt'), 'w').write(text)
    with open(os.path.join(outdir, 'summary.txt'), 'a') as fh:
        fh.write('\n' + text)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
