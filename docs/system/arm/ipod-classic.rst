iPod Classic (``ipod-classic``)
===============================

This port boots the locally supplied decrypted retailOS 2.0.4 image through
the S5L8702 boot ROM and NOR EFI loader into the normal iPod main menu.
The verified configuration has 64 MiB DRAM, 256 KiB IRAM and a writable disk
with a 512 MiB FAT32 data volume. Music, Settings and About navigation work;
About reports approximately 510 MiB free on the newly prepared volume.

This is a partial peripheral implementation. I2S0 now serializes the signed
WAV fixture with exact sample continuity, and Now Playing advances past
0:00. The codec now sends PCM through nominal gain, mute and routing controls
to a QEMU audio backend. Analog dynamics and DSP filters remain absent.
Bundled game launches fail with the supplied damaged firmware marker. An
explicit diagnostic reconstruction lets Klondike, iPod Quiz and Vortex
start; the audit describes its evidence and validation limits.
The reproduced About background failure is fixed; wider
transition coverage is incomplete. Video playback, USB/iTunes restore,
and whole-machine migration have not been
validated. Hold-switch wakeup restores the UI, and RTC alarms resume firmware
execution. Ordinary button wakeup, power sequencing and automatic saving of
settings remain incomplete; settings are read from a restored Preferences file.
Device VMState descriptions do not imply whole-machine migration support.

The detailed audit records reproduced failures, remaining device stubs,
boot workarounds, and the limits of the firmware evidence:

.. toctree::
   :maxdepth: 1

   ipod-classic-audit

Local inputs and build
----------------------

No Apple firmware, keys, disk image or Preferences file is distributed in
this repository. Use the files already present in the surrounding workspace.
The verified inputs are:

* ``bootrom_patched.bin``: the existing 64 KiB boot ROM image.
* ``new_nor_patched.bin``: the existing 1 MiB NOR image. Its SysCfg ``HwVr``
  is ``0x00130200``. A value of ``0x00130000`` from an older reference setup
  is rejected by this retailOS revision.
* ``osos.fw.decrypted``, ``rsrc.fw``, ``aupd.fw.decrypted`` and ``hash.fw``
  from the local ``iPod_35.2.0.4`` directory.
* The local ``stop.bin`` firmware partition header.
* Optionally, an existing ``iPod_Control/Device/Preferences`` file to restore
  configured settings. Without it, the firmware presents language selection
  on first boot; selecting a language enters the main menu.

The supplied ROM/NOR were already patched for a pre-decrypted boot setup.
This work does not replace them or patch retailOS instructions. The disk
builder writes a header authentication tag at osos header offset ``0x40``:
``SHA1(header[0:0x40])[0:16]``, appropriate to identity fused-key transfers.
It is not a valid authentication tag for an original encrypted device.

Limit build concurrency to avoid exhausting host memory. From the QEMU
source directory, for a fresh build::

  mkdir build
  cd build
  ../configure --target-list=arm-softmmu --disable-werror
  ninja -j2 qemu-system-arm tests/qtest/s5l8702-test

For the existing configured build::

  ninja -j2 -C build qemu-system-arm tests/qtest/s5l8702-test

Disk preparation
----------------

Install ``dosfstools`` and ``mtools``. From the surrounding workspace::

  python3 qemu-classic/scripts/ipod-classic/build-disk.py \
    --firmware-dir /path/to/iPod_35.2.0.4 \
    --stop ipod_files/stop.bin \
    --preferences ipod_files/Preferences \
    --output ipod_files/ipod_hdd_restored.img

The builder exclusively creates its output and refuses to overwrite an
existing disk. It uses sparse files and 1 MiB copy buffers, not a disk-sized
allocation. A FAT32 data volume smaller than 512 MiB is rejected because
4096-byte sectors with one sector per cluster otherwise fall below the
FAT32 cluster-count threshold. The original ``ipod_hdd.img`` is preserved.

