# AM5728 boot notes (as the RISC OS Titanium ROM performs it)

Derived from tracing `TITANIUM.ROM` (RISC OS 5.31) under the `titanium` QEMU
machine, cross-referenced with the AM5728 TRM and RISC OS Open `HAL_Titanium`.

## ROM image format (TI GP boot)

The ROM begins with an 8-byte TI **GP header**:

| Offset | Field | Value in TITANIUM.ROM |
|---|---|---|
| 0 | image size | `0x00000800` (2 KiB first-stage) |
| 4 | dest address | `0x40300008` (OCMC_RAM1) |
| 8 | first-stage code | begins `B <reset>` |

The SoC mask ROM copies `size` bytes to `dest` and jumps there. The first-stage
(`HAL_Titanium/s/Top`, position-independent) sets up the DPLLs, DDR, pin mux and
UART, then loads the rest of the ROM from QSPI flash. We emulate this mask-ROM
step in `titanium_load_rom()`.

## Observed first-stage sequence

1. `MSR CPSR_c, #0xD3` — Secure SVC, IRQ/FIQ masked.
2. `MRC/MCR p15 … c1,c0,0` — clear `SCTLR.M/C/I` (MMU + caches off).
3. `… c1,c0,1` — `ACTLR` config. **Requires Secure state / EL3** — with
   `has_el3=false` this faulted Undefined.
4. Early `SMC` — on real silicon the AM5728 secure ROM services it. We route
   SMC through QEMU's PSCI conduit so it returns instead of trapping to an
   unset monitor vector.
5. PRCM clock-control writes (e.g. `CM_*_CLKCTRL` around `0x4A009840`), then a
   **DPLL lock poll**:

   ```
   loop: LDR  r2, [r1, #4]   ; DPLL status
         TST  r2, #1         ; lock bit
         BEQ  loop
   ```

   A plain RAM stub never sets the lock bit, so this spins — this is the
   current stopping point. A PRCM/DPLL device that reports lock (and module
   IDLEST ready) is the next requirement.

## Clock targets (from `HAL_Titanium/s/Top`)

- Per DPLL → 768 MHz (256 MHz FUNC, 192 MHz DSS, 64 MHz QSPI)
- Core DPLL → 2128 MHz
- MPU DPLL → 2000 MHz (1000 MHz core clock)
- DDR DPLL → 2128 MHz (532 MHz EMIF)

## QEMU bring-up gotchas hit

- Keep EL3 (Cortex-A15 default) — GP boot runs Secure.
- `psci-conduit = SMC` so early SMCs don't dead-end at the monitor vector.
- Back the L4 peripheral space or accesses raise external (data) aborts.
- Secondary cores: RISC OS uses a single A15, so the machine is single-core.

## CRITICAL FINDING: no UART output in the production ROM

`HAL_DebugTX` (s/Init) is wrapped in `[ Debug ... ]` conditional assembly. The
shipped TITANIUM.ROM is built with `Debug = FALSE`:
- `HAL_DebugTX` compiles to a bare `MOV pc, lr` (no-op) — the HAL never writes
  the UART.
- The debug strings ("Entered HAL_Init", "Registering devices") are **not even
  present** in the ROM (verified with grep) because the `DebugTX` macro emits
  nothing.

Implication: **there is no UART banner to reach with this ROM.** Chasing
HAL_Debug serial output is a dead end. RISC OS's visible output goes to the
screen via the **DSS display subsystem** (→ TFP410 → HDMI), not the serial port.

The realistic path to visible output is therefore: complete HAL device init +
RISC OS kernel boot, and model the **DSS/DISPC** so the framebuffer RISC OS sets
up in DRAM can be displayed. This is a large, multi-peripheral effort, not a
matter of a few more register stubs. A debug-enabled ROM build (Debug=TRUE),
if obtainable from RISC OS Open, would be the only way to get serial output.

## Blind-stubbing ceiling (no serial)

