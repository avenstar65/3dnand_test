# Q3N Linux compatibility patches

This directory contains ordered patches for the supported upstream Linux
version. The overlay scripts apply them before installing the Q3N driver
sources.

Run:

```sh
./scripts/apply-linux-patches.sh /path/to/linux-7.0.12
```

The operation is strict and idempotent: a patch is applied only when its
forward check succeeds, accepted as complete only when its reverse check
succeeds, and rejected for any partial or version-mismatched state.