The local Preferences used in the successful cold-boot test was captured
from the firmware's own settings object after selecting English through
the clickwheel. Exactly the writer's serialized extent (object + 4,
``0xb8c`` bytes) was restored to the data volume, then verified with a new
QEMU process. The disk builder also accepts a Preferences file saved on a
real device with this version. It does not modify a running guest's state.

Boot and controls
-----------------

Make a separate writable copy of the supplied NOR for the restored machine.
An equivalent command line, from the workspace, is::

  qemu-classic/build/qemu-system-arm \
    -M ipod-classic,bootrom=ipod_files/bootrom_patched.bin \
    -global s5l8702-aes.fused-key-bypass=on \
    -drive if=mtd,format=raw,file=ipod_files/ipod_nor_restored.bin \
    -drive if=ide,format=raw,file=ipod_files/ipod_hdd_restored.img \
    -icount shift=4

``scripts/ipod-classic/run.sh`` supplies these arguments. Its defaults use
``build/qemu-system-arm`` in the source checkout and ``ipod_files`` beside
the checkout. It accepts extra QEMU options and overrides via ``IPOD_DISK``,
``IPOD_NOR``, ``IPOD_BOOTROM``, ``IPOD_QEMU`` and ``ICOUNT``. Relative override
paths are resolved from the source checkout. Use ``-snapshot`` for a
session whose disk writes should be discarded. For a headless session,
add ``-display none`` and a QMP socket.

The codec uses QEMU's ``audiodev`` property. For a WAV capture, append::

  -audiodev wav,id=ipod-audio,path=ipod-output.wav,out.frequency=44118 \
  -global s5l8702-cs42l55.audiodev=ipod-audio

Select an available host audio driver instead of ``wav`` for local listening.
The default connector is headphones; ``-global s5l8702-cs42l55.line-out=on``
selects the separately controlled line output. The host buffer is bounded
and cannot stall guest DMA. ``host-dropped-frames`` on ``/machine/codec``
reports host staging overflow. Digital gain ramps now follow LRCK; analog
gain/mute changes wait for sampled zero crossings or a provisional timeout.
Digital mute ramps, analog dynamics, DSP filters and capture remain incomplete.

The firmware boot was verified without a debugger, direct RAM loader,
semihosting, guest PC hooks, or injected interrupts. Instruction counting
provides the virtual CPU/timer pacing used for verification.

Keyboard controls are:

* Up/Down (or Q/E): rotate the wheel.
* Enter: Select.
* Escape (or W): Menu/back.
* Left/Right (or A/D): Previous/Next.
* Space (or S): Play/Pause. Long Play enters sleep.

The physical hold switch is the boolean QOM property ``hold`` on
``/machine``. Set ``-M ipod-classic,hold=on`` to start locked, or change it
at runtime through QMP::

  {"execute":"qom-set","arguments":{"path":"/machine","property":"hold","value":true}}

Use ``false`` to unlock. The firmware shows the lock icon and blocks wheel
input while locked; unlocking can wake it from sleep. Normal wheel-button
wakeup still needs implementation. The PMU RTC supports guest time/alarm
writes and honors ``-rtc base=...`` and ``-rtc clock=vm|host``. RTC state is
retained across a system reset within the running QEMU process.

The AES device implements custom-key AES-128/192/256 CBC and ECB with the
QEMU crypto API. Fused keys cannot be recovered from these firmware files.
``fused-key-bypass=on`` explicitly enables identity DMA for the supplied
pre-decrypted setup. It defaults to off on the device. It does not decrypt
an original encrypted firmware image.

Reverse-engineering record
--------------------------

The reference repository was used as a source of hypotheses:
https://github.com/davidmonterocrespo24/qemu-ipod-classic

Critical boot behavior was checked against the local 2.0.4 firmware using
Capstone disassembly, Ghidra decompilation and QEMU/GDB register and DMA
traces. Addresses below are runtime addresses, not offsets in osos.fw.
The image has a ``0x800``-byte wrapper. Startup relocates its first
``0xaed8`` payload bytes to ``0x22000000`` and the rest to ``0x08000000``.
Using the unrelocated base gives misleading decompiler references.

