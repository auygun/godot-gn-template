#!/usr/bin/env python3
"""Print 1 if the named binary is on PATH, else 0 (ccache detection)."""

import sys
import shutil

if shutil.which(sys.argv[1]):
  print("1")
else:
  print("0")
