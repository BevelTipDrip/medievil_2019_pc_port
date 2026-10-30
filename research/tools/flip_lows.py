"""1% / 0.1% lows per 5 s from a flip trace (GOW3_FLIP_TRACE): present-to-present frame times.

    py -3.14 flip_lows.py <trace.csv> [stage]
"""
import sys,csv
path=sys.argv[1]; stage=sys.argv[2] if len(sys.argv)>2 else 'presented'
ts=[int(r['ns']) for r in csv.DictReader(open(path)) if r['stage']==stage]
ft=[(ts[i]/1e9,(ts[i]-ts[i-1])/1e6) for i in range(1,len(ts))]
t_end=ft[-1][0]
def lows(fr):
    s=sorted((x[1] for x in fr),reverse=True); n=len(s)
    a=lambda p:1000/(sum(s[:max(1,int(n*p))])/max(1,int(n*p)))
    return n/(fr[-1][0]-fr[0][0]+1e-9), a(0.01), a(0.001), s[:4]
print('frames %d, %.0f s'%(len(ft),t_end))
# 5 s timeline
i=0; rows=[]
while i<len(ft):
    j=i
    while j<len(ft) and ft[j][0]<ft[i][0]+5: j+=1
    fr=ft[i:j]; avg,l1,l01,w=lows(fr)
    rows.append((ft[i][0],avg,l1,l01,w)); i=j
for r in rows:
    flag=' <' if r[2]<160 else ''
    print('%6.0fs  avg %5.1f  1%% %5.1f  0.1%% %5.1f  worst %s%s'%(r[0],r[1],r[2],r[3],[round(x,1) for x in r[4][:3]],flag))
