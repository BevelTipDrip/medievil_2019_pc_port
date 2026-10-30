"""Per-thread on-CPU timeline around one moment of an xperf dumper export (CSwitch records): who stops
when a frame stalls. Times in the export's microseconds.

    py -3.14 etw_timeline.py <dump.csv> <t_us> <before_us> <after_us> <bin_us> <tid>,<tid>,...
"""
import sys,collections
dump=sys.argv[1]; t_end=int(sys.argv[2]); before=int(sys.argv[3]); after=int(sys.argv[4]); bin_us=int(sys.argv[5]); tids=sys.argv[6].split(',')
GAME='medievil-runtime.exe'; A=t_end-before; B=t_end+after
nb=(B-A)//bin_us; occ={t:[0]*nb for t in tids}; running={}
def add(t,a,b):
    if t not in occ: return
    a=max(a,A); b=min(b,B)
    while a<b:
        i=(a-A)//bin_us; e=min(b,A+(i+1)*bin_us); occ[t][i]+=e-a; a=e
for line in open(dump,encoding='utf-8',errors='replace'):
    if line[:26].strip().startswith('CSwitch,'):
        p=[x.strip() for x in line.split(',')]
        if not p[1].isdigit(): continue
        ts=int(p[1]); cpu=p[16]
        if cpu in running: add(running[cpu][0],running[cpu][1],ts)
        running[cpu]=(p[3],ts)
for cpu,(t,s) in running.items(): add(t,s,B)
mark=''.join('|' if A+i*bin_us<=t_end<A+(i+1)*bin_us else ' ' for i in range(nb))
print('%8s %s   (each char %d us; | = stall frame presented; # >=75%% busy, + >=25%%, . >0)'%('',mark,bin_us))
for t in tids:
    print('%8s %s'%(t,''.join('#' if v>=0.75*bin_us else '+' if v>=0.25*bin_us else '.' if v>0 else ' ' for v in occ[t])))