With DSS DISPC + OMAP UART + reset/clock-status models, the boot reaches ~1243
basic blocks (deep in HAL device init, ~0xFC01E000). But progress past the
clock bring-up required faking CM status reads (return "ready"), and that leads
the HAL to a translation-fault loop at an unmapped VA (0xF9BFFFF0) — i.e. faked
values send it down an invalid path. Without serial observability there is no
way to tell legitimate progress from garbage. Genuine further progress needs
either a Debug=TRUE ROM (serial) or proper device models (real PRCM/clock/CTRL
behaviour), not blind register fakes. The full RISC OS kernel boot still lies
beyond the HAL.

## Real PRCM model (replaces blind fakes)

The CM is now modelled properly (CM_CORE_AON 0x4A005000, CM_CORE 0x4A008000):
- DPLL/APLL lock: IDLEST registers are read-only; an unwritten CKGEN register
  whose preceding CLKMODE was written nonzero reports locked (bit0). Written
  config (CLKMODE/CLKSEL/DIV) is returned verbatim — fixing the earlier blanket
  OR that corrupted dividers and sent the HAL to a garbage address.
- CLKCTRL IDLEST (bits[17:16]) from MODULEMODE: DISABLE->DISABLED, ENABLE->FULL;
  AUTO->IDLE for the CM_CORE_AON accelerator domains (DSP/EVE/IPU, unused by
  RISC OS) and FULL for CM_CORE peripheral domains (USB etc., in use).
- SerDes/PHY/PLL space (0x4A084000..0x4A098000: USB3 PHY OCP2SCP, DPLLCTRL_SATA)
  reports ready for unwritten status reads.

With this the boot advances *correctly* through CM -> USB3 PHY -> SATA PLL to
~1243 blocks. Next genuine blocker: a translation-fault loop at VA 0xF9BFFFF0
(HAL accessing an unmapped virtual address) - an MMU/mapping issue to resolve
next, not a PRCM one.

## Debug ROM: serial observability achieved, 0xF9BFFFF0 confirmed as the kernel CAM

A `Debug=TRUE` RISC OS 5.31 ROM (built from the ROOL source under RPCEmu+DDE,
with HAL `Debug`, kernel `DebugROMInit` and `DebugHALTX` all forced on) boots in
this machine with `-serial stdio` and, for the first time, narrates its progress
over the OMAP UART:

```
12345TiTwentyTwo
InitARM done
RAM registered
Entered HAL_Init
PCIe link down (x2)
PCI probing
HAL initialised
IICInit
ClearWkspRAM
HAL_CleanerSpace          <- last line, then hangs
```

`-d int` shows the hang is an infinite Data Abort loop (~1.1M aborts in 6 s):

```
ESR 0x25/0x9600007f, DFSR 0x80e (2nd-level page translation fault, WRITE)
DFAR 0xf9bffff0, faulting PC 0xfc01e6b0, EL3->EL3, return to svc then re-fault
```

This independently confirms the long-suspected `0xF9BFFFF0` blocker is the
**kernel building the CAM** (`Kernel/s/HAL`), not a HAL/PRCM issue:

- After `HAL_CleanerSpace` the A15 returns "no cleaner space"; the ROM is
  uncompressed, so the kernel jumps to "Allocate the CAM" (`Init_MapInRAM`).
- The CAM is *cleared* top-down by `ConstructCAMfromPageTables`
  (`STMDB a2!,{...}`) and faults on its **first** write at `0xf9bffff0`, so
  `CAMTop = 0xf9c00000` and the CAM region was never actually page-mapped
  (the later SVC/IRQ/heap allocations succeeded, so boot got this far).
- The CAM size maths is self-consistent
  (`MaxCamEntry = pages-1`, `SoftCamMapSize = ceil(pages/256)*4096 >= pages*16`),
  so this is **not** a kernel computation bug.

Conclusion: `Init_MapInRAM` maps the wrong/no physical RAM for the CAM, which
points at the **HAL-supplied PhysRamTable / RAM-bank layout** — i.e. what
`HAL_Titanium` derives from this machine's EMIF/DDR model versus real silicon.
The fix belongs in the QEMU machine's memory-map/EMIF modelling, not the kernel.
(Register-dump probes have been added to the debug kernel at the CAM mapping
site and the fill loop to read out the exact bank/size/phys values next.)
