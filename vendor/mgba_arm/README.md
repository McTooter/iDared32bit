# mgba_arm

The ARM interpreter core from the [mGBA](https://github.com/mgba-emu/mgba)
Game Boy Advance emulator (`src/arm/` and `include/mgba/internal/arm/`),
copyright © 2013–2024 Jeffrey Pfau.

The files as first imported are identical to those in upstream mGBA commit
[`3a5bc24`](https://github.com/mgba-emu/mgba/commit/3a5bc24629867576b0fb576a5d5a21d3b3d6b576),
so that commit is the base to compare this copy against.

This copy is maintained as part of iDared 32bit. The mGBA project is not
affiliated with iDared 32bit and does not endorse it, so please report problems
with this copy to iDared 32bit, not to mGBA.

It is vendored here, heavily extended, as touchHLE's CPU core, wrapped by
`src/cpu/mgba_wrapper/`. mGBA is under the **Mozilla Public License, v.
2.0** — the same license as touchHLE's own code — so a binary built with it is
MPL-2.0 as a whole, with no GPL component compiled in.

## License

MPL-2.0. See the license headers in the individual source files, and
`LICENSE`.

## Modifications from upstream mGBA

Only the ARM core is taken; nothing else from mGBA is present. The core
proved self-contained: after compiling `src/arm.c`, `src/isa-arm.c` and
`src/isa-thumb.c` together, the only unresolved symbols are `memset` and
`__stack_chk_fail`.

- `include/mgba-util/common.h` is **not** mGBA's. The real one is ~350 lines
  of platform shims for Windows, MSVC and assorted consoles, none of which
  the ARM core needs, so this is a minimal stand-in providing only what
  `src/arm/` actually uses: `CXX_GUARD_*`, `UNUSED`, `containerof`, the
  `ATTRIBUTE_*` and `LIKELY`/`UNLIKELY` hints, `ROR`, and the `LOAD_*LE` /
  `STORE_*LE` prefetch macros. The endian macros assume a little-endian host,
  which every platform touchHLE targets is; mGBA's versions additionally
  handle big-endian and alignment-restricted hosts.
- `include/mgba/core/cpu.h` is vendored verbatim (40 lines).
- `src/isa-thumb2.c` is not mGBA's at all: it holds the Thumb-2 32-bit
  instructions (see "Thumb-2" below).
- ARMv5 and ARMv6 instructions have been added (see below), which touches
  `src/isa-arm.c`, `src/isa-thumb.c`, `src/arm.c`,
  `include/mgba/internal/arm/isa-arm.h`,
  `include/mgba/internal/arm/emitter-thumb.h`,
  `include/mgba/internal/arm/emitter-arm.h` and
  `include/mgba/internal/arm/arm.h`.
- **Unaligned accesses follow ARMv6, not ARMv4T.** Word and halfword loads
  and stores go to the address as given. ARMv4T rotated an unaligned word
  read and sign-extended a *byte* for an `LDRSH` from an odd address, and
  mGBA implements both (the rotate in its GBA memory map, the `LDRSH` case
  in `isa-arm.c`); ARMv6 does a true unaligned access whenever CP15 c1's U
  bit is set, which is how Darwin configures it. So `LDRSH` here always
  sign-extends 16 bits, and the wrapper's callbacks pass the address
  through unmasked. Compilers emit unaligned accesses for packed structs
  and inlined `memcpy`, so getting this wrong corrupts data silently
  rather than failing.

## ARMv5 additions

The ARMv5TE delta is complete: `BLX` (register and immediate), `CLZ`, `PLD`,
`LDRD`/`STRD`, the saturating arithmetic (`QADD`, `QSUB`, `QDADD`, `QDSUB`)
and the DSP multiplies (`SMULxy`, `SMLAxy`, `SMULWy`, `SMLAWy`, `SMLALxy`).
`BKPT` was already present. All of these fill previously-`ILL` slots in the
emitter table; `BLX_R` and `CLZ` go at `-12---3-` and `-16---1-`, and the
rest are laid out as described below.

Thumb gets `BLX` in both forms: `BLX Rm`, and the `BLX` suffix of a
`BL`/`BLX` pair, which goes to ARM state and word-aligns the target. (Both
halves of the pair are now decoded together, as the one 32-bit instruction
Thumb-2 makes them; see below.)

ARMv5T also made loads to the PC interwork: bit 0 of the loaded value
selects Thumb or ARM state, as for `BX`. That covers `LDR pc`, `LDM` with
the PC (except `LDM^`, an exception return, which takes its state from the
SPSR), and Thumb's `POP {pc}` -- which is how an ARM function returns to a
Thumb caller and vice versa. The shared helper is `ARMInterworkWritePC`.
ARMv7 extends the same treatment to ARM-state data-processing instructions
that write the PC, such as `MOV pc, Rm`; that is done here too, since ARMv6
leaves the case unpredictable and the guests ran on ARMv7 hardware from the
iPhone 3GS on. Storing the PC with `STR` stores the instruction's address
plus 8, which is ARMv7's rule and what `STM` already did, rather than
ARM7TDMI's plus 12.

`BLX` immediate needed more than a table entry: it lives in the `cond == 0xF`
encoding space, which ARMv4T read as "never execute", so `ARMStep` rejected it
before dispatch. `ARMStep` now routes that space to `ARMStepUnconditional`,
which handles `BLX` and `PLD` and reports anything else as illegal.

`LDRD`/`STRD` use mGBA's existing `DEFINE_LOAD_STORE_MODE_3_INSTRUCTION_ARM`,
which generates all twelve addressing variants, and occupy the `D` and `F`
slots of the bit-20-clear ALU rows plus the eight individually-enumerated
slots of rows `-10-` through `-16-`. The doubleword access is done as two
`load32`/`store32` calls; the second is skipped when `Rd + 1` would be `PC`.

The saturating and DSP-multiply encodings all live in slots 5 and 8/A/C/E of
rows `-10-` through `-16-`. Rather than a function per slot, each DSP
multiply reads its half-select bits (opcode bits 5 and 6) at run time, so one
emitter serves all four slots of a row. Note that these encodings put `Rd` at
bits 19:16, not 15:12. The `Q` flag is CPSR bit 27, which nothing else in
this core touches.

## ARMv6 additions

The ARMv6 delta is complete apart from VFP. In ARM state:

- The 36 parallel (SIMD) additions and subtractions: the signed, signed
  saturating, signed halving, unsigned, unsigned saturating and unsigned
  halving forms of `ADD16`, `ASX`, `SAX`, `SUB16`, `ADD8` and `SUB8`.
- `REV`, `REV16`, `REVSH` and `SEL`.
- `SXTB`/`SXTH`/`SXTB16`/`UXTB`/`UXTH`/`UXTB16` and their `SXTA*`/`UXTA*`
  accumulate forms.
- `PKHBT`/`PKHTB`, `SSAT`/`SSAT16`/`USAT`/`USAT16`.
- `SMLAD`/`SMUAD`/`SMLSD`/`SMUSD`, `SMLALD`/`SMLSLD`,
  `SMMLA`/`SMMUL`/`SMMLS` (and their `X` and rounding variants),
  `USAD8`/`USADA8`, `UMAAL`.
- `LDREX`/`STREX` and the ARMv6K byte, halfword and doubleword variants,
  plus `CLREX`.
- `SETEND`, `CPS` and `BXJ`.

All of it fills slots that were `ILL`. The bulk lands in the media space
(bits 27:25 == `011` with bit 4 set), which is the odd half of mGBA's
load/store-register rows, so `DECLARE_ARM_LOAD_STORE_BLOCK` now takes those
eight slots as parameters -- named `EX1` through `EXF` after the slot each
one occupies -- with a wrapper supplying `ILL` for the rows nothing was
added to. That keeps the table the same shape as upstream's.

`SETEND`, `CPS` and `CLREX` are in the `cond == 0xF` space and so are
handled by `ARMStepUnconditional` rather than the table. `CPS` is a no-op:
touchHLE runs guest code as an application and models neither the interrupt
masks nor privilege changes. `SETEND BE` is reported as illegal rather than
ignored, since both mGBA's prefetch and touchHLE's memory assume a
little-endian guest and would silently mis-execute. `BXJ` is wired straight
to `BX`, which is what it means with no Jazelle implementation. The ARMv6K
hints (`NOP`, `YIELD`, `WFE`, `WFI`, `SEV`) needed no work: they decode as
`MSR` with an empty field mask, which already writes nothing.

In Thumb state: `REV`, `REV16`, `REVSH`, `SXTB`, `SXTH`, `UXTB`, `UXTH`,
`SETEND` and `CPS`. That is the whole Thumb delta — everything else ARMv6
added is ARM-only, and the Thumb forms of the extends, saturation, packing
and SIMD are 32-bit Thumb-2 encodings (see "Thumb-2" below). `CPY` needed
nothing: it is the ARMv6 mnemonic for the `MOV(3)` encoding that was
already there. `SETEND` and `CPS` share a table slot (`0xB640`..`0xB67F`) and are picked
apart inside the one emitter.

Two pieces of state ARMv4T has no use for came with this:

- **The GE flags**, CPSR bits 19:16, written by the non-saturating,
  non-halving parallel instructions and read by `SEL`. Like `Q`, they have
  no named field in `union PSR` -- both fall inside its 20-bit `unused` run
  -- so they are reached through `packed`. `MSR` previously ignored the
  status field byte entirely and treated the flags field as NZCV alone; it
  now writes GE and `Q` as well.
- **An exclusive monitor** for `LDREX`/`STREX`, as two new `ARMCore` fields
  cleared by `ARMReset`. touchHLE's guest threads are scheduled onto a
  single emulated core, so nothing can steal the monitor between a load and
  its store; tracking the address is only enough to reject unpaired uses,
  and deliberately cannot livelock a correctly written retry loop.

## ARMv7 additions

The ARM-state part of ARMv7 (by way of ARMv6T2) is done:

- `MOVW` and `MOVT`. These take rows `-30-` and `-34-`, the `TST` and `CMP`
  immediate encodings with the S bit clear. ARMv6 left those undefined, but
  upstream decoded them as `TST`/`CMP`, so they are the only slots here that
  change meaning rather than going from `ILL` to something.
- `MLS`, in slot 9 of row `-06-`.
- `BFC`/`BFI`, `SBFX` and `UBFX`, in the media slots of rows `-7A-` through
  `-7F-`. Their fields straddle bit 7, so each occupies two slots per row.
  The permanently undefined `UDF` (slot F of row `-7F-`) stays `ILL`, since
  compilers emit it as a trap.
- `RBIT`, in slot 3 of row `-6F-`.
- `SDIV` and `UDIV` (slot 1 of rows `-71-` and `-73-`). Strictly these are
  ARMv7s -- the iPhone 5's A6 has them and the Cortex-A8 does not -- and
  so, with VFPv4 and NEONv2 (see below), the core runs armv7s code too.
- In the unconditional space: `DMB`, `DSB` and `ISB` as instructions, where
  ARMv6 used CP15 operations, plus the `PLI` and `PLDW` hints. The barriers
  are no-ops except `ISB`, which refetches the prefetch the way `MSR` does,
  so that code the guest has just written runs.

`LDRHT`, `STRHT`, `LDRSBT` and `LDRSHT` needed nothing: in user mode they are
plain post-indexed accesses, and the rows they fall in already decode them
that way. The interworking changes that ARMv7 makes are covered under
"ARMv5 additions" above.

## Thumb-2

ARMv6T2's Thumb-2 is implemented in full, apart from the unconditional
coprocessor instructions (the `MCR2` family), which have no use in user
code. The 32-bit instructions live in `src/isa-thumb2.c`. Advanced SIMD's
Thumb encodings are its ARM ones with the top byte changed, so they are
converted and passed to the same hook (see "Coprocessor support").

- **Decoding.** A first halfword of `0xE800` or above starts a 32-bit
  instruction, so those 96 entries of the Thumb table all go to one handler,
  which reads the second halfword from the prefetch and decodes the pair.
  This replaces the ARMv5 `BL`/`BLX` halves, which are just the 32-bit
  `BL`/`BLX` encodings when executed in order, and gives the same results.
- **The PC.** While a 32-bit instruction runs, `r15` reads as its address
  plus 4, which is what Thumb code expects; only if it did not branch does
  the pipeline then step over the second halfword. Anything that branches
  refills the prefetch from its target, and so does `ISB`, by branching to
  the next instruction.
- **One implementation per operation.** Where a Thumb-2 instruction is an
  ARM instruction with its fields rearranged -- the media instructions, the
  multiplies and divides, saturation and bit fields -- it is rewritten into
  the ARM encoding and run through the ARM table. Data-processing (whose
  modified immediate constants have no ARM equivalent), loads and stores,
  `LDM`/`STM`, `LDRD`/`STRD`, the exclusives, `TBB`/`TBH` and the branches
  are implemented directly. VFP in Thumb state is the ARM encoding with an
  `AL` condition, so it goes to `vfp.c` unchanged.
- **IT blocks.** The IT state is kept where the architecture keeps it, in
  CPSR bits 26:25 and 15:10, so it is saved and restored with the CPSR when
  touchHLE switches threads. `ThumbStep` takes a separate path only while it
  is nonzero: the instruction runs if the condition passes (a skipped 32-bit
  instruction still skips both halfwords), the 16-bit instructions that set
  the flags outside an IT block have them put back inside one, and the
  state then advances.
- **The 16-bit additions** are `CBZ`/`CBNZ`, `IT`, and the hints `NOP`,
  `YIELD`, `WFE`, `WFI` and `SEV`, which are all no-ops with one core and
  no interrupts.
- **Unpredictable encodings** have defined behaviour; see "UNPREDICTABLE
  encodings" below.

## Fixes found by differential testing

The ARMv7 work was checked by differential testing: an independent emulator,
QEMU's Cortex-A15 run through the Unicorn library, served as a black-box
reference, and no code was taken from it. The same random instructions, with
the same registers, flags and memory, were run through both and the results
compared: some 150,000 Thumb-2, 16-bit Thumb, IT-block and ARM cases at first,
and now no differences at all (see "UNPREDICTABLE encodings" below for how
that was reached). That turned up these bugs, all fixed:

- **The core must be built with `-fwrapv`**, as mGBA's own build does. Its
  ALU relies on signed arithmetic wrapping; without the flag, overflow is
  undefined behaviour, and an optimising compiler removed the V flag's
  overflow test (`NEGS` of `0x80000000` left V clear). `build.rs` passes it.
- **Setting the flags cleared Q.** Upstream clears the whole top byte of the
  CPSR before setting N, Z, C and V. On ARMv4T that is all the byte holds,
  but it also holds Q from ARMv5TE, and two bits of the IT state from
  ARMv6T2. Only the four flags are written now.
- **`MULS` and `MLAS` changed C**, to the stale shifter carry. ARMv4T left C
  meaningless after a multiply; from ARMv5 it is unchanged.
- **`SMLA<x><y>` and `SMLAW<y>` saturated**, where they should wrap and only
  set Q. (A mistake in the ARMv5 work above, not upstream's.)
- **Thumb's `LDRSH` (register)** still had ARM7TDMI's odd-address behaviour,
  loading a sign-extended byte; the ARM form had already been fixed.

## UNPREDICTABLE encodings

The architecture leaves many encodings UNPREDICTABLE: the PC or SP in most
register fields, should-be-zero or should-be-one bits with the wrong value,
overlapping registers, and so on. An implementation may do anything with
them short of breaking security, including treating them as undefined, and
real cores differ. This core gives each such case one defined behaviour,
chosen to agree with the reference used in testing, so that every difference
that testing finds is a real one:

- **Should-be-zero and should-be-one fields** with the wrong value make the
  instruction undefined. Upstream ignored them, as the ARM7TDMI does. In ARM
  state a table built at reset gives each decode row its check, applied in
  `ARMStep` once the condition has passed (`ARMStrictCheck`, inlined there
  so that it costs nothing measurable); the Thumb-2 decoder checks its fields
  as it goes.
- **Writes to the PC** by instructions that are not branches -- Thumb-state
  data processing, multiplies, the media instructions, `MOVW`/`MOVT`, load
  and store writeback, `LDRD` into the PC, VFP and Advanced SIMD transfers --
  are a branch that stays in the current state, ignoring the low bits
  (`ARMWritePCUnpredictable`). ARM-state data processing interworks, as ARMv7
  defines for the non-S forms; ADR to the PC in Thumb interworks too.
- **Exception returns** from user mode are undefined: `MOVS` and `SUBS` to
  the PC, and `LDM`/`STM` with `^`. So are `MRS` and `MSR` of the SPSR. The
  other S forms with the PC as the destination set the flags and branch.
- **`LDM` and `STM`** with an empty list or a PC base are undefined, as is
  `LDM` loading its base with writeback; in Thumb-2 so are lists of fewer
  than two registers and any list holding the written-back base. Exclusive
  loads and stores with the PC (or, in Thumb, SP), or whose status register
  is also Rt or Rn, are undefined, as are 16-bit `BX`/`BLX` with their
  should-be-zero bits set.
- **`LDRD`/`STRD`** with an odd Rt, or post-indexed with W set, are undefined;
  Rt == LR transfers the PC as the second register; and writeback comes after
  both loads.
- **The PC as a register-specified shift's operand** reads as the
  instruction plus 8, where the ARM7TDMI read plus 12.
- **Long multiplies with RdHi == RdLo** keep RdHi, with the flags from the
  64-bit result. `SMMLS` with Ra == 15 is `SMMUL`.
- **VFP and Advanced SIMD:** `VMOV Rt, Sn` with Rt == 15 sets the condition
  flags from the value, as `VMRS APSR_nzcv` does; fixed-point `VCVT` with
  more fraction bits than the size scales by the negative difference; a
  load/store register list running past D31 reads and writes storage no
  other instruction can see, except for the single-lane forms, which are
  undefined; and a permutation of a register with itself has a fixed result.
- **The unallocated memory hints** in the unconditional space are no-ops,
  as ARMv7 says.

The last runs, some 190,000 random instructions across every mode (ARM,
Thumb-2, 16-bit Thumb, IT blocks, VFP and Advanced SIMD), found no difference
in any of these. What still differs is a handful of UNDEFINED encodings that
the reference executes, where this core follows the ARM ARM: Thumb-2 `STR`,
`STRH` and `STRB` with Rn == 15, and `VQDMULL` with U set and `VMUL.F32` with
size<1> set among the Advanced SIMD encodings. (`YIELD` and `WFE` also show
up, but only because the test harness stops at them.)

A real Cortex-A8 may well ignore a should-be-zero field where this core
faults. Compilers never emit such encodings, so in practice this only
matters to hand-written or deliberately obfuscated code.

## Coprocessor support

Upstream's `struct ARMCoprocessor` offers `mrc`/`mcr`/`cdp` hooks that
receive a `CRn`/`CRm`/`opcode1`/`opcode2` decomposition, and stubs `LDC`/`STC`
out entirely with no hook at all. Neither suits VFP: its encodings scatter
the `D`, `N` and `M` register-extension bits and bit 20 across the word, and
its `CRn`/`CRm` fields are register numbers rather than opaque coprocessor
registers, so the decomposition throws away what a handler needs.

`struct ARMCoprocessor` therefore also has a `raw` hook, which takes the
whole opcode and returns whether it handled the instruction. It gets first
refusal on `CDP`/`MCR`/`MRC` before the decomposed hooks, and is the only
path for `LDC`/`STC`. `MCRR`/`MRRC` were not decoded at all -- upstream maps
their rows onto `STC`/`LDC` -- so rows `-C4-` and `-C5-` now hold one emitter
that routes them the same way.

Advanced SIMD is not a coprocessor as far as encodings go: its
data-processing instructions are `1111 001x` and its element and structure
loads and stores `1111 0100 xxx0`, in the unconditional space, which upstream
treats as undefined apart from `BLX`. `struct ARMCore` therefore has an
`advancedSimd` hook as well, which `ARMStepUnconditional` calls for those two
patterns with the opcode as it is. The Thumb-2 decoder converts its forms,
`111x 1111` and `1111 1001 xxx0`, to the ARM ones before calling the same
hook. Advanced SIMD's moves between core registers and vector lanes, and
`VDUP` from a core register, are in coprocessor 11's space, and arrive
through its `raw` hook like VFP.

Five things about this core are worth knowing before wiring it to anything,
all discovered the hard way:

- **Do not call `ARMInit`/`ARMDeinit`.** Both dereference `cpu->master`,
  mGBA's component system, which touchHLE has no use for and leaves NULL.
  `ARMReset` is all that's needed.
- **`processEvents` must advance `nextEvent`.** `_ARMSetMode` sets
  `nextEvent = cycles` on every ARM/Thumb switch to force an event check, and
  `ARMRun` spins until the handler pushes the horizon back out. A no-op
  handler hangs on the first interworking branch -- including mGBA's own `BX`.
- **`memory.activeRegion` must be a real mapping.** Instruction fetch goes
  through it unconditionally with no callback path, so a NULL region
  dereferences NULL on the first instruction.
- **`r15` is not the PC touchHLE means, and changing it needs a refill.**
  The core keeps `gprs[ARM_PC]` one instruction ahead of the one that runs
  next, because it maintains a two-entry prefetch, and every PC change has
  to be followed by `ARMWritePC`/`ThumbWritePC` to refill it. touchHLE
  expects neither property, so the wrapper keeps its own shadow register file and
  converts on the way in and out; see the comment on `struct
  touchHLE_MgbaWrapper`. Handing out the core's `gprs` directly reports a
  PC two instructions too high and silently goes on executing already
  fetched instructions.
- **`nextEvent` must be finite**, even with nothing to schedule. `cpu->cycles`
  is an `int32_t` that only `processEvents` resets, and `ARMRun` only calls it
  once the horizon is reached -- so at `INT32_MAX` a multi-cycle instruction
  starting just short of it overflows the counter first, which is undefined
  behaviour. A guest that stays in ARM mode gets there in under a minute,
  since nothing else resets it.

## Scope gap

mGBA emulates the GBA's ARM7TDMI, which is **ARMv4T**. touchHLE's guests are
**ARMv6** with VFP, so this core needed extending before touchHLE could use
it. That work is now done, and the core runs real guests.

- ARMv5 and ARMv6 integer: done in both ARM and Thumb state, see above.
- ARMv7 integer: done, in both ARM state and Thumb-2.
- Advanced SIMD (NEON): done, outside this directory; see below.
- CP15: the user-mode slice, in `src/cpu/mgba_wrapper/cp15.c`, matching
  touchHLE's earlier core -- the ARMv6 barriers (which are CP15 operations,
  there being no DMB/DSB/ISB instructions yet) and the thread ID registers.
  Any other MCR/MRC succeeds, reading zero, and is logged.
- VFP: VFPv3-D32, as on the Cortex-A8, implemented outside this directory.
  The decoder side is here (see "Coprocessor support" above); VFP itself is
  `src/cpu/mgba_wrapper/vfp.c`. VFPv3 adds to the ARM1176's VFPv2 the
  double registers D16 to D31, `VMOV` immediate, and conversions between
  floating and fixed point; VFPv2 code runs unchanged. The register file is
  64 words, exactly the `extregs` of touchHLE's thread context, and
  `MVFR0`/`MVFR1` describe what is implemented. (Before this, an
  instruction naming D16 to D31, which VFPv2 should reject, indexed past
  the 16-register array.)

  Arithmetic uses the host's `float` and `double` rather than a soft-float
  implementation, wrapped in the architecture's rules where hosts differ
  or have no equivalent:

  - NaNs: a NaN operand propagates by operand order (the first signalling
    NaN, quietened, else the first quiet NaN), and an invalid operation
    gives the positive default NaN (x86 would give a negative one). The
    default-NaN bit (DN) replaces every NaN result with the default NaN.
  - Flush-to-zero (FZ): denormal inputs are read as zero, setting `IDC`,
    and results that are tiny before rounding, as ARM detects it, are
    written as zero, setting `UFC`. iOS runs threads with FZ on (Apple's
    TN2293), so touchHLE's threads start with FPSCR 0x01000000; code built
    for ARMv6 that relies on denormals being flushed now sees that.
  - Rounding mode: the host's rounding mode is set to FPSCR's for the
    duration of an instruction when it is not round-to-nearest (the
    default, left alone so the common case costs nothing), and put back
    afterwards. `build.rs` passes `-frounding-math` so the compiler does not
    assume the default mode.
  - Short vectors: with FPSCR's LEN nonzero, the arithmetic, `VMOV`,
    `VABS`, `VNEG` and `VSQRT` work on LEN+1 registers at the given stride,
    wrapping within a bank, with a destination in the first bank making the
    operation scalar and an Rm there making it scalar-by-vector. The
    Cortex-A8 deprecated these, but they were there on the original
    iPhone's ARM1176, and the "vfpmathlibrary" of that era used them for
    matrix work.

  The cumulative exception bits other than `IOC`, `IDC` and the flush's
  `UFC` are not maintained, which would mean reading the host's exception
  state after every operation, and which no guest looks at.

  Beyond the Cortex-A8, the core has what the armv7s A6 (iPhone 5) adds:
  VFPv4's fused multiply-accumulates (`VFMA`, `VFMS`, `VFNMA`, `VFNMS`), done
  with the host's correctly rounded `fma` inside the architecture's
  `FPMulAdd` rules (NaN order addend first, a quiet NaN addend not excusing
  infinity times zero, and FZ's tininess judged before rounding), and the
  half-precision conversions (`VCVTB`, `VCVTT`), with FPSCR's rounding mode,
  its alternative half-precision format (AHP), and the Underflow flag for a
  tiny inexact half. The fused forms are undefined with LEN or STRIDE set.
  Both were checked against the reference's Cortex-A15, which has VFPv4:
  40,000 random VFP instructions with random controls, and 60,000
  half-precision conversions from values chosen around half precision's
  range, agree in every bit. With AHP a NaN becomes a signed zero, which is
  what ARMv8's pseudocode says; ARMv7's says +0. FPSID identifies
  the Cortex-A15's VFPv4, the model it was checked against, to match; but
  as on a device, only FPSCR is accessible from user mode, where touchHLE's
  guests run, and `VMRS`/`VMSR` of FPSID, MVFR0, MVFR1 or FPEXC there is
  undefined.

  Like the integer work, VFP was checked against the reference's Cortex-A15:
  some 60,000 random VFP instructions in ARM and Thumb state, starting from
  zeroes, infinities, NaNs, denormals and extremes, agree in every register
  bit (NaN payloads aside) and in FPSCR's flags. The controls were checked
  against its Cortex-A8, which has short vectors: another 45,000 instructions
  with random rounding modes, FZ, DN, LEN and STRIDE agree in every bit, NaN
  payloads included, except where the reference departs from the ARM ARM on
  short vectors. It steps a single-precision vector with STRIDE 0b11 by four
  registers rather than two, and its two-operand double-precision vector loop
  advances Vd from Vm instead of advancing Vm.
- Advanced SIMD (NEON), as on the Cortex-A8, in `src/cpu/mgba_wrapper/neon.c`:
  all of the integer, polynomial and single-precision instructions, the
  element and structure loads and stores (`VLD1`-`VLD4`, `VST1`-`VST4`, in all
  three forms), and the lane transfers; and the armv7s A6's NEONv2 additions,
  `VFMA`/`VFMS` and `VCVT` between single and half precision (which,
  unusually, follows FPSCR's AHP). The registers are VFP's, so thread switches
  already save them. Each instruction reads all of its sources before writing,
  which is what the architecture specifies for overlapping registers. Floating
  point uses VFP's primitives (`fp.h`) under the "standard FPSCR value" the
  architecture prescribes -- flush-to-zero, default NaN, round to nearest,
  whatever FPSCR says -- and sets FPSCR's cumulative flags, as the saturating
  instructions set QC. `VRECPE` and `VRSQRTE` use the architecture's estimate
  tables, so they give the Cortex-A8's exact bits. Alignment qualifiers are
  not checked, since touchHLE's memory has no alignment faults. `MVFR1` now
  advertises all of this.

  NEON was checked against the reference's Cortex-A15 and Cortex-A8: over
  270,000 random Advanced SIMD instructions in ARM and Thumb state (lane
  values chosen to hit saturation, rounding and sign boundaries, and floats as
  for VFP), and 5,000 IT blocks of them, agree in every register bit, in
  memory and in FPSCR's flags. The UNPREDICTABLE forms are as described under
  "UNPREDICTABLE encodings" above; the only differences left are two
  unallocated encodings the reference executes, `VQDMULL` with U set and
  `VMUL.F32` with size<1> set.
