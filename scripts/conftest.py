"""Make the ``dbqa`` package importable from any working directory."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
