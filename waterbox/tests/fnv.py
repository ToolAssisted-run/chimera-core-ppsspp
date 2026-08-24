#!/usr/bin/env python3
# FNV-1a of a file, in the same form run-native prints for each memory domain.
import sys

h = 1469598103934665603
for b in open(sys.argv[1], "rb").read():
    h ^= b
    h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
print("%016x" % h)
