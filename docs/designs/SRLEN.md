# SRLEN: Selective Run-Length Entropy Encoding

✅ SRLEN with per symbol run-bit table (SRLEN)
  - 1-bit symbols:
    - read 1-bit, index into rb table (2 entries)
    - rb table entry:
      - 1-bit run value (1 or 2)
  - 2-bit symbols:
    - read 2-bits, index into rb table (4 entries)
    - rb table entry:
      - 2-bit run value (1-4)
  - 4-bit symbols:
    - read 4-bits, index into rb table (16 entries)
    - rb table entry:
      - 4-bit run value (1-16)
  - 8-bit symbols:
    - read 8-bits, index into rb table (256 entries)
    - rb table entry:
      - 8-bit run value (1-256)
✅ SRLEN encoder specification:
  ✅ A symbol occupies N bits (N ∈ {1, 2, 3, 4, 5, 6, 7, 8, ...})
  ✅ Runs are bucketed using R bits {0, 1, 2, 3, 4, 5}
  ✅ For each symbol, we evaluate all run‑bit options:  rb ∈ {0, 1, 2, 3, 4, 5}
  ✅ rb = 0 means RAW (no run encoding)
  ✅ For each symbol, we pick the rb that minimizes encoded size
  ✅ Encoding uses per‑symbol rb
  ✅ This guarantees no expansion per symbol
  ✅ Write unittests for SRLEN that encodes known streams and then decodes and compares
