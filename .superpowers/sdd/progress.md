# Erase/Parity Cancel Barrier Progress

Plan: docs/superpowers/plans/2026-07-14-q3n-erase-parity-cancel-barrier.md

Task 1: complete (commits 93d464f..7e084c9, review clean)
Task 2: complete (commits 7e084c9..1adcabf, review clean)
Task 3: complete (commits 1adcabf..2494fc9, review clean)
Task 4: complete (commits 2494fc9..088fad2, review clean after 088fad2)
Task 5: complete (commits 088fad2..d263fdf, review clean)

Task 10: complete (commits 24bdb6a..053593d, review clean)

# Q3N v2 LDPC/OOB/RAID Progress

Plan: docs/superpowers/plans/2026-07-19-q3n-v2-ldpc-mtd-raid-implementation.md

Baseline: 46743dc
Task 1: complete (commits 46743dc..2d8df51, review clean)
Task 2: complete (commits 2d8df51..565d7f3, review clean after overlay-size correction)
Minor review note: task-2-report retains superseded 2240-byte narrative before its appended correction; product code/docs are correct.
Task 3: complete (commits 565d7f3..709fbf0, review clean)
Minor review note: controller helper harness does not cover live MMIO/media integration or a CRC32C golden vector; cover through later guest integration tests.
Task 4: complete (commits 709fbf0..f9f374c, review clean after replay/endian/alignment fixes)
Task 5: complete (commits f9f374c..a20abef, review clean after tombstone/frontier/tail-contract fixes)

# Q3N Program-Order Boundary Revision

Plan: docs/superpowers/plans/2026-07-24-q3n-remove-program-frontier.md

Baseline: b82eaf1
Task 1: complete (commits b82eaf1..6b4ca0b, review clean)
Minor review note: qemu/README.md still describes removed frontier/order ABI and is assigned to Task 4.
Task 2: complete (commits 6b4ca0b..915c71d, review clean after BBM-only status fix)
Minor review note: Task 2 GREEN report overstates KUnit case-name visibility; kernel build and no-failure guest evidence are valid.
Task 3: complete (commits 915c71d..239e230, review clean after queue-failure accounting fix)
Minor review note: later-stripe KUnit proves independent parity queueing but does not assert successful PROTECTED completion; final review should decide whether to extend coverage.

Program-order boundary revision
Task 1: complete
Task 2: complete
Task 3: complete
Task 4: complete
Verification: host smoke, QEMU build, kernel build, rootfs build, serial guest,
raw-media persistence guest
