import os, sys, time, ctypes

pid = int(sys.argv[1])
tid = int(sys.argv[2])
count = int(sys.argv[3]) if len(sys.argv) > 3 else 50
iv = float(sys.argv[4]) if len(sys.argv) > 4 else 0.02

libc = ctypes.CDLL("libc.so.6", use_errno=True)
libc.syscall.restype = ctypes.c_long

for i in range(count):
    libc.syscall(234, ctypes.c_long(pid), ctypes.c_long(tid), ctypes.c_long(23))
    time.sleep(iv)
