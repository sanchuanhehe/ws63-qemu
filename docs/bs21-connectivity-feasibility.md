# BS21 / WS63 connectivity (BLE / SLE) emulation feasibility

Measured + reconnaissance verdict on whether the BLE 5.4 / SLE (NearLink)
connectivity of the BS2X family (and WS63) can be simulated in QEMU. Short
answer: **not by emulating the radio**, and **not cheaply at HCI either** — both
sides of every useful boundary are closed binary blobs.

This doc records the conclusion so the dead end is not re-explored. The
empirical probe code lives on the `sle-radio-probe` branch (a B_CTL trace+absorb
model in `src/hw/riscv/bs21.c`, gated on `BS21_BCTL_TRACE=1`); it is deliberately
**not** in `master` (see "Why no register absorber in master" below).

## Architecture (both BS21 and WS63)

Single core. The BLE/SLE **host** and **controller** both run as LiteOS tasks on
the app core — there is no separate BT core (the `SLAVE_CPU_BT` enum is dead;
`core:2` in a reboot dump is APPS_CORE). The stack splits at two boundaries:

```
  app (C source)
    │  high-level BLE/SLE API
  host stack blob   libbth_gle.a / libbg_common.a   (GAP/GATT/ATT/L2CAP/SMP, SLE, HCI encode)
    │  api_h2c_write()  ↓        ↑  event callbacks      ← the "HCI boundary" (in-memory, not UART)
  controller blob   libbgtp.a    (Link Layer FSM, HCI controller side)
    │  56 write-only PHY regs + IRQ 26 events             ← the "radio boundary" (B_CTL MMIO)
  B_CTL radio MMIO @0x59000000  +  closed analog PHY
```

## Boundary 1 — radio MMIO (B_CTL @0x59000000): measured, dead end

Probed by tracing every B_CTL access while running the un-trimmed (BT-present)
vendor firmware on `-M bs21`:

- **B_CTL low regs `0x59002008..0x5900388c`: 56 writes, 0 reads.** A flat,
  write-only PHY/RF constant table from one code site — no register-level poll or
  handshake. Trivially absorbed (~30 lines), but the values are opaque RF tuning
  constants meaningless without the analog model.
- **BT_SUB / BT_EM `0x59400000..0x59417fff`: ~16.5k accesses = RAM** (descriptors,
  buffers), not registers. RAM-backing it removes the near-NULL-deref crash that
  trace-absorb causes.
- After PHY init + exchange-mem setup, the controller **reboots core:2 (APPS_CORE),
  cause 0x2045**, waiting for a **PHY RX/TX event on IRQ 26 (`BCPU_INT0_ID`)** that
  no analog model emits.

**Verdict:** the registers are shallow; the wall is the radio *event/timing*
model, which is open-ended reverse engineering of the closed controller blob with
no register-readback breadcrumbs. You can make the controller *start* but never
*work*. This is where every radio-MMIO emulation attempt stops.

## Boundary 2 — HCI (host ↔ controller): exists, but blob-on-blob

The host hands commands to the controller through an in-memory HCI interface
(`api_h2c_write()` exported by `libbgtp.a`; controller→host via event callbacks),
*not* over a UART. For BLE the packets are presumably standard Bluetooth-SIG HCI
(host objects `gle_hci_cmd`, `gle_hci_ev`). SLE rides the *same* `api_h2c_write`
boundary with HiSilicon-private command/event codes (GLE = unified BLE+SLE host).

Critically: **both the host (`libbth_gle.a`) and the controller (`libbgtp.a`) are
closed `.a` archives with no `.c` source.** There is **no DTM / HCI-over-UART
controller firmware** in this SDK, so BlueZ / standard H4 tooling cannot attach.

What this leaves:

- **Theoretically feasible for BLE:** replace *one* blob with a synthetic that
  satisfies the other's ABI — provide your own `api_h2c_write` + event callback,
  link the **host** blob against a synthetic controller. You do not need the
  host's source, only its linker symbols and a valid message stream.