Verified input SHA-256 values::

  osos.fw.decrypted
  f4368251a58b2fdc7b46acf3178dae1d24bc1e029736240741015851256c65c4
  rsrc.fw
  82a530325526bca1b24b01ffe19a5417c30f4be71ccaa9bbbab008a99f63f523
  new_nor_patched.bin
  8a51a4ea9bbc4921503cb6cba7938b61dd5df305a0377eb2d47e21cf48aa2e2f

Hardware and startup observations:

* ``0x083880ac``, ``0x080db374``, ``0x082b19e4``: the initial task reads the
  hardware identifier from the EFI SysInfo handoff and requires major
  ``0x13``, revision at least ``0x0200``. Failure selects ``softupdt``.
* MIU control at ``0x38100000`` is programmed to ``0x80d`` during startup.
  Bit 0 switches low memory from the boot ROM to IRAM. Without the switch,
  exceptions execute the wrong vector table. Two MemoryRegion aliases now
  cover the IRAM banks, and reset/post-load update the mapping.
* ``0x083600b8`` programs Timer E: CON ``0x440``, prescaler 11, DATA0
  ``0xffffffff``, CMD 3. ``0x083600a8`` reads its counter. These values
  select a 1 MHz counter with the configured 12 MHz ECLK. Runtime IRQ traces
  and qtests establish timer compare/reload and W1C behavior; E/F/G/H
  occupy TSTAT bit groups 26:24, 18:16, 10:8 and 2:0.
* ``0x2200200c`` selects clock sources and dividers; ``0x0835fe18`` programs
  HCLK/PCLK division. Clock outputs now follow these registers, replacing
  the fixed 121.5 MHz PCLK. The checked boot programs 216/108/54 MHz
  CCLK/HCLK/PCLK and later idles at 18/9/9 MHz. Timer deadlines follow
  rate changes and gating while retaining fractional counter progress.
  The timer controller uses these rates, and SPI honors clock gating.
  PLL lock is instantaneous, and CCLK does not rescale TCG execution.
  Remaining clock and event-timing assumptions are listed in the audit.
* ``0x083608dc``, ``0x08360a78``, ``0x08360cac``: I2C byte completion and
  interrupt acknowledgement are separate handshakes. START transfers an
  address; the next GO transfers the first data byte. The old extra receive
  consumed a byte too early. ``0x082da724`` consequently read the wrong
  hold-switch value. The corrected PMU transaction returns register 0x87
  bit 1 from the physical switch input; high permits wheel input.
  STOP must also release the selected slave: ``0x08360968`` writes ``0xd0``
  after TX, and ``0x083609f8`` writes ``0x90`` after RX. The controller used
  to clear its BUSY bit before testing it, leaving the previous slave
  selected. This sent the codec initialization from ``0x080a79bc`` to the
  PMU. The corrected bus selects codec address ``0x4a``; a regression test
  covers TX/RX STOP, address changes, absent slaves, and interface disable.
  Address ACK/NACK, receive data, and slave read/write effects now occur
  at byte completion. STOP, disable, and reset cancel pending operations.
  This removes immediate slave effects followed by a delayed status bit;
  the atomic byte model still uses an unverified 25 microsecond duration.
  The original updater's embedded diagnostic maps I2C0/I2C1 to PWRCON1
  bits 4/6. Both gates now suspend byte progress and snapshots preserve
  suspended transfers. Clock rate/divider timing and the alternate source
  remain unverified; the fixed duration counts only enabled time.
* ``0x0809f1c8`` configures codec master clocking and a staged power-up;
  ``0x080a7838`` selects the rate. Clock group 6 at ``0x220021a0`` and
  ``0x0835ff54`` supplies its MCLK source, dividers and gate. The codec now
  derives LRCK from this input and its control registers, and has corrected
  reset values and control-port addressing. Its LRCK output now drives
  the I2S0 TX serializer. The codec accepts those frames and supplies a
  standard QEMU audio voice. Original paired volume writes at ``0x080cf76c``
  update both headphone channels; capture and analog dynamics remain absent.
