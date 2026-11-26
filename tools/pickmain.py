import os, sys, time

pid = int(sys.argv[1])

def read(tid):
    s = open(f"/proc/{pid}/task/{tid}/stat").read()
    r = s.rindex(")")
    comm = s[s.index("(")+1:r]
    f = s[r+2:].split()
    return comm, int(f[11]) + int(f[12])

tids = sorted(int(t) for t in os.listdir(f"/proc/{pid}/task"))
prev = {}
for t in tids:
    try: prev[t] = read(t)
    except OSError: pass
time.sleep(1.0)
best = None
for t in tids:
    try: comm, now = read(t)
    except OSError: continue
    delta = now - prev.get(t, (comm, now))[1]
    if "MINECRAFT MAIN" in comm:
        print(f"MAIN tid={t} comm={comm!r} delta={delta}", file=sys.stderr)
        if best is None or delta > best[1]: best = (t, delta)
if best: print(best[0])
