#!/usr/bin/env python3
"""dolphin_fields.py -- the console field (VCounter) each frame of a Dolphin
frame dump shows (M51, PLAN.md 66.5).

  dolphin_fields.py FRAMES.avi [--offset K] > FIELDS

Dolphin stamps each dumped frame with the emulated time since the boot; the
game's VCounter counts fields from its PADInit, the frame dump lags it: field = round(t x 59.94) + 10, measured by matching pictures (PLAN.md 66.5)
(the poll calibration: PADRead at VCounter v reads poll 2v + 53, two polls a
field from the boot).  field = round(t x 59.94) - K."""
import os, subprocess, sys
off = -10
args = sys.argv[1:]
if '--offset' in args:
    i = args.index('--offset')
    off = int(args[i + 1])
    del args[i:i + 2]
ffprobe = os.environ.get('FFPROBE', os.path.expanduser('~/bin/ffprobe'))
out = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'frame=pts_time,best_effort_timestamp_time',
                      '-of', 'csv=p=0', args[0]], capture_output=True, text=True).stdout
for line in out.split():
    t = line.split(',')[0] or line.split(',')[-1]
    try:
        print(round(float(t) * 60000 / 1001) - off)
    except ValueError:
        pass
