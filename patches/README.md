# WS63 patch-series (per QEMU version)

The WS63 model is an out-of-tree overlay on upstream QEMU. The files that are
**new** to the tree (`hw/riscv/ws63.c`, `target/riscv/insn_trans/trans_xlinx.c.inc`,
`tests/qtest/ws63-test.c`) live under `src/` and are copied in by
`scripts/build.sh`. The **edits to existing QEMU files** are carried here as a
`git format-patch` series.

Those edits drift between QEMU releases (headers move, struct/field offsets
shift, idioms change), so the series is **maintained per version**:

```
patches/
  v10.0.0/   <- current default baseline (scripts/build.sh QEMU_TAG default)
  v10.2.3/   <- newest 10.2.x, ported
  v9.2.4/    <- previous baseline, still maintained
```

`scripts/build.sh` applies `patches/$QEMU_TAG/[0-9]*.patch` in order. A QEMU
version is **supported iff `patches/<tag>/` exists**; otherwise build.sh fails
with the list of supported versions. The `qtest-matrix.yml` workflow runs the
register-level qtest on every supported version (required) plus the newest stable
release (experimental radar — a red cell means "port the series to that version").

## Series layout

Each version dir holds the same logical series:

| Patch | Touches | What |
|-------|---------|------|
| `0001-target-riscv-*` | `target/riscv/{cpu-qom.h,cpu.c,cpu.h,cpu_helper.c,translate.c}` (+ `internals.h` on 10.2) | `-cpu ws63`, custom local interrupts (LOCI* CSRs, IRQ ≥32 delivery), xlinx decode hooks, mask-ROM call interception |
| `0002-hw-riscv-*` | `hw/riscv/{meson.build,Kconfig,trace-events}` | register the machine |
| `0003-tests-qtest-*` | `tests/qtest/meson.build` | register `ws63-test` |
| `0004-*` (version-specific) | the copied `ws63.c` | adapts it to a non-default QEMU API. `v9.2.4/0004`: `system/`→`sysemu/` headers + non-`const` `Property[]` with `DEFINE_PROP_END_OF_LIST`. `v10.2.3/0004`: `exec/`→`system/address-spaces.h` + `CharBackend`→`CharFrontend` |

Note how the same support drifts across releases: 10.0→10.2 alone moved `insn_len`
to `internals.h`, made the CPU definition declarative (`DEFINE_RISCV_CPU`),
table-driverised `decode_opc`, and renamed `CharBackend`→`CharFrontend` — which is
exactly why the series is kept per-version.

`src/` tracks the **default (newest) baseline**; older version dirs carry a small
compat patch (`0004`) for the API differences in the copied sources.

## Porting to a new QEMU release

1. `QEMU_TAG=<new> QEMU_DIR=/tmp/q bash scripts/build.sh` — fails "not ported".
2. Clone the tag, copy `src/` files, apply the nearest version's series with
   `git apply --reject` (or GNU `patch --fuzz`), resolve rejects, fix any API
   drift until it builds and `scripts/qtest.sh` passes.
3. Commit the edits in the same 0001/0002/0003 grouping, `git format-patch`, and
   drop the result into `patches/<new>/` (plus a `0004` compat patch if the copied
   `ws63.c` needs adapting).
4. Add the version to `qtest-matrix.yml`; bump the `QEMU_TAG` defaults to make it
   the baseline if desired.
