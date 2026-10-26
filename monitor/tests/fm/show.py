#!/usr/bin/env python3
# show.py - from the output of run.sh keep the screen dumps (and the console with the -t flag)
import sys
t = sys.stdin.buffer.read().decode('utf-8', 'replace')
if '-t' in sys.argv:
    print(t[:t.find('=== SCREEN')])
for part in t.split('=== SCREEN')[1:]:
    body = part.split('=== END ===')[0]
    print('=== SCREEN' + body.rstrip('\n'))
