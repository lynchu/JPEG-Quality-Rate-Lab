#!/usr/bin/env python3
"""Reproduce the rate/distortion experiment on a photographic JPEG.
Requires Pillow, NumPy and Matplotlib. Every PSNR reference is the decoded
input JPEG, not the unknown pre-JPEG original photograph.
"""
import argparse
import csv
import hashlib
import json
import math
import statistics
import subprocess
from pathlib import Path
import numpy as np
from PIL import Image
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def run(tool, args):
    result = subprocess.run([str(tool), *map(str, args)], check=True, text=True, capture_output=True)
    return result.stdout.strip()


def metrics(ref, candidate):
    a = np.asarray(Image.open(ref).convert('RGB'), dtype=np.float64)
    b = np.asarray(Image.open(candidate).convert('RGB'), dtype=np.float64)
    assert a.shape == b.shape
    mse = np.mean((a-b)**2)
    return (math.inf if mse == 0 else 10*math.log10(255**2/mse)), bool(np.array_equal(a, b))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=Path)
    parser.add_argument('--tool', type=Path, default=Path('./jpegtool'))
    parser.add_argument('--out', type=Path, default=Path('results'))
    parser.add_argument('--repeats', type=int, default=7)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error('--repeats must be positive')
    tool = args.tool.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    w, h = Image.open(args.input).size
    size = args.input.stat().st_size
    rows = [dict(mode='input', bytes=size, reduction_percent=0.0, bpp=size*8/(w*h), psnr_db='inf', pixel_equal=True, median_codec_ms=0)]
    cases = [('optimize', None, None), ('scale_1.5', 'scale', '1.5'),
             ('scale_2', 'scale', '2'), ('scale_4', 'scale', '4'),
             ('quality_75', 'quality', '75'), ('quality_50', 'quality', '50'),
             ('mpeg4_1', 'mpeg4', '1'), ('mpeg4_2', 'mpeg4', '2')]
    for name, kind, argument in cases:
        output = args.out/(name+'.jpg')
        command = (['optimize', args.input, output] if not kind else
                   ['requant', args.input, output, kind, argument])
        run(tool, command)  # warmup
        times = []
        for _ in range(args.repeats):
            report = run(tool, command)
            times.append(float(report.split('elapsed_ms=')[-1]))
        psnr, equal = metrics(args.input, output)
        if equal:
            print(run(tool, ['verify', args.input, output]))
        count = output.stat().st_size
        rows.append(dict(mode=name, bytes=count, reduction_percent=100*(1-count/size),
                         bpp=count*8/(w*h), psnr_db='inf' if math.isinf(psnr) else psnr,
                         pixel_equal=equal, median_codec_ms=statistics.median(times)))
    csv_path = args.out/'measurements.csv'
    with csv_path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)
    env = dict(input_sha256=hashlib.sha256(args.input.read_bytes()).hexdigest(),
               width=w, height=h, repeats=args.repeats,
               reference='decoded input JPEG; RGB sample MSE',
               timing='C tool monotonic wall clock, warmup + median; includes JPEG read/write and temporary-file rename, excludes process launch; no fsync')
    (args.out/'experiment.json').write_text(json.dumps(env, indent=2)+'\n')
    finite = [r for r in rows if r['psnr_db'] != 'inf']
    fig, ax = plt.subplots(figsize=(8, 5), layout='constrained')
    colors = ['#2563eb' if r['mode'].startswith('scale') else '#dc7c14' for r in finite]
    ax.scatter([r['bpp'] for r in finite], [r['psnr_db'] for r in finite], c=colors, s=70)
    rightmost = max(r['bpp'] for r in finite)
    for row in finite:
        on_right = row['bpp'] == rightmost
        ax.annotate(row['mode'], (row['bpp'], row['psnr_db']),
                    xytext=(-8 if on_right else 6, 6),
                    ha='right' if on_right else 'left',
                    textcoords='offset points', fontsize=9)
    ax.set(xlabel='Whole-file bits per pixel (lower is smaller)',
           ylabel='RGB PSNR vs decoded input JPEG (dB)',
           title='Coefficient-domain JPEG requantization')
    ax.grid(alpha=.2)
    fig.savefig(args.out/'rate_distortion.png', dpi=160)
    plt.close(fig)
    selected = [('Input', args.input), ('Huffman optimized', args.out/'optimize.jpg'),
                ('DQT x 2', args.out/'scale_2.jpg'), ('DQT x 4', args.out/'scale_4.jpg')]
    fig, axes = plt.subplots(1, 4, figsize=(12, 4), layout='constrained')
    for ax, (label, path) in zip(axes, selected):
        ax.imshow(Image.open(path))
        ax.set_title(label); ax.axis('off')
    fig.savefig(args.out/'comparison.png', dpi=160)
    plt.close(fig)
    for row in rows:
        print(f"{row['mode']:12s} bytes={row['bytes']:7d} reduction={row['reduction_percent']:7.3f}% PSNR={row['psnr_db']} median_ms={row['median_codec_ms']:.3f}")


if __name__ == '__main__':
    main()