- **Substantial, not cheap:** you must (1) reverse the exact packet ABI at
  `api_h2c_write`, and (2) satisfy the host blob's *other* dependencies (LiteOS,
  timers, the controller's remaining exported symbols) — a large surface.
- **SLE: effectively infeasible.** Proprietary command/event set, no public
  reference, fused into the GLE blob.

### Probe result (the "try it"): HCI is unreachable at runtime, and not raw H4

Two concrete findings from actually attempting the `api_h2c_write` trace:

1. **The HCI boundary is downstream of the radio wall in boot order.** Located
   `api_h2c_write` in the BT-present firmware by prologue signature
   (`0x901357ca`; the trimmed build's `0x9012bfe6` matches `application.map`).
   Running the un-trimmed firmware on `-M bs21` with the B_CTL probe model, that
   PC is **never executed (0 hits in the instruction trace)** — the `bt`
   *controller* task brings up the radio and crashes/reboots (core:2, cause
   0x2045) **before the host ever sends an HCI command**. So you cannot observe
   the HCI ABI by running the firmware without *first* solving the radio-event
   problem (the IRQ-26 PHY wall). The HCI boundary sits behind the dead end.

2. **The boundary is a HiSilicon-framed message ABI, not raw H4 HCI.** Static
   disassembly of the host caller (`gle_hci_send_data.c.obj` →
   `gle_hci_command_encode_send_tl`) shows `api_h2c_write(a0,a1,a2,a3)` is not
   "pointer + length to a standard HCI packet": `a0` carries a message *type* in
   its high byte (`a0>>16`, e.g. `0xc`), `a1`/`a2` carry a code/length, and `a3`
   points to a small payload assembled on the stack. So the "bridge the host blob
   to BlueZ/NimBLE over standard H4" shortcut does **not** apply — the framing is
   private and would itself have to be reverse-engineered.

Net: HCI-boundary virtualization is gated behind the *same* radio wall at runtime
**and** uses a non-H4 private ABI, so neither runtime observation nor
standard-tool bridging works without reverse-engineering the closed blobs.

## Recommendation

1. **Do not pursue radio-MMIO emulation** for functional connectivity — confirmed
   dead end (Boundary 1). Same conclusion for WS63 (identical architecture).
2. **The HCI-boundary probe was run and is refuted as a cheap path** (see "Probe
   result" above): `api_h2c_write` is unreachable at runtime (radio wall first)
   and uses a private, non-H4 framing. So an HCI-boundary effort would require
   building a synthetic controller against the host blob's ABI from *static*
   reverse engineering only — substantial, BLE-only, no runtime breadcrumbs.
3. **SLE stays out of scope** — proprietary with no reference implementation.

**Bottom line:** every boundary that could be virtualized (radio MMIO, HCI) is
either behind closed-blob analog/event timing or behind a private blob-to-blob
ABI that is itself gated behind the radio wall. Connectivity emulation is not a
viable next step; the functional-peripheral work is where the leverage is.

## Reproduce the radio probe

```
git checkout sle-radio-probe
# build, then:
BS21_BCTL_TRACE=1 qemu-system-riscv32 -M bs21 -nographic -serial mon:stdio \
  -device loader,file=<flashboot-code>,addr=0x40000 \
  <flash1 64K chunks at 0x90100000..> \
  -device loader,addr=0x40000,cpu-num=0  2>bctl_trace.log
```
(flashboot code = tail `code_size` bytes of the signed flashboot image; flash1 =
the pre-trim BT-present build.)

## Why no register absorber in master

The B_CTL window is left to fault in the `bs21`/`bs22`/`bs20` machines on
purpose. No supported firmware touches `0x59000000` (the functional examples are
BT-trimmed; the Rust drivers never reach the radio), so an absorber would have
zero functional consumer, and a silent return-0 would let buggy firmware limp
instead of failing loud. A clean fault at the recognizable radio base is the
better signal. The trace+absorb model stays on `sle-radio-probe` for anyone who
wants to re-run the probe.
