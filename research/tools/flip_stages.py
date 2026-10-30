"""Per-frame pipeline stages from a flip trace (GOW3_FLIP_TRACE): submit -> irq (stage A) -> queued
(recorded) -> gpu_ready -> present_start -> presented. Ranges in trace seconds, or spike:a-b for the
frames around each frame over 7 ms in that range.

    py -3.14 flip_stages.py <trace.csv> 441.0-446 spike:80-120
"""
import sys,csv,collections
path=sys.argv[1]; specs=sys.argv[2:]
ev=collections.defaultdict(dict)
for r in csv.DictReader(open(path)):
    ev[int(r['flip_arg'])].setdefault(r['stage'],int(r['ns']))
args=sorted(a for a in ev if 'presented' in ev[a])
pres=[(a,ev[a]['presented']) for a in args]
def row(i):
    a,t=pres[i]; e=ev[a]; ms=lambda x,y:(e[y]-e[x])/1e6 if x in e and y in e else float('nan')
    prev_sub=ev[pres[i-1][0]].get('submit')
    return '%8.3fs ft %6.2f | submit gap %6.2f | submit>irq %5.2f irq>queued %5.2f queued>gpu_ready %5.2f gpu_ready>present_start %5.2f present %5.2f'%(
        t/1e9,(t-pres[i-1][1])/1e6,(e['submit']-prev_sub)/1e6 if prev_sub and 'submit' in e else float('nan'),
        ms('submit','irq'),ms('irq','queued'),ms('queued','gpu_ready'),ms('gpu_ready','present_start'),ms('present_start','presented'))
for s in specs:
    if s.startswith('spike:'):
        a,b=map(float,s[6:].split('-'))
        for i in range(1,len(pres)):
            if a<=pres[i][1]/1e9<b and (pres[i][1]-pres[i-1][1])/1e6>7:
                for k in range(i-3,i+3): print(row(k))
                print('-')
    else:
        a,b=map(float,s.split('-'))
        for i in range(1,len(pres)):
            if a<=pres[i][1]/1e9<b: print(row(i))
