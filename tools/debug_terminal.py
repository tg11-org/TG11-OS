#!/usr/bin/env python3
"""Fix terminal.c stack frame builder - handles CRLF line endings."""
import sys, os

path = r'U:\Projects\TG11-OS\kernel\terminal.c'

with open(path, 'rb') as f:
    raw = f.read()

crlf = b'\r\n' in raw
print(f'CRLF: {crlf}, size: {len(raw)}')

# Work with LF-normalised text
text = raw.replace(b'\r\n', b'\n').decode('utf-8')

# Look for the anchor line
anchor = 'argv string packing while we stabilize'
idx = text.find(anchor)
print(f'Anchor found at: {idx}')
if idx < 0:
    sys.exit(1)

# Show context
print(repr(text[idx-100:idx+100]))