* ``0x22004a20`` programs PL080 transfers and ``0x220048fc`` updates their
  descriptors. Tracing confirms that the preceding request bypass consumed
  an audio buffer and loads a zero-count descriptor before the firmware
  fills it. Width conversion, source-unit counts, completed-descriptor
  interrupts and bus errors are corrected. PL080 now has a four-word FIFO
  per channel, single/burst request handshakes and bounded scheduling.
  I2S0 TX supplies burst request 10 and consumes DMACCLR; its transaction
  buffer drains at the codec LRCK rate. Other request sources remain absent.
  Removing the catch-all memory mapping makes unmapped DMA fail.
* ``0x0815ec10`` writes the second display's background; shutdown routines
  ``0x080bc4e0`` and ``0x0815dfe4`` disable its outputs and poll three
  control registers for stop acknowledgement. A partial TVO model retains
  these seven verified registers and acknowledges stop immediately, allowing
  Long Play sleep and hold-switch resume. Its scanout, clocks, interrupts
  and analog outputs remain absent; reset values and stop timing are
  unverified. Unmapped register accesses still fail.
* Boot ROM ``0x20004b4c`` / ``0x20004af4`` polls SPI TX/RX FIFO counts;
  ``0x20008f90`` programs a receive limit that includes the NOR command
  and address bytes. ``0x20004e5c`` then enables automatic reception.
  SPI now has bounded FIFOs and enforces this limit. Empty RX reads do
  not consume NOR data, and the three PWRCON1 gates suspend transfers.
  SETUP bits 3/4 are required for master transfers, following the original
  updater's ``0x0800fb84`` configuration sequence. Their master/clock-enable
  interpretation is corroborated by the older Samsung manual; isolated
  bit effects on S5L8702 hardware remain unverified.
  Serial timing, RX capacity, overflow behavior, IRQs and DMA requests
  remain incomplete or unverified; the audit lists these assumptions.
* ``0x08362a0c`` polls clickwheel status, consumes RX, clears pending flags
  and restores its mask. ``0x08362a9c`` decodes a button-query response;
  its button ordering differs from unsolicited wheel-event packets.
  The controller uses a bounded FIFO, level IRQ, QEMU input handlers and
  virtual timers. Report cadence and keyboard rotation increments are
  emulation choices, not measured physical timing.
* ``0x083607f4`` reads GPIO pin ``n`` using port ``n >> 3``, stride ``0x20``
  and data offset 4. ``0x0835eb10`` identifies PCON nibble 1 as output.
  Boot ROM ``0x200018fc`` encodes GPIOCMD port at bit 16, pin at bit 8 and
  configuration in the low nibble. PCON readback must retain all 32 bits.
  All sixteen ports now distinguish input levels from output latches.
  ``0x083606d8`` encodes output-low/high as modes ``0xe``/``0xf``;
  ``0x0835eb10`` uses these encodings to save outputs before sleep, and
  ``0x0835ead0`` restores them. GPIOCMD and PCON apply the same rules.
  The retained GPIO wheel serial workaround no longer overwrites other
  port-14 inputs, but its read-driven timing remains an approximation.
  ``0x080c8df0`` reads pin 86 for headphone detection; forcing it high as
  a supposed disk-presence signal was removed.
* The CPU has no parent bus and must be explicitly registered for system
  reset. The SoC now uses QEMU's Resettable adapter for this purpose.
  Reset restores PC zero and the boot ROM alias, including from WFI.
  The normal firmware reaches the main menu again after a paused reset.
* ``0x22002dcc`` saves clocks, writes/polls SYSIC clock-control bit 0,
  executes WFI, then writes/polls bit 12 and restores clocks. Clock-control
  readback is implemented. The PMU's active-low IRQ on GPIO 123 now wakes
  the CPU for hold changes and RTC alarms. Long Play stops the timer
  clocks; hold-switch wake restores them and returns to the normal UI.
  Other wake inputs and power-domain behavior remain incomplete.
* Boot ROM ``0x20001a20`` assembles custom AES key words in big-endian
  order with 32-bit read-modify-write accesses. Keys of 128/192/256 bits
  start at offsets ``0x5c/0x54/0x4c``. ``0x20001b8c`` and ``0x20001c54``
  program key size at bits 5:4, chaining at bit 3 and direction at bit 0.
  Treating the entire mode value 14 as a 256-bit key was incorrect.
