#!/usr/bin/env python3
"""Conservative FAT12 standard floppy capacity precheck; not a disk-image test."""
from pathlib import Path
import sys
SECTOR = 512
DISK_SECTORS = 2880
RESERVED_SECTORS = 1
FAT_COUNT = 2
SECTORS_PER_FAT = 9
ROOT_ENTRIES = 224
ROOT_SECTORS = (ROOT_ENTRIES * 32 + (SECTOR - 1)) // SECTOR
USABLE = (DISK_SECTORS - RESERVED_SECTORS - FAT_COUNT * SECTORS_PER_FAT - ROOT_SECTORS)*SECTOR
if len(sys.argv)!=2:
    print('Uso: verificar_floppy.py build\\Proyecto144.exe')
    sys.exit(2)
f=Path(sys.argv[1]); n=f.stat().st_size
print(f'FAT12 1.44MB: {DISK_SECTORS*SECTOR} bytes brutos; {USABLE} bytes de datos disponibles en volumen vacio')
print(f'Archivo: {n} bytes. Margen de datos: {USABLE-n} bytes')
if n>USABLE:
    print('FALLO: el archivo no cabe como fichero unico en FAT12 estandar.')
    sys.exit(1)
print('PASA filtro de capacidad. AUN HAY QUE CREAR IMAGEN FAT12 REAL Y EJECUTAR EN WINDOWS.')
