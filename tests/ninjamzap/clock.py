#!/usr/bin/env python3
# Copyright (c) 2026 Javier Cordero
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does the sender's audio clock lose time? Gaps between its audio interval BEGINs, against the
nominal interval, for any Jamtaba binary. usage: clock.py <app binary> <seconds> <camera:0|1> <avmarks:0|1>"""
import json,os,sys,tempfile,time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import suite
app,secs,camera,marks=sys.argv[1],int(sys.argv[2]),sys.argv[3]=="1",sys.argv[4]=="1"
suite.APP=app
if not marks:
    env=dict(os.environ)
wd=tempfile.mkdtemp(prefix="clock",dir=suite.PEERS)
srv=suite.Server(wd); sn=suite.Sniffer(srv.port); jt=suite.Jamtaba(wd)
try:
    jt.connect(srv.port)
    if camera: jt.cmd("camera on")
    time.sleep(secs)
    a=[b["t"] for b in sn.of(suite.USER,"OGGv",0)]
    gaps=[y-x for x,y in zip(a,a[1:])]
    late=[round(g-suite.INTERVAL,2) for g in gaps if abs(g-suite.INTERVAL)>0.15]
    print(json.dumps(dict(app=os.path.basename(app),camera=camera,intervals=len(gaps),off_nominal=late,lost_s=round(sum(gaps)-suite.INTERVAL*len(gaps),2),rss=round(jt.rss_mb()),video=len(sn.of(suite.USER,"H264")))))
finally:
    jt.stop(); sn.stop(); srv.stop()