* Boot ROM ``0x20002044`` submits sixteen SHA input words per block.
  CONFIG bit 1 starts compression, bit 3 continues a message, bit 0 is
  busy. Padding is supplied by the guest. ``0x20002030`` registers IRQ 40
  and ``0x20002118`` clears its status. The model now stores only one
  block and the five-word compression state, including intermediate hashes.
  retailOS ``0x08081680`` submits DMA with source/count at ``+0x84/+0x8c``;
  ``0x08091bc0`` restores digest words at ``+0x20..+0x30`` when switching
  hash contexts. These paths are implemented and fix the game's executable
  hash failure. A separate damaged word in the supplied OS image still
  prevents an unmodified-input game launch. The user's wInd3x decryptor
  has a matching final-ciphertext-block truncation bug; see the audit for
  the reproduction and the limits of reconstructing that word.
* The graphics compiler/copy path ``0x08238260`` -> ``0x082b7478`` emits
  A32 comparisons with a nonzero SBZ destination field. ARM926 decoding now
  ignores that field and computes the normal flags. This implementation
  choice is inferred from the original renderer; physical confirmation is
  outstanding. Other CPU models retain UNDEF. With the separate diagnostic
  OS data correction, Klondike now deals cards and responds to input.
* Original ``0x080ab624`` and ``0x080cd04c`` identify SM1's four
  ``+0xa44/+0xa48`` pairs as memory lengths and byte offsets. The inherited
  I2S frame-counter interpretation caused an alignment abort during game
  startup and has been removed. SM1's actual region layout and processing
  remain unimplemented; the current descriptors are register placeholders.
* The PL192 edge-glue device was removed. Pending levels come from the
  peripheral status registers; acknowledging a vector affects priority,
  not the source level. Cascaded acknowledge/EOI, nested priorities,
  software interrupts, and combined local/cascaded FIQ are covered by
  qtests. Spurious acknowledges cannot overflow the priority stack.
* ``0x081441d8`` and table ``0x083f18fc`` select display blend factors
  and routing. The compositor implements these alongside the planar source
  configured by ``0x081436dc``. Settings -> About now retains its gradient.
* ``0x083602c0`` identifies GPIO 55 for the panel update callback.
  ``0x080cd8ec`` dispatches VIC interrupt 1 to SYSIC group 5;
  ``0x22002ee0`` acknowledges its status after running the callback.
  The panel TE signal now follows this path, allowing the firmware to submit
  frames through ``0x080baef4``. The TE timer does not copy pixels itself.
  Its 60 Hz cadence and zero-width pulses remain timing approximations;
  controller transfer latency is also not modeled. See the audit for scope.

Disk and configuration observations:

* ``0x080a9104`` parses MBR firmware partitions of type 0 or 0x3f.
  ``0x08082c44`` recognizes data partition types including 0x0b and
  checks FAT signatures when probing a volume view.
* ``0x0806eb80`` reads the firmware directory in 4096-byte device blocks.
  Its directory location is firmware-start + directory-offset + one
  device block. Its resource location is firmware-start + devOffset +
  one device block, with entryOffset subsequently applied in 512-byte
  units. Directory entries are 40 bytes. The EFI osos loader uses its
  own direct devOffset convention.
* The rsrc update image's 0x800-byte wrapper is removed; its FAT VBR is
  at payload offset 0x1600. The resource extent must be 4096-byte aligned.
  Incorrect resource geometry prevents fonts and UI assets from loading.
* Runtime DMA initially read the data VBR six device blocks before the
  actual VBR. Extending the firmware partition count to include alignment
  padding made that read land exactly on the VBR and enabled filesystem
  writes. This supports the observed data-view start at the firmware
  partition's end; the builder leaves no gap between the two extents.
* For the generated disk, firmware start is byte 32768, firmware extent
  is 22856 device blocks, and data starts at byte 93650944. Runtime
  DMA uses 512-byte ATA sectors, so the data VBR is ATA LBA 182912.
