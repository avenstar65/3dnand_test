# Task 10 report — Q3N multi-plane guest smoke

Base: `29eff25`.

## Host behavior and RED/GREEN evidence

- `tests/test_scripts.sh` first failed as intended on the absent
  `--nand-mode` interface; after implementation it runs a fake QEMU and
  verifies identity, Page RAID, multi-plane, explicit-image, config-guard,
  and scoped `--fresh-nand` behavior.  The config mismatch rejects before the
  fake QEMU is called.
- The same test first failed on the absent guest command resolver, then on the
  absent host wrapper, and then on the absent lower-level media verifier.
  Its final run passes.  It sources and executes the resolved guest function;
  it does not grep implementation markers.
- The wrapper test supplies guest metadata to a fake verifier and proves a
  verifier nonzero exit (digest mismatch) prevents the final pass marker.
- Review round 1 added RED/GREEN behavior coverage for duplicate or malformed
  guest metadata, missing kernel powerdown, inconsistent topology, unexpected
  pre-mark digest, and a verifier that mutates its base image.  The wrapper
  accepts exactly one anchored guest-complete record and one success-stage
  record, requires the actual kernel `reboot: Power down` record, and compares
  base-image inode/size/mtime/allocated-block fingerprints around verification.
- A guest fixture first proved that an unexpected pre-mark digest was accepted,
  then that full OOB pattern I/O was absent.  It now executes the guest command
  boundary and verifies full-4096-byte PLACE/RAW pattern operations and both
  normal and RAW marked-block MEMREAD rejection paths.
- `sh scripts/smoke-test.sh` passed after the host changes.

## Implemented acceptance path

`scripts/run-qemu.sh` now defaults to `identity`, supports
`identity|page-raid|multiplane`, selects the dedicated
`q3n-nand-multiplane.raw` only for multi-plane, canonicalizes the resolved
image path, validates the matching built Kconfig symbol before QEMU, and
resets only that resolved file.

The guest checks exact `65536/4096/4092/104857600/43620761600` geometry,
first/page-1599/page-1600/last-page I/O, PLACE and RAW main/OOB, raw BBM
folding at four offsets, markbad, the expected post-mark NAND Core raw-read
failure, and BBT visibility after module reload.  It deliberately emits a
separate `q3n multi-plane guest complete ... main_digest=...` record; that
digest proves the first-page program completed before the markbad operation.
The host then starts built QEMU in qtest mode against the same image, issues a
raw multi-plane MMIO read for the recorded logical group, reconstructs all
four 16 KiB slices, and verifies the known SHA-256 for 64 KiB of erased
`0xff` bytes before printing the single final marker.

PLACE and RAW use the acceptance helper's `page-pattern-*` operations: every
one of the 4096 OOB bytes is compared against a deterministic pattern, while
the four BBM offsets `0/1024/2048/3072` stay `ff` in their good blocks.  The
markbad test confirms both PLACE (normal `MEMREAD`) and RAW `MEMREAD` reject
the block after `MEMSETBADBLOCK`.

## Real guest evidence and NAND Core markbad semantics

Fresh multi-plane guest execution reached:

```text
q3n multi-plane geometry writesize=65536 oobsize=4096 oobavail=4092 erasesize=104857600 size=43620761600 blocks=416
q3n multi-plane stage: first and boundary pages
q3n multi-plane stage: folded BBM
q3n multi-plane stage: NAND Core markbad erase
q3n multi-plane stage: reload NAND Core BBT
q3n multi-plane guest complete logical_block=3 die=1 block_in_plane=1 page=0 main_digest=f790d342cca81bc826050f0b6ce23ce7b4c06c7f174ce97c499653e4202fd450
MTD smoke 测试通过，关闭虚拟机
```

The subsequent real QEMU MMIO raw read completed all four planes and returned
the erased 64 KiB logical main digest:

```text
71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063
```

This is the expected generic NAND Core behavior: `nand_block_markbad_lowlevel`
calls `nand_erase_nand()` before it writes the BBM and updates the BBT.  Thus
the original requirement that `MEMSETBADBLOCK` preserve the already-programmed
first-page main data was incorrect.  The guest's pre-mark digest is retained
as evidence that it wrote the data; after marking, normal and raw MTD reads
are rejected for the bad block while the independent QEMU MMIO verifier
confirms the physical four-plane group was erased.  A read-only parse of the
same versioned media header and the four mapped physical page-0 slots (989,
1236, 1483, 1730) independently produced the same all-`ff` digest.

No driver or QEMU production code was changed.  The final fresh run completed
guest validation, normal poweroff, lower-level erased-media verification, and
printed `q3n multi-plane smoke passed` only after all of those checks passed.
Its final host lines were:

```text
q3n multi-plane media erase verified logical_block=3 main_digest=71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063
q3n multi-plane smoke passed
```

The qtest verifier launches QEMU with `-snapshot`, because this device
requires a writable block node even for the read command.  It records the
same base-image fingerprint before and after qtest; the wrapper repeats that
comparison and rejects any mutation.  The snapshot overlay lets the actual
qtest read proceed without persisting a base-image write.

An additional direct verifier run preserved the multi-plane base fingerprint
`82866920:116549226496:1785642841:2304768` (inode:size:mtime:allocated
512-byte blocks) exactly before and after.  The legacy image still has its
recorded `82254988:116549226496:1785342127:345728` fingerprint.

## Artifacts and preservation

The multi-plane kernel, rootfs, and QEMU artifacts were freshly configured or
built.  The first successful guest phase took about 5.5 seconds including the
host qtest comparison; host smoke completed in about 17 seconds.

Before Task 10, the legacy image was recorded as inode `82254988`, logical
size `116549226496`, mtime `1785342127`, and `345728` allocated 512-byte
blocks.  After all fresh multi-plane runs those same stat values remain.  It
was not reset, opened as the selected image, or hashed (a full hash would be
an avoidable read of its 116.5 GB sparse logical extent).  The separate
`work/media/q3n-nand-multiplane.raw` exists and is the only image targeted by
`--fresh-nand` in this task.
