import os, sys, time

pid = int(sys.argv[1])
prev = {}
tids = sorted(int(t) for t in os.listdir(f"/proc/{pid}/task"))
for tid in tids:
    try:
        s = open(f"/proc/{pid}/task/{tid}/stat").read()
    except OSError:
        continue
    r = s.rindex(")")
    comm = s[s.index("(")+1:r]
    f = s[r+2:].split()
    state = f[0]
    ut, st = int(f[11]), int(f[12])
    prev[tid] = (comm, ut+st)
time.sleep(2.0)
rows = []
for tid in tids:
    try:
        s = open(f"/proc/{pid}/task/{tid}/stat").read()
    except OSError:
        continue
    r = s.rindex(")")
    f = s[r+2:].split()
    tot = int(f[11]) + int(f[12])
    comm, was = prev[tid]
    rows.append((tot-was, tid, comm, f[0]))
rows.sort(reverse=True)
for d, tid, comm, st in rows:
    if d or True:
        print(f"{tid:7d} +{d:5d} ticks  {st:1s}  {comm}")