* ``0x080b30f8`` constructs default preferences. ``0x0809c5e8`` reads up
  to 0xb8c bytes of Preferences. ``0x080587d8`` checks version 0x3e and
  the following zero word, with upgrade paths for older versions.
  ``0x080636cc`` checks the dirty flag and invokes ``0x080635cc``, which
  serializes object + 4 to ``iPod_Control/Device/Preferences``.
  Restoring that file was verified to remove language selection on a
  new process's boot. Preferences bytes are configuration, not executable
  firmware, and were not used to modify control flow.

Reproducing static analysis
---------------------------

Prepare separate relocation images::

  python3 scripts/ipod-classic/prepare-firmware.py \
    /path/to/osos.fw.decrypted /path/to/analysis

Import ``dram.bin`` as ARM little-endian ARMv5T at ``0x08000000`` in Ghidra;
import ``iram.bin`` at ``0x22000000``. Follow ARM/Thumb exchange instructions
when analyzing mixed-mode code. The checked routines above are ARM code.
Ghidra does not recover every indirect call or obfuscated function reliably;
check the assembly and live register state before trusting pseudocode.

The ``scripts/ipod-classic/ghidra/ExportFirmware.java`` post-script exports
selected functions from an existing analyzed program. For example::

  analyzeHeadless /path/to/projects ipod -process dram.bin -noanalysis \
    -scriptPath scripts/ipod-classic/ghidra \
    -postScript ExportFirmware.java /path/to/preferences.c \
    080b30f8 0809c5e8 080587d8 080636cc

Use one headless Ghidra instance at a time, a bounded JVM heap (2 GiB was
used), and ``-max-cpu 2`` for analysis. Keep generated images and decompiled
Apple code outside the source tree.

Reproducing the firmware-tail diagnosis
----------------------------------------

The supplied decrypted image has a damaged final library marker. The audit
records the matching wInd3x reader defect and original firmware checks.
With Python's ``cryptography`` package installed, reproduce the 576 synthetic
AES cases and verify the original image boundaries without modifying either
input::

  python3 scripts/ipod-classic/reproduce-wind3x-tail.py \
    /path/to/osos-original-encrypted.bin /path/to/osos.fw.decrypted

The encrypted input is the complete wrapped osos member extracted from the
original 2.0.4 update, including its padded ciphertext. Both inputs must
match the documented hashes. Synthetic results do not recover the hardware
key or original final plaintext.

To repeat the conditional game-startup experiment, shut down the source
machine and explicitly create a separate diagnostic disk and NOR copy::

  python3 scripts/ipod-classic/prepare-marker-diagnostic.py \
    --firmware /path/to/osos.fw.decrypted \
    --source-disk /path/to/ipod_hdd_restored.img \
    --source-nor /path/to/ipod_nor_restored.bin \
    --output-dir /path/to/new-diagnostic-directory

This tool verifies the source firmware extent, exclusively creates the
output directory and images, and reconstructs only the four-byte marker.
``marker-reconstruction.json`` records the changed bytes, offsets, hashes,
and limitations. This is an experimental workaround, not verified original
plaintext; the other twelve damaged bytes remain unchanged. Select these
copies explicitly with ``IPOD_DISK`` and ``IPOD_NOR`` when running the
launcher. The default boot continues to use the original-input disk.

Validation
----------

Run the firmware-independent peripheral tests from the source directory::

  QTEST_QEMU_BINARY=build/qemu-system-arm build/tests/qtest/s5l8702-test

