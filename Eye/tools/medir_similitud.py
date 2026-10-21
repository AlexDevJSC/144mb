#!/usr/bin/env python3
"""Developer-only aligned texture SSIM; requires Pillow and scikit-image.

Usage: python tools/medir_similitud.py references/texture_crops candidate_texture_crops
Crops must be independent original-like material samples at SAME viewpoint,
scale and alignment. Screenshot crops with perspective are not valid.
"""
from pathlib import Path
import sys
from PIL import Image
import numpy as np
from skimage.metrics import structural_similarity as ssim

MATERIALS = ['entramado', 'vigas', 'ventana', 'puerta', 'tejado', 'cimiento', 'camino', 'fronton']
THRESHOLD = 0.84


def main():
    if len(sys.argv) != 3:
        print('Uso: medir_similitud.py DIRECTORIO_REFERENCIAS DIRECTORIO_EYE', file=sys.stderr)
        return 2
    refs, gen = map(Path, sys.argv[1:])
    failed = []
    for name in MATERIALS:
        a, b = refs/(name+'.png'), gen/(name+'.png')
        if not a.is_file() or not b.is_file():
            failed.append(name)
            print(f'{name}: SIN REFERENCIA ALINEADA (no se puede certificar similitud)')
            continue
        aa = np.asarray(Image.open(a).convert('RGB'), dtype=np.uint8)
        bb = np.asarray(Image.open(b).convert('RGB'), dtype=np.uint8)
        if aa.shape != bb.shape:
            print(f'{name}: dimensiones distintas: {aa.shape} / {bb.shape}')
            failed.append(name)
            continue
        v = float(ssim(aa, bb, data_range=255, channel_axis=2))
        print(f'{name}: SSIM={v:.4f} -> {"PASA" if v >= THRESHOLD else "NO PASA"}')
        if v < THRESHOLD: failed.append(name)
    print('Criterio: TODAS las 8 texturas deben superar SSIM >= 0.84; además, inspección visual humana.')
    if failed:
        print('PENDIENTES:', ', '.join(failed))
        return 1
    print('Pasa la métrica SSIM. Esto NO implica un 84% de parecido perceptual universal.')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
