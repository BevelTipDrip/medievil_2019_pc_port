"""Isolated single-frame stalls (one frame > 6.5 ms between frames < 5.5 ms) in the seconds a flip trace
runs at the frame cap, per minute: the ~7 ms stalls of N-27.

    py -3.14 flip_stalls.py <trace.csv> [<trace.csv> ...]
"""
import sys,csv,statistics
for path in sys.argv[1:]:
    ts=[int(r['ns']) for r in csv.DictReader(open(path)) if r['stage']=='presented']
    ft=[(ts[i]/1e9,(ts[i]-ts[i-1])/1e6) for i in range(1,len(ts))]
    # flat time: frames whose 1 s neighbourhood median is at the cap (< 4.6 ms)
    flat_s=0; stalls=[]; dips=[]
    sec={}
    for t,f in ft: sec.setdefault(int(t),[]).append(f)
    capsec={s for s,v in sec.items() if len(v)>150 and statistics.median(v)<4.6 and max(v)<200}
    flat_s=len(capsec)
    for i in range(1,len(ft)-1):
        t,f=ft[i]
        if int(t) in capsec and f>6.5 and ft[i-1][1]<5.5 and ft[i+1][1]<5.5: stalls.append((t,f))
    print('== %s: %d frames, %.0f s; at-cap seconds %d; isolated stalls there %d (%.1f/min); sizes %s'%(
        path.split('/')[-1],len(ft),ft[-1][0],flat_s,len(stalls),len(stalls)/flat_s*60 if flat_s else 0,
        sorted(round(f,1) for t,f in stalls)))
    print('   at', [round(t) for t,f in stalls])
    # all frames anywhere > 10 ms and < 200 (non-load)
    big=[(round(t,1),round(f,1)) for t,f in ft if 10<f<200]
    print('   frames 10-200 ms anywhere: %d'%len(big))