They exercise I2C byte sequencing, MIU remap, timer reload/acknowledgement,
clickwheel FIFO semantics, DMA bounds, GPIO register width, PL192 priority
and cascade handling, AES CBC known vectors for all three key sizes, and
SHA compression/IRQ behavior including more than 1 MiB of streamed blocks.
The seventy-two tests also cover planar/alpha composition, panel GRAM and
window bounds, GPIO interrupt polarity/masking/acknowledgement, the panel
TE route, multi-block SHA DMA, and hash context save/restore. PMU cases
cover hold changes, deferred RTC writes, calendar rollover, retained RTC/GPM
state on reset, alarm programming/masking, and the complete IRQ route.
GPIO cases exercise all sixteen ports, configuration/output-latch behavior,
input preservation, invalid commands, reset and snapshot restoration.
A TCG test executes a small test ROM, enters WFI with IRAM remapped, and
checks that paused and running system resets restart through the boot ROM.
Clock cases cover oscillator/PLL selection, bus and ECLK division,
compare deadlines across gating, fractional progress across rate changes,
and snapshot restoration of clock and timer state.
I2C cases check deferred ACK/data and PMU read-to-clear effects, cancelled
writes, nanosecond deadline resolution, both clock gates, and snapshots
during running and suspended transfers.
SPI cases cover FIFO counts, clears, NOR receive limits, clock gating,
SETUP controls on all three ports, reset, and snapshots of queued transfers
and a NOR read with automatic reception suspended.
I2S/codec cases check stop acknowledgement, control-port addressing,
clock rates/gates, freeze and snapshot restoration. Output cases check
gain encoding boundaries, independent mutes, stereo/mono routing, headphone
and line power, HPDETECT input, frozen volume updates and restored playback.
Transition cases check intermediate gain, reversal, clock gating/rate changes,
independent analog crossings, timeout progress and pending-state snapshots.
A stalled host backend must leave the guest serializer progressing while
counting drops in its bounded host buffer. TX transport cases
check DMA ordering, payload/channel order, LRCK deadlines, partial frames
across gating/rate changes and snapshots with queued data. PL080 cases exercise
all nine supported width combinations, fixed/incrementing addresses,
descriptor interrupt ordering, LLI bus selection, bus-error handling,
reserved widths, unsupported flow modes, reset and snapshot IRQ state.
The UART regressions cover the four S5L port IRQ routes, 16-byte FIFO
status, UCON masking, UTRSTAT acknowledgement before draining RX data,
clock gating, reset and snapshot restoration, and character-backend output.
TX uses a separate clocked shift register and completes independently of
host backpressure. Tests check frame boundaries, divisor/format changes,
clock-source changes, fractional-cycle snapshots and receive timeout.
External clock routing, fine-tuning and timeout details remain provisional;
autobaud edge detection and dock/accessory protocols are unimplemented.

The SM1 regression checks that I2S serialization cannot change its memory
descriptors; their placeholder values are not asserted as hardware defaults.
The upper-control case reproduces the original firmware's divider readback
and independent bit updates at ``+0x1000/+0x1004``, plus reset and snapshots.
These registers still do not control a working SM1 processing engine.
Request cases check held/absent requests, software requests, short final
bursts, packing across requests, count readback, halt/drain, FIFO snapshots,
circular-list responsiveness and recursive register writes.
The TVO case checks its limited control-register model, stop acknowledgement,
reset/snapshot restoration and errors at adjacent unmapped registers.
Refresh signalling without guest submission must leave
the displayed frame unchanged.

``tests/tcg/arm/test-cmp-sbz.S`` is a separate CPU test. It checks twelve
A32 compare/test forms with nonzero reserved destination fields, flags,
register preservation, predication, and ordinary Thumb comparisons. With
ARM system TCG tests configured, run ``run-test-cmp-sbz`` and
``run-test-cmp-sbz-arm1176``. The latter verifies that the compatibility
choice has not relaxed decoding on ARM1176.

For firmware validation, cold-boot the generated disk with the command
above. Use QMP ``screendump`` to capture the main menu, navigate Settings
and About, verify reported free space, quit QEMU and repeat in a new
process. Examine the FAT volume only while QEMU is stopped, for example::

  mdir -a -i ipod_files/ipod_hdd_restored.img@@93650944 -s ::/iPod_Control

Successful tests created ``Device/clock`` and ``Tones/Beep.tone`` through
emulated ATA writes. A restore/iTunes graphic or an Apple logo alone is
not a passing boot result. Other firmware versions and unused peripheral
modes require their own reverse-engineering and validation.
