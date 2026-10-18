"""Small VT screen reader for assertions against terminal screen diffs.

Ignore styling and control replies, but apply cursor movement and erasure so
an assertion does not depend on a renderer emitting a whole phrase at once.
"""
import re
import unicodedata

TOKENS = re.compile(r'\x1b(?:\[[0-?]*[ -/]*[@-~]|[\]P_^].*?(?:\x07|\x1b\\)|.)|[^\x1b]', re.S)


def screen_text(data, rows=32, cols=120):
    grid = [[' '] * cols for _ in range(rows)]
    y = x = 0
    saved = (0, 0)
    previous = ' '
    for match in TOKENS.finditer(data.decode('utf-8', errors='replace')):
        token = match.group()
        if token.startswith('\x1b['):
            args, command = token[2:-1], token[-1]
            if args.startswith(('?', '>', '=', '<')):
                continue
            values = [int(a or 0) for a in args.split(';')] if re.fullmatch(r'[0-9;]*', args) else []
            n = (values[0] if values else 0) or 1
            if command in ('H', 'f'):
                y = n - 1
                x = ((values[1] if len(values) > 1 else 0) or 1) - 1
            elif command == 'G': x = n - 1
            elif command == 'd': y = n - 1
            elif command == 'A': y -= n
            elif command == 'B': y += n
            elif command == 'C': x += n
            elif command == 'D': x -= n
            elif command == 'J' and values and values[0] in (2, 3):
                grid = [[' '] * cols for _ in range(rows)]
            elif command == 'K' and 0 <= y < rows:
                mode = values[0] if values else 0
                start, end = (0, cols) if mode == 2 else (0, x + 1) if mode == 1 else (x, cols)
                grid[y][max(0, start):min(cols, end)] = [' '] * max(0, min(cols, end) - max(0, start))
            elif command == 'b':
                for _ in range(n):
                    if 0 <= y < rows and 0 <= x < cols: grid[y][x] = previous
                    x += 1
            continue
        if token == '\x1b7': saved = (y, x)
        elif token == '\x1b8': y, x = saved
        elif token.startswith('\x1b'): continue
        elif token == '\r': x = 0
        elif token == '\n': y += 1
        elif token == '\b': x = max(0, x - 1)
        elif token == '\t': x = (x // 8 + 1) * 8
        elif ord(token) >= 32:
            if unicodedata.combining(token):
                if 0 <= y < rows and 0 < x <= cols: grid[y][x - 1] += token
                continue
            if 0 <= y < rows and 0 <= x < cols: grid[y][x] = token
            previous = token
            x += 2 if unicodedata.east_asian_width(token) in ('W', 'F') else 1
    return '\n'.join(''.join(row) for row in grid).encode()
