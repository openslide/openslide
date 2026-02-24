#
# OpenSlide, a library for reading whole slide image files
#
# Copyright (c) 2025 Benjamin Gilbert
# All rights reserved.
#
# OpenSlide is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as
# published by the Free Software Foundation, version 2.1.
#
# OpenSlide is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with OpenSlide. If not, see
# <http://www.gnu.org/licenses/>.
#

import os
from pathlib import Path
import shutil
import subprocess
import sys
import venv

requirements = Path(sys.argv[1])
python = Path(sys.argv[2])
env_dir = python.parent.parent

if env_dir.exists() and not python.exists():
    # recreate env from scratch after "ninja clean"
    shutil.rmtree(env_dir)
if not env_dir.exists():
    venv.create(env_dir, with_pip=True)

subprocess.run(
    [
        python.with_stem('pip'),
        'install',
        '--disable-pip-version-check',
        '-q',
        '-r',
        requirements,
    ],
    check=True,
)

os.utime(python)
