#!/usr/bin/python3

import os
from pathlib import Path
import shutil
import subprocess

base = Path(os.getenv('MESON_DIST_ROOT'))

subprocess.run(['meson', 'compile', 'doc/html'], check=True)
shutil.copytree('doc/html', base / 'doc/html', symlinks=True)
