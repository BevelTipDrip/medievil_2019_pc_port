"""Export a time range of an ETL to an xperf dumper CSV (with stacks), and print the trace's span.

    py -3.14 etw_export.py <trace.etl>                          # span only (start / end, local)
    py -3.14 etw_export.py <trace.etl> <from_s> <to_s> <out.csv> # ETW seconds from the trace start

Each export re-reads the whole ETL (~1 min per 3 GB). Sizes: ~1 GB per 10 s with the built-in CPU profile,
~60 MB per second with wpr-jitter.wprp. A WPR ring keeps only the last ~75-90 s of events although the header
spans the whole session: an export before that holds nothing; during a -stop the samples go on without
switches or stacks. Write exports to the scratchpad, not the repo, and delete them when done.
"""
import os
import sys
import etw_common as E

etl = sys.argv[1]
start, end = E.etl_span(etl)
print(f'{etl}: {start} -> {end} ({(end - start).total_seconds():.1f} s)')
if len(sys.argv) >= 5:
    a, b, out = float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
    E.export(etl, a, b, out)
    print(f'{out}: {os.path.getsize(out) / 1e6:.0f} MB')
