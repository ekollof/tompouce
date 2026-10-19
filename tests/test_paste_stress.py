"""Unresolved terminal-input stress reproductions; intentionally not a green CTest gate.

Run `python tests/test_paste_stress.py ascii|unicode|fragmented` against the
system Notcurses integration. These retain the newly discovered failures;
the maintained PTY cases are not weakened to make them pass.
"""
import argparse
import os
from pathlib import Path
import runpy

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('case', choices=('ascii', 'unicode', 'fragmented'))
args = parser.parse_args()
# Load the existing owned PTY harness without running unrelated editor cases.
os.environ['TOMPOUCE_TEST_FILTER'] = 'paste stress'
check = runpy.run_path(str(Path(__file__).with_name('test_tui_pty.py')))['check']
if args.case == 'fragmented':
    keys = [b'\x1b[200~caf\xc3', .25, b'\xa9\x1b[201~']
    expected = 'café'
else:
    expected = '// ' + ('pattern ' if args.case == 'ascii' else 'café ') * 32768
    keys = b'\x1b[200~' + expected.encode() + b'\x1b[201~'
check('paste stress '+args.case, '', keys, expected=expected)
