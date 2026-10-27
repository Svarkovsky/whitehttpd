#!/usr/bin/env python3
"""Compatibility entry point: run the maintained real-server suite."""
import pathlib,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[2]
raise SystemExit(subprocess.call([sys.executable,str(root/'tests/regression.py'),'--variant','nolibc-x86_64',*sys.argv[1:]],cwd=root))
