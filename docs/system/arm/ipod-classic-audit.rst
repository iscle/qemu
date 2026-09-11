iPod Classic implementation audit
=================================

Audit date: 2026-09-11, updated after SM1 and decryption-provenance work.
This describes the current working tree based on
QEMU 8.0.4, including inherited code and the changes made to boot the local
retailOS 2.0.4 image. It is an inventory of known gaps, not a claim that
every register or firmware path has been audited.
Subsequent implementation changes are identified below;
the peripheral fixes do not patch retailOS. A separate, explicitly identified
game experiment corrected one damaged data word in a disposable VM; later
tests use an explicitly reconstructed copy of the disk with that same word.
The CPU compatibility choice below is an explicit inference from the
firmware's generated code; it has not been measured on a physical CPU.

Evidence categories used below:

* **Reproduced**: observed in a running guest.
* **Source-confirmed**: directly visible in the emulator implementation.
* **Firmware-confirmed**: checked in the original local executable using
  disassembly/decompilation, with runtime state where indicated.
* **Unverified**: requires more reverse engineering or an end-to-end test.

Normal ROM/NOR/retailOS startup, the main menu, basic clickwheel navigation,
resource loading, and a mounted writable FAT volume have been demonstrated.
Those milestones do not establish working games, complete playback, synchronization,
power management, or complete restoration through iTunes.

Reported display and game failures
----------------------------------

1. Black backgrounds during menu transitions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Fixed in the reproduced transition; wider coverage remains open.**
Before the compositor changes, entering Settings ->
About briefly displays text and artwork over black, then the proper gradient
appears. This is captured in the local workspace at
``ipod-work/audit-about-frames/008.png`` through ``012.png``; ``013.png``
shows the completed background. These are screenshots of actual guest
output, not reconstructed UI images.

The original compositor in ``hw/misc/s5l8702-disp.c`` had two relevant gaps:

* It drew only packed-pixel descriptor slots, omitting the planar source.
* It ignored alpha by default. Its optional ``alpha-blending`` property
  reversed slot order globally without implementing blend/routing registers.

This was checked against **this firmware**, rather than trusting the older
addresses in the inherited display header:

* ``0x081436dc`` configures layers, with a separate path for layer 5.
  Format 8 sets register ``0x38900028`` to ``0x100`` and programs packed
  source strides at ``+0x2c``.
* ``0x08144078`` selects source addresses. Cases 0..4 write the packed
  slots; case 5 writes the plane-address registers ``+0x38..+0x44``.
* ``0x08143b64`` controls six enables in ``+0x08``. Layer 5 uses bit 7;
  layers 0..4 use bits 6..2. The old model gated only layers 0..2.

A paused black transition had CONTROL ``0x411181e1``, so layer 5 was
enabled. Its plane registers contained ``0x0931d5f8``, ``0x09318af0``,
zero, and ``0x09313fe8``. Packed layers 0 and 1 used format 7, stride
1280, and complementary sliding rectangles. Of 46,116 black pixels in
those rectangles, 39,110 corresponded to source pixels with alpha zero.
The original firmware has supplied transparency and an additional source;
the old model omitted that composition.

The compositor now implements planar 4:2:0, all six enables, the six routing
permutations in firmware table ``0x083f18fc``, and the alpha-factor pairs
programmed by ``0x081441d8``. ``0x08143ebc`` supplies constant alpha.
The experimental alpha switch has been removed. RGB565, XRGB8888, and
ARGB8888 are distinguished, with nearest-neighbor planar scaling.

The reproduced Settings -> About transition now keeps its gradient.
Captures in ``ipod-work/panel-te-about/`` show the actual guest output after
the GPIO/TE fix. Peak dark pixels in the measured content rectangle fell
from 45,304 to 1,483 (text in the starting Settings screen); the large black
background is gone.

Further original-input captures in ``ipod-work/display-transitions/fixed-*/``
cover entering/leaving Music and Cover Flow, entering Settings, and entering
About. Each sequence contains 100 host screenshots over approximately
3.5--3.8 seconds. None shows the former large black rectangle; peak dark
pixel counts in the same content rectangle are 1,367 for the Music/Cover
Flow paths, 1,503 for Settings, and 1,483 for About. These counts include
menu text. The private media fixture has one generated PCM track without
cover artwork, so this does not validate artwork loading. Host screenshots
can miss guest frames; asynchronously sampled display registers are not an
atomic record of each image. Other menus, exact color rounding/filtering,
overlapping windows within the multi-window group, and cursor priority
still need more tests.

2. Game launch failure
~~~~~~~~~~~~~~~~~~~~~~

**SHA failure fixed; a second failure is in the supplied OS image.**
The initial iPod Quiz/Klondike error was "This game cannot be launched."
The resource volume contains the bundled games under
``Resources/Games/games_RO/{11004,11010,12347}``, including encrypted
executables, manifests, signature containers, and assets.

The original Klondike loader ``0x080f9664`` successfully processes the
manifest through ``0x080f92bc`` and assets through ``0x080f93bc``. Its
executable verifier ``0x082cc5e8`` originally returned ``0xffff5bda``
(-42022) at ``0x080f99b8``. Disassembly, decompilation, and RAM captures
now establish the cause:

* The 632,804 executable bytes in RAM exactly match the resource file.
* ``0x082e7b58`` hashes one selected byte per four-byte word. The decoded
  initial state is ``0x6618cc0c``; each step computes
  ``state = state * 0xac27 + 0x9d47`` modulo 2**32, then selects byte
  ``(state >> 5) & 3``. SHA-1 of the 158,201 sampled bytes is
  ``1054a6af944a7e243ec8308adcd58c400f01ffc0``, exactly the manifest digest.
* The firmware's hash wrapper reaches its SHA hardware driver. The model
  lacked DMA input at ``+0x80/+0x84/+0x8c`` and writable digest words
  at ``+0x20..+0x30`` for saving/restoring software hash contexts.
* After implementing both paths, the original verifier returns zero and
  produces a valid context. ``0x08063208`` decrypts the executable in
  place, returns zero, and produces its ``eapp`` header. No verification
  result or executable instruction was overridden.

The next error, "This version of the game is no longer supported," comes
from ``0x080e9140`` returning -1001. It finds the ``Users`` export table
at ``0x08a106a0``, then checks its closing marker at ``0x08a10708`` against
``0x13061973``. The supplied ``osos.fw.decrypted`` already contains
``0x5a7c7c3c`` at file offset ``0x00a1bde0``. QEMU loads that wrong value
unchanged. Every preceding export table has the expected marker.
The bootstrap relocation sizes at ``0x2200478c``, ``0x220047a4``, and
``0x220047ac`` show that this word is the last four bytes copied into
DRAM. The remaining twelve bytes in the supplied image are not copied.

**Diagnostic data correction, not a peripheral fix:** changing only that
word to the loader's expected marker in the snapshot VM makes the module
loader return zero. Klondike enters its own startup function at
``0x180226f0`` and displays name entry and its Start Game menu. Selecting
Start Game initially exposed a third failure: an undefined-instruction
exception. The saved LR was ``0x092a1814``; the instruction at
``0x092a1810`` was ``0xe3574000`` (CMP r7, #0 with a nonzero SBZ destination
field). The firmware's graphics compiler/copy path ``0x08238260`` ->
``0x082b7478`` -> ``0x22000188`` puts this instruction in executable RAM;
``0x08236bf4`` calls the generated rasterizer. Other nearby generated
comparisons also contain nonzero destination fields.

**CPU behavior inferred from the original renderer:** the ARM926 model now
ignores the SBZ destination field for A32 CMP/CMN/TST/TEQ, while retaining
the ordinary flag computation and omitting a destination write. Other CPU
models retain UNDEF. No guest instruction is rewritten. The
`ARM Architecture Reference Manual
<https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a>`_
classifies nonzero SBZ fields as UNPREDICTABLE; it does not guarantee this
response for every implementation. Physical ARM926/S5L8702 confirmation
remains outstanding. This choice is not a claim of complete CPU fidelity.

The new TCG test exercises all twelve compare/test forms, destination
fields 4 and 15, expected flags, unchanged operands, failed conditions,
and ordinary Thumb comparisons. It passes on ARM926 and checks continued
UNDEF behavior on ARM1176. The pre-change ARM926 binary fails the first
noncanonical encoding. After the change, the diagnostic Klondike run deals
cards, draws three cards from the stock, and moves the selection with wheel
input. This earlier snapshot run did not test saving/resuming, audio or
complete gameplay. The later fresh-boot test is described below.
The normal disk, original input, and launcher retain their existing bytes;
there is no marker substitution in an emulator device.

A separately downloaded Apple ``iPod_35.2.0.4.ipsw`` has SHA-1
``94fc68f5aad63a5dc8cf8b62408907ec050a4bc7``. Its embedded S5L8702 Secure
Boot certificate verifies the image's PKCS#1 SHA-1 signature over the
0x800-byte header and 0xa1b5f0 encrypted payload bytes. The embedded
certificate chain also verifies. This checks the image against its embedded
signature, not an independent root of trust or reconstructed plaintext.
The original header declares an unrounded payload length of 0xa1b5e8;
the supplied decrypted wrapper rounds it to 0xa1b5f0. The user confirms
that wInd3x decrypted this file on a physical iPod which is no longer
available. Its source contains a matching final-block bug:

* At commits ``591c4400043fddbc3f930a28978ae065a0b3143d`` (March 2023)
  and ``e97a90843a6277d94fe585bca93b835845c3593c`` (the inspected default
  branch), ``image.Read`` reads only the unrounded ``BodyLength``.
* ``decrypt.Decrypt`` processes 48-byte chunks, padding the last chunk
  with zeros. Here it discards the last eight real ciphertext bytes,
  ``4339a697f74ced0b``, and substitutes zeros. The final AES input block
  becomes ``1282df29ee99a6270000000000000000``.
* Its CBC chaining construction preserves earlier plaintext blocks but
  corrupts the final block, starting at file offset ``0xa1bde0``. This is
  exactly the location of the damaged marker. Its output also has exactly
  the supplied image's rounded length. The recovery file receives the same
  decrypted chunks, so it would retain this corruption.
* A known-key reproduction checks 576 length/key-size combinations and
  confirms that reading the rounded ciphertext length preserves the
  plaintext. These synthetic checks reproduce the mechanism, not the
  actual hardware-key output. The complete original final plaintext
  remains unknown.

wInd3x's ``img1-improvements`` branch contains
`the corresponding reader fix
<https://github.com/freemyipod/wInd3x/commit/19702d226257415a554f2aaad67cecc79817e57f>`_,
which rounds the input length to sixteen bytes. That commit is not in the
inspected default branch. Changing the reader cannot repair an already
decrypted file without another decryption. Both the locally archived copy
and its Git blob contain the same damaged bytes; the public firmware
repository still points to that same archived commit.

This strongly establishes the source of the damage, but does not
cryptographically recover the block. The diagnostic marker replacement
is still a structural reconstruction, not verified original plaintext.
Evidence and a read-only reproducer are in
``ipod-work/wind3x-tail/`` and ``ipod-work/reproduce-wind3x-tail.py``;
``ipod-work/wind3x-tail-reproduction.json`` records the original hashes,
ciphertext boundary, synthetic results, and recovery limitations.
Portable copies of the reproducer and explicit diagnostic-disk builder are
included in ``scripts/ipod-classic/``; the machine guide documents their
arguments and prerequisites.

**Later fresh-boot diagnostic:** a separate private disk copy reconstructs
only those four marker bytes, with the transformation recorded in
``ipod-work/wind3x-tail/marker-reconstruction.json``. It exposed an
independent SM1 memory-address bug, described in item 7. After removing that
incorrect I2S-counter coupling, a new VM boots without a debugger and:

* Klondike accepts a profile name, deals cards, draws three cards, and moves
  the selection. Exiting and relaunching resumes the same deal within this
  VM session.
* iPod Quiz reaches its menu, starts Movie Trivia, and displays questions
  and its statistics screen.
* Vortex reaches its main menu, Level 1 prompt, and live 3D gameplay.

These tests used a none audio backend. Game sound, wins, later levels,
complete controls, Music Quiz's media requirements, and save/profile
persistence across a fresh boot remain unverified. The full final AES block
is still unrecovered. The source firmware, normal disk and default launcher
retain their existing bytes; the reconstruction is confined to the
explicit diagnostic copy. This is conditional game-startup validation,
not complete hardware emulation or an unmodified-input game boot.

Display, DMA, and media implementation gaps
-------------------------------------------

3. LCD frame submission and completion
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Submission path corrected; timing remains approximate.** Host redraws now
read the panel's GRAM, which changes on PIO pixel writes or guest frame
submission. The synthetic RAM framebuffer and fixed 200-microsecond IRQ
pulse are removed. The old LCD IRQ output was not connected to the CPU.
A subsequent experimental continuous-copy timer was also removed: it hid
the missing panel signal instead of honoring firmware frame submission.

Original-firmware disassembly, Ghidra output, and live breakpoints establish:

* ``0x081448b0`` enables the engine at ``+0x70`` and sets geometry at
  ``+0x74``. Initialization also writes ``+0x78 = 0x5000a`` and
  ``+0x7c = 0x804``; their timing fields remain undecoded.
* ``0x083602c0`` supplies GPIO 55 and polarity 1 to ``0x081447fc``.
  It registers callback ``0x080cc210`` through ``0x080404d0``.
* GPIO 55 maps to SYSIC group 5, bit 15, then VIC interrupt 1.
  The old SYSIC wiring incorrectly used VIC interrupt 14 for this group.
* The callback reaches ``0x08143f34`` and submits a frame only if its
  dirty flag is set. ``0x080baef4`` selects ``+0x80 = 1``, calls the panel
  window/memory-write routine ``0x080db2e0``, then clears ``+0x80``.
* The new model's panel TE output reaches that callback and the original
  frame-submission routine, confirmed in
  ``ipod-work/panel-te-firmware-trace.log``. No firmware patch is involved.

Panel command ``0x35`` enables TE and ``0x34`` disables it. The panel's
virtual timer produces that GPIO signal; it never copies a frame itself.
Sleep stops the signal. This is an emulation scheduling mechanism, not a
new timer peripheral visible to the guest.

**Remaining approximations:** ``refresh-rate=60`` is an unmeasured panel
oscillator assumption. TE pulses have zero modeled width; horizontal TE
mode is not implemented. Transfer latency is synchronous, ``+0x8c`` always
reads idle, and PIO status always reads ready. Controller clocks, bus-cycle
timings, exact transfer trigger/latching, FIFO/DREQ behavior, and any LCD
controller interrupt distinct from GPIO TE require further reverse
engineering. The firmware's initialization constants do not establish 60 Hz.

4. Display formats and panel behavior
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Partly implemented.** Planar format 8 and packed formats 3, 6, and 7 are
implemented. Packed formats 0, 1, 2, 4, and 5, interlaced/packed YUV formats
9 and 10, palette, gamma, color-matrix programming, rotation, and scaler
filter coefficients remain unsupported. A failed DMA read preserves the
previous panel frame instead of committing a partial composition.

**RGB565 source cropping corrected.** Original routine ``0x081436dc``,
specifically ``0x08143978..0x081439f4``, splits a source crop between a
word-aligned address and the format register's starting-nibble field in
bits 2:0. For format 3, an odd source column sets bit 2 to skip the first
halfword. The compositor previously ignored that field, displaying the
preceding pixel and shifting every cropped row by one pixel. It now applies
the halfword offset before clipping to the panel. The new qtest fails on
the previous model and passes for all five windows, checking odd/even
source columns, row strides, clipping on both axes, and a crop at the end
of RAM. An overlong DMA read still preserves the previous panel frame.
This fixes a source-addressing omission; it does not establish the remaining
packed-format meanings, DMA padding/burst behavior, or reserved-bit behavior.

The fixed 320x240 panel has bounded GRAM, window addressing, sleep/display
blanking, and vertical TE signalling. PIO pixels remain RGB565-only; panel
orientation, pixel-format changes, gamma, brightness, and backlight behavior
are incomplete. The synthetic guest RAM mapping at ``0x0fe00000`` is gone.

**Second display control is partial; its output remains unimplemented.**
Removing the SoC's
blanket ``unimplemented-mem`` mapping exposed a boot write to ``0x39200048``.
Original constructor ``0x081ce3cc`` selects the second layer manager,
calls ``0x0815e140``, and invokes its vtable setter ``0x0815ec10`` with
``0x00108080``. The setter writes this value directly to the register.
The same driver's ``0x0815f3d8`` selects 480-line or 576-line geometry and
programs both ``0x39200004`` and ``0x39300008``; ``0x0815dc10`` programs
plane geometry in ``0x39100000``. This supports identifying an external
display path separate from the working panel compositor at ``0x38900000``.
The interpretation of ``0x00108080`` as limited-range YCbCr black is an
inference, not a measured hardware result.

The earlier four-byte unimplemented device only discarded the background
write. Long Play then exposed another unmapped access at ``0x39300280``
and reset the guest. Original routine ``0x080bc4e0`` writes 1 there and
sets bit 0 at ``0x39300284``; output initialization ``0x080b3888`` clears
the latter. Shutdown ``0x0815dfe4`` also clears encoder-config bits 3:0
at ``0x3930003c`` and stops three blocks at ``0x39300000``,
``0x39200000`` and ``0x39100000`` in that order. Each stop clears control
bit 0 and polls bit 1 for acknowledgement.

The partial TVO device now retains these seven verified registers,
including the background, and derives read-only stop acknowledgement
from the enable state. Reset and snapshot restoration are covered by a
qtest; unknown adjacent registers still return bus errors. The default
register values and other writable/reserved-bit masks remain unverified.
Stop acknowledgement is immediate because the model has no pending
scanout; its latency is not established by the firmware's timeout.
Composition, scanout, clock gating, interrupts and analog output are still
absent. No blanket success-returning memory region is used.

A fresh boot reaches the normal main menu with this restricted mapping.
Long Play reaches WFI at ``0x22002e74``, with all three controls reporting
stopped, and hold-switch resume restores the Music menu. This verifies the
tested shutdown path, not external display operation or all sleep modes.

5. PL080 DMA requests and transfer timing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Request bypass removed; peripheral connections remain unfinished.** The
controller now implements single/burst requests in DMAC-controlled modes
0--3, a four-word FIFO per channel, and DMACCLR acknowledgement outputs.
The four request classes each have sixteen named GPIO inputs; last-request
inputs are retained but their peripheral-controlled transfer modes 4--7
remain unimplemented. Software request registers latch write-one requests;
single/burst bits clear when serviced. Hardware acknowledgement remains high
until all requests from that peripheral are low, preventing a held request
from triggering duplicate transfers.

Memory-to-peripheral transfers wait for BREQ before accessing the source.
Peripheral-to-memory transfers accept source single/burst requests, and
peripheral-to-peripheral transfers can retain source data until the
destination requests it. Partial packing remains in the channel FIFO.
Halt stops new source requests while allowing queued data to drain; disable
discards the FIFO. Active reflects FIFO occupancy, and TransferSize readback
derives remaining destination data in source-width units. Terminal count
and descriptor reload wait until destination delivery completes.

I2S0 TX now drives DMAC0 burst request 10 and consumes its DMACCLR
acknowledgement. The observed stereo16 configuration accepts one
four-halfword request, then consumes two samples per codec LRCK frame.
The next request follows buffer availability and the enable/clock state.
SPI and the other audio request sources remain unconnected. The I2S
transaction buffer is an abstraction of the verified DMA burst size; the
physical FIFO depth, request thresholds and upper configuration bits have
not been established from the original firmware. Its values differ from the older
S5L8700 manual, which must not be treated as an S5L8702 specification.

A bounded main-loop callback now prevents nonempty circular descriptor
lists from trapping the host in one DMA invocation. Its work limit is a
host scheduling bound, not a hardware timing parameter. Arbitration is
functional fixed priority at individual-access granularity; AHB bursts,
bus-cycle timing, clock gating, request synchronizer latency and lock
behavior remain incomplete. Both AHB master selections use one address
space; big-endian transfers and protection attributes remain unsupported.
Recursive DMA writes that would reprogram the running controller are
rejected with a bus error to prevent host FIFO corruption; physical
self-reprogramming behavior is not established.

**Transfer and error handling corrected.** Source/destination widths now
control packing, address increments, and the count in source-width units.
Terminal count follows the completed descriptor's interrupt bit, before
loading the next descriptor. LLI bus-selection bits are excluded from the
address. Source, destination and descriptor accesses check ``MemTxResult``;
a bus error latches the channel error and disables that channel. Interrupt
masks are recomputed, and reset/post-load restore output levels. Reserved
widths and nonintegral packing are rejected; unimplemented flow modes log
and wait instead of terminating the host through ``hw_error``. MMIO is
explicitly little-endian. These changes follow ARM's
`PL080 DDI 0196G manual
<https://documentation-service.arm.com/static/5e8e3c6488295d1e18d3a8c3>`_
and were checked against the original setup and descriptor-update routines.
The I2S0 TX connection is described below; other peripheral request sources
and physical bus timing remain unfinished.

Before the I2S transport, a signed, generated WAV library reached Now Playing
but remained at 0:00. Channel 1 is enabled in memory-to-peripheral mode with destination
request 10 and TX address ``0x3ca00010``. In the preceding bypass model,
its count was zero while its
self-linked descriptor at ``0x22010020`` contains a new count of 2048
halfwords. Original setup ``0x22004a20`` copies the previous channel control
into that descriptor before enabling the channel; ``0x220048fc`` later
updates descriptor contents. Read-only GDB tracing now confirms the ordering:
the channel-enable store at ``0x22004c0c`` consumes the complete buffer and
loads a zero-count descriptor before the next instruction executes. The
original update at ``0x220049e4`` subsequently stores count 2048 into that
descriptor; the channel remains enabled with count zero. No request event
resumed it. The PL080 manual specifies that a zero-count DMAC-controlled
channel does not initiate a transfer, so periodically reloading that
descriptor would conceal the missing request/FIFO behavior.

A further original-firmware trace establishes that the first DMA enable
precedes TXCOM enable. ``0x080ed168`` enables CLKCON, then ``0x22008744``
programs TXCON ``0x0b100019``, RXCON ``0x1000`` and CLKDIV ``0x110``.
At the first channel-enable write, TXCOM is still zero; ``0x220087e0``
later sets its bits 1/2. The subsequent two observed enables use TXCOM 6
and four-halfword DMA bursts. This confirms the need to wait for the
peripheral's readiness rather than consuming a buffer at channel enable.

6. Audio output and input
~~~~~~~~~~~~~~~~~~~~~~~~~

**Digital TX and a codec output path implemented for the observed mode;
capture remains absent.** ``hw/misc/s5l8702-i2s.c`` retains one
four-halfword DMA request and consumes stereo frames using the codec's
LRCK input. TXCOM bits 1/2 control requests/transmission; CLKCON enable,
PWRCON1 bit 7 and codec clock gating also constrain progress. Partial
frame progress survives clock pauses and rate changes. Terminal-count
interrupts now follow delivered DMA buffers, allowing the original audio
task to refill them and advance Now Playing.

A captured 51,016-frame serialized sequence is byte-for-byte identical
to the generated source WAV, with no missing sample-counter increments.
Its spectral peaks are approximately 440.176 Hz left and 660.263 Hz right
at the codec-derived LRCK of 12 MHz / 272, matching the nominal 440/660 Hz
fixture at the slightly different programmed clock ratio. The UI advances
past 0:00; the checked screenshot shows 0:22. No guest data, decoder return
value or instruction was substituted to obtain this result.

The model recognizes the original TXCON ``0x0b100019`` configuration;
other formats and ports remain unsupported. Four buffered halfwords are
one software-modeled DMA transaction, not evidence for the physical FIFO
capacity. Request thresholds, bus/serial phase relationships, individual
SCLK edges and underrun/overrun status remain unverified or absent.
Register ``+0x3c`` still returns its inherited unverified value. A typed
QOM link now carries serialized frames to the CS42L55. A standard QEMU
audio voice consumes the selected headphone or line output. The first
WAV-backend run contains sustained stereo peaks at approximately
440.192/660.285 Hz and a peak amplitude of 25 at the firmware's initial
headphone setting. This independently checks delivery through ``AUD_write``;
the earlier serializer trace alone did not establish host output.

The host staging buffer is bounded to 16 KiB. It preserves partial backend
writes and never controls guest DMA or LRCK progress. Overflow drops host
frames and increments a diagnostic counter; a regression deliberately
delays host consumption and checks that all 12,000 guest frames still
serialize. The normal captured playback run reported zero drops. Backend
rates are rounded to integer Hz (44118 for the original 12 MHz / 272).
Rates above 192 kHz disable host output as a resampler resource bound;
they do not change the modeled LRCK. This is a host limitation, not a
physical clock restriction. Reset, rate changes, power-down, clock gating and snapshot
load discard the codec staging buffer. Previously presented audio cannot be
rolled back. Clock gating deactivates the host voice without destroying it. An earlier
implementation recreated the WAV backend on wake and truncated the capture;
retaining the voice fixes that lifecycle error. System reset and snapshot
load still recreate the voice to discard old software-voice state, which
can restart a WAV file. Resampling and long-duration drift need wider testing.

**Clock stop acknowledgement corrected.** Original retailOS
``0x2200881c`` writes zero to TXCOM, RXCOM and CLKCON, then polls CLKCON
bit 1 at ``0x220088a4`` before setting the PWRCON1 gate. For port 0,
``0x22004ca4`` selects ``0x3ca00000`` and the gate is PWRCON1 bit 7.
The previous register-storage model returned zero indefinitely after this
sequence. CLKCON now derives its stop acknowledgement from the enable
state; enabling again clears it. A regression test reproduces the missing
acknowledgement on the previous binary and covers stop, restart and reset.
Clock disable currently discards any pending transaction samples and
acknowledges stop immediately. This remains an approximation: drain,
abort, FIFO-flush semantics and physical clock-stop latency have not been
established. The serializer now consumes its clock inputs, but does not
establish that final shutdown behavior.

Original configuration routine ``0x22008744`` writes TXCON, RXCON and
CLKDIV; ``0x220087e0`` sets TXCOM/RXCOM bits 1/2. The sample-rate routine
``0x080a7838`` selects CLKDIV ``0x110`` and codec register 5 ``0x0b`` for
44100 Hz, and changes the clock source/divider for 32000 Hz. Further
disassembly confirms codec initialization ``0x0809f1c8`` writes register 4
``0x2e``: the codec generates serial clocks in master mode. Its documented
MCLK/2 and sample-rate ratio give approximately 44117.6 Hz from 12 MHz,
consistent with the nominal 44100 Hz selection. This is a derived rate,
not a hardware measurement. The codec now supplies that LRCK period to
I2S0. Serial bit framing, SCLK phase and CLKDIV's full role still require
decoding. I2S ports 1 and 2 are not instantiated.

7. SM1 engine and audio codec
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Incorrect I2S counter coupling removed; SM1 remains partial.** The
inherited model treated ``+0xa48`` (four entries, stride ``0x14``) as a
playback position and returned the I2S stereo-frame count modulo 32768.
Original retailOS 2.0.4 disproves that interpretation:

* ``0x080ab624`` returns the configured CPU memory base plus ``+0xa48``
  directly, without multiplying it by four. The companion ``+0xa44``
  is returned as a length.
* ``0x080cd04c`` uses the returned pair for an address-range check,
  comparing a pointer against base and base plus length.
* ``0x0802e5ec`` uses that base for a memory upload; ``0x0809f0ec``
  copies words to it. During a diagnostic Klondike launch, the synthetic
  I2S count made this address ``0x220207fa`` (``0x220240ce`` in a second
  run). The first word store at ``0x0809f108`` caused a data abort:
  DFSR 1, alignment checking enabled. The executable verifier and module
  loader had both succeeded, and Klondike had entered its startup code.

The unrelated I2S link, frame-count readback and invented ring wrapping
are removed. A regression test transmits actual I2S frames and checks that
the memory-description pairs remain unchanged; it fails against the old
model after the first frame. It deliberately does not assert their current
placeholder values as hardware defaults. The SM1 device now uses Resettable
and explicit little-endian register accesses.

**Still missing:** the actual sizes, offsets, access permissions and aliasing
of these SM1 memory regions. The descriptors currently retain the generic
register-file placeholder, initially zero; this is not a completed memory
map. Power/run/stop responses remain synthesized, and the processing engine,
interrupts and timing are unimplemented. The older comments also claimed
that retailOS never accesses beyond the low 4 KiB. Original ``0x080ab4b8``
accesses ``+0x1004/+0x1008/+0x100c``; the latter two remain unimplemented.
Several inherited addresses referred to a different firmware layout and
have been removed from the implementation comments. A successful codec
output test does not establish these SM1 functions.

**Upper-page readback corrected.** Original IRAM ``0x2200200c``, clock
group 8, writes divider minus one to ``0x38501000``. The frequency reader
``0x08360030`` uses its low nibble plus one with the HCLK source/divider
entry. The zero window discarded this programming: writing 3 subsequently
read as divide-by-one instead of divide-by-four. Original ``0x080ab4b8``
also independently sets/clears bits 0 and 1 at ``+0x1004`` using
read-modify-write operations; returning zero loses the other enabled bit.
These two programmed words now retain their values and survive snapshot
restoration. SM1 VMState version 2 includes them; version 1 restores the
old zero-window contents. The regression reproduces the divider failure
on the previous binary and checks bit preservation, reset and snapshots.

This is register readback, not a completed clock or execution model. The
divider does not yet pace SM1 execution, and the control bits' effects on
the engine remain unknown. Zero reset contents and storage of unused bits
are provisional. The write-only usage of ``+0x1008/+0x100c`` in the original
driver does not establish their readback or full command semantics, so
those registers still need implementation.

Further original-code tracing identifies the context table at
``0x08b2f648``, its configured CPU memory base ``0x22020000``, and the
program-image loader at ``0x080b4768``. Its address mapper ``0x0809a548``
and copy dispatch ``0x0802e6bc`` handle separate program/data layouts,
including packed halfwords and banked word uploads. The subsequent poll
at ``0x0809acec`` waits for ``+0xa98`` bit 2 after an upload; this sequence
does not establish the inherited label "clock ready". The helper writing
``+0x824`` also runs from ``0x080a3010`` when ``+0x860`` bit 2 is set.
That status register and the associated command semantics need decoding.
Using either write as a generic power-on event remains a workaround.
The processing instruction set and its actual memory geometry have not
been identified. Evidence is in ``ipod-work/sm1-geometry/``.

``ipod-work/wind3x-tail/sm1-evidence.txt`` records the original routines,
fault traces, removed assumptions and the failing/passing regression.
``runtime-results.txt`` records the conditional game runs. A separate
unmodified-input boot also reaches the main menu and plays the diagnostic
track, with advancing Now Playing time and nonzero serialized I2S samples;
its evidence is ``ipod-work/audio-fixture/sm1-original-*``. These runs use
a none audio backend and do not establish audible output or DSP fidelity.

**Codec control and master clock partly implemented.** Original
``0x0809f188`` selects a register and reads one byte; ``0x080ca298`` applies
masked updates. Its initialization clears the ADC/pump power bits while
preserving global PDN until the subsequent power-up operation. The old
zero-filled reset state lost that sequence. The codec now has documented
reset values, read-only ID/status registers, MAP.INCR addressing, and
FREEZE handling for its active controls, following
`Cirrus DS773F1 sections 4.8, 4.14, 5 and 6
<https://www.mouser.com/datasheet/2/76/CS42L55_F1-33604.pdf>`_.

Original clock group 6 programming at ``0x220021a0`` selects CLKCON3's
low source/divider fields; ``0x0835ff54`` controls its bit-15 gate. The
clock controller now connects this output to the codec's MCLK input.
The original 32000 Hz selection uses PLL2 / 27 = 8 MHz with register 5
``0x09``; the other checked rates use 12 MHz. QEMU Clock objects derive
LRCK from the selected input and codec controls, preserving fractional
periods. Source gating, master/slave selection, MCLK disable and FREEZE
affect that derived clock. This introduces no periodic callback.

**Nominal PCM playback controls implemented.** Original volume routine
``0x080ca36c`` calls ``0x08088bc4``, which computes and clamps headphone
gain and calls ``0x080cf76c``. Disassembly and Ghidra establish that the
latter sends three bytes to I2C address ``0x4a``: an incrementing register
address and the same value for both channels. Live UI input produces
headphone values ``0x5a``, ``0x56``, ``0x51`` and ``0x4d``, while line
volume stays zero. The older trace of single writes at ``0x080a79bc``
missed these paired updates. No guest instructions or PCM were substituted.

The codec implements nominal PCM/master/output gain, independent digital
mutes, ganged volume, PCM mix/swap/polarity, global and per-output power,
and the DAC versus analog-input mux. Initialization sets PDN_DSP while
playing: the model bypasses the DSP mixer and polarity controls in this
state. DS773F1 figure 12 explicitly keeps master gain, mute and soft ramp
available with DSP power off, confirming the path used by the original
initialization. A physical-device comparison remains unavailable.
An HPDETECT GPIO input supplies status and output power selection; board
jack insertion policy remains unwired. Host ``line-out`` selection chooses
one connector, so headphone attenuation is not bypassed by adding line
output. Tests cover control encodings and selected signal paths.

**Volume transitions implemented at the modeled sample level.** Original
``0x0809f1c8`` enables register-7 mask ``0x1c`` during initialization and
resume, and clears it before shutdown muting. Fresh Ghidra and disassembly
confirm this sequence. It enables digital soft ramp and analog zero cross;
the mask also includes reserved bit 4, whose effect remains unexplained.
PCM and master gain now move by one eighth dB per LRCK cycle. Progress is
derived from elapsed virtual time, preserving fractional cycles through
clock gating and frequency changes. It continues with codec LRCK even
when the separate I2S bus clock is stopped. FREEZE stages target controls
while existing transitions continue; clearing it commits the new targets.
No periodic codec timer or host-time delay was added.

Headphone/line gain and mute use separate per-channel latches. A change
waits for a sampled DAC zero crossing, with independent crossings even
when targets are ganged. A live firmware volume adjustment latched right
before left by 17 frames; both transitions occurred at the respective
input sign crossing. All 331,334 paired samples in that capture agree
with the latched nominal gain within one S16 rounding unit. The 32 codec
frames after separately disabling the I2S trace lack matching input
records and are excluded explicitly, not treated as sample mismatches.

**Zero-cross timeout remains provisional.** DS773F1 section 6.7.2 gives
1024 sample periods, but its 10.7 ms example at 48 kHz corresponds to 512
LRCK cycles. The model uses the literal 1024-cycle count, with timeout
progress paused when LRCK stops. Neither the firmware nor an available
measurement resolves that inconsistency. Zero detection uses sampled
pre-amplifier DAC values, without analog reconstruction/filter delay.
Already-silent analog inputs and powered-down rails latch controls directly.
Those analog timing details still require characterization.

Remaining codec gaps are substantive: digital mutes and power transitions
are immediate. The digital mute-ramp endpoint is undocumented in the checked
source; no arbitrary attenuation floor was introduced. There is no ADC input,
analog passthrough source,
beep/tone/de-emphasis/limiter processing, analog filter or amplifier rail
model, SCLK edges, clock-error/overflow status, or reset-pin sequencing.
Register 6 controls Class H amplifier power adaptation, not serial format;
its voltage-rail behavior remains unimplemented. The PCM frame interface
assumes the original serial format pairing.
Nominal analog gain is not a measurement of the chip's nonlinear gain
curve. Undocumented initialization controls remain storage. The digital
path saturates to a 24-bit DAC range before nominal analog gain and S16
host conversion; exact internal rounding and clipping are unverified.
Clock startup/stop phase and power transition delays remain incomplete.
Register 1 retains the reference model's unverified ``0x5a`` revision.
Read-only QOM properties expose derived LRCK, the last selected output
frame, frame count and host drops; none are guest-visible registers.
Codec VMState version 4 preserves current digital levels, analog latches,
pending timeouts, previous DAC samples and fractional cycle progress;
it rejects earlier snapshots. Save synchronizes progress and load anchors
the retained phase to restored virtual time. Additional read-only QOM
properties expose PCM/master levels in eighth-dB units and the four analog
control latches. Tests include intermediate PCM amplitude, ramp reversal,
clock gating/rate changes, crossings and snapshots of pending transitions.

**I2C address switching corrected.** STOP previously overwrote the BUSY bit
before checking whether to call ``i2c_end_transfer``. QEMU's I2C bus then
kept the PMU selected for subsequent codec transfers. The original firmware
at ``0x080a79bc`` supplies address ``0x4a`` and a register/value buffer to
``0x08360cac``; its dispatcher ``0x083608dc`` writes TX STOP ``0xd0`` or RX
STOP ``0x90``. Disassembly, Ghidra, and live breakpoints confirm this path.
STOP and interface disable now release the bus and cancel pending byte
completion. The new qtest fails on the old binary with the codec's value
in the PMU's register, and passes after the fix. A fresh normal boot reaches
the UI and traces codec initialization to ``0x4a``. This fixes control-bus
isolation; host output now additionally depends on the connected I2S and
codec playback models described above.

**I2C completion ordering corrected; wire timing remains approximate.**
The original dispatch routine ``0x083608dc`` checks ACK/NACK at
``0x0836091c`` / ``0x0836097c`` and consumes receive data at
``0x083609b0`` / ``0x083609dc``. The controller previously performed the
slave operation immediately and delayed only its completion flags. An
absent address therefore reported NACK without elapsed time, and a write
cancelled immediately after GO had already changed a PMU register.
Regression tests reproduce both failures against the previous binary.
Address selection, ACK/NACK, receive data, and slave side effects now
occur in the byte-completion callback. Pending transmit data, direction,
and ACK generation are latched for the operation. STOP, interface disable,
and reset discard the pending operation; a receive does not consume the
PMU's read-to-clear interrupt latch before completion. Deadlines use
nanosecond resolution without rounding the start down to a microsecond.
This is an atomic byte model, not a bit-level I2C simulation. It retains
the unverified 25 microsecond duration and lacks clock stretching,
arbitration and programmed divider timing. The firmware's
completion handshake does not establish the exact wire or slave sampling
phase within a byte.

Further clock investigation confirms retailOS ``0x08360b94`` writes
``IICCON=0x184`` and ``IICUNK14=0``. The supplied EFI ``DxeSmbus`` and
``PreEfi`` modules use the same constants. These paths do not compute a
bus-frequency/divider relationship. The reference driver's prescale
comment is explicitly unconfirmed, so it has not replaced the fixed delay.

**I2C clock gates now implemented.** The original ``aupd.fw.decrypted``
contains a diagnostic executable at file offset ``0xbbd68``. Its startup
code establishes code at ``0x08000000`` and initialized data at
``0x08400000``. These addresses belong to that diagnostic, not retailOS.
Ghidra and disassembly of diagnostic ``0x08009434`` show bus 0 selecting
gate ID ``0x2c`` and bus 1 selecting ``0x2a``. The gate dispatcher
``0x080164f4`` maps these IDs to PWRCON1 bits 4 and 6 respectively;
``0x08005e10`` clears the selected bit to enable the controller.
This independently confirms the gate assignments from original Apple code.
The diagnostic's configuration routines ``0x08009758`` / ``0x0800a928``
also write IICCON bit 6 and bits 3:0 from separate parameters, but do not
establish their frequency formula. Writes to the additional ``+0x28``
register are visible there; this register remains unimplemented.

Both controllers now consume gated QEMU clock inputs. An address, write
or read waits while disabled; gating during a byte preserves its remaining
provisional duration. Completed IRQ status remains pending across gating.
STOP, interface disable and reset cancel suspended bytes as well as running
ones. Snapshot state includes the input clock and suspended operation.
Tests reproduce the previous completion while disabled, and check both
gates independently, nanosecond remainder retention and suspended snapshots.
The clock input currently controls whether work progresses; nonzero rate
changes do not yet scale byte timing. Alternate clock selection at ``+0x14``,
wire-level gate effects and retention of a physical divider mid-byte remain
unverified. Register accesses remain available while the clock is gated.

8. JPEG decoder shortcuts
~~~~~~~~~~~~~~~~~~~~~~~~~

**Source-confirmed workarounds.** ``hw/misc/s5l8702-jpeg.c`` assumes
320x240 with 4:2:0 sampling. It decodes the whole frame on the first
trigger and fakes per-MCU staging on subsequent triggers. It additionally
writes decoded planes at ``OUT_CR + 0x10000``, a firmware-specific scratch
buffer assumption rather than a demonstrated hardware DMA destination.
One busy bit toggles on reads. Dimensions, incremental state, completion,
DMA bounds/errors, and reset during decoding are incomplete.

9. Playback and media databases
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Unverified end to end.** Music-menu navigation does not establish song
decoding, audio playback, seeking, gapless playback, album art, video, photo
slideshows, or TV output. The default generated volume has no populated
music/photo database. A separate private fixture now contains a generated
30-second stereo 16-bit WAV and a libgpod database signed for the identity
supplied by the original boot path. Original hash58 key derivation
``0x08038718``, its lookup tables and HMAC initialization were checked
against the signing algorithm. A zero-identity database was rejected;
the correctly signed one displays the song and opens Now Playing without
changing the firmware's database checks. With the I2S0 transport, the UI
advances to 0:22 and captured digital samples match the source WAV exactly.
This validates that fixture's database, WAV data path, buffer refill and
serialization. Audible output remains absent. Compressed formats, end of
track, seeking, pause/resume and gapless playback remain unverified. The JPEG boot-logo path is not a general video
implementation. Other media still need fixtures and playback tests.

Storage and boot workarounds
----------------------------

10. ATA transfer and error handling
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Source-confirmed gaps.** ``hw/ide/s5l8702-ata.c`` implements a limited
custom PIO/DMA command path alongside the IDE bus. It performs synchronous
block I/O inside device accesses. Read failures can become zero-filled
successful transfers; write return codes are ignored. Unknown commands,
abort/error status, FIFO state, transfer counts, timing, cancellation, and
flush behavior need a complete command/error model.

DMA strips bit 31 from addresses with ``& 0x7fffffff`` instead of relying
on the machine's address-space mappings. Guest DMA ranges and memory access
errors need validation. The current bounded sector count limits the normal
transfer buffer to 128 KiB, but that does not establish DMA safety or correct
error propagation. Backend errors must reach the guest before storage can
be considered reliable.

11. ATA identity and capacity
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Source-confirmed simplification.** IDENTIFY is fabricated, with a fixed
model/serial/geometry and LBA28 capacity capped at ``0x0fffffff`` sectors.
LBA48 is deliberately not advertised. The tested small disk does not prove
support for original large-capacity Classic disks, alternate sector sizes,
or all firmware storage modes.

12. Patched boot inputs and fused-key bypass
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Explicit workaround.** The launcher uses the supplied
``bootrom_patched.bin`` and patched NOR. The exact original-to-patched byte
changes in those pre-existing files have not been reconstructed in this
work. The current retailOS executable payload has not been instruction
patched to force the menu.

``-global s5l8702-aes.fused-key-bypass=on`` turns every non-custom key-source
transfer into an identity copy. Device-fused UID/GID keys are unavailable
from the inspected firmware inputs. This is not emulation of those keys
and cannot decrypt original encrypted input. The disk builder also replaces
the osos header tag at ``+0x40`` with ``SHA1(header[0:0x40])[0:16]`` for
this pre-decrypted setup; that is not original-device authentication.

13. AES and SHA beyond the tested paths
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Partial implementation.** Custom-key AES-128/192/256 CBC/ECB uses QEMU's
crypto API; the register ordering was checked in boot-ROM disassembly.
Completion is synchronous, AES interrupt signaling is absent, and auxiliary
buffer/control semantics and output-size enforcement remain incomplete.

SHA implements guest-padded PIO and DMA compression, continuation,
writable intermediate digest words, and IRQ 40. Original driver
``0x08081680`` writes source ``+0x84``, byte count ``+0x8c``, selects DMA
with ``+0x80 = 1``, and starts compression. ``0x08091bc0`` restores digest
words; ``0x0808748c`` saves them. A new NIST-vector test covers multi-block
DMA and context save/restore while another context uses the engine.

DMA reads use a fixed 64-byte buffer, validate the complete RAM extent,
and reject MMIO, wrapping addresses, and malformed lengths. The current
0xffc0-byte per-request cap follows the driver's chunking; the actual
hardware count-field width remains unverified. Completion is synchronous.
Busy timing, clock gating, auxiliary registers, other control modes, DMA
progress readback, hardware error flags, and reset during an active transfer
remain unimplemented or unverified. Rejected DMA preserves the digest and
does not generate a new completion; this is conservative model behavior,
not a decoded hardware error response.

14. Prepared disk versus complete restoration
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Explicit preparation; end-to-end restore unverified.** The working disk
is generated offline with firmware/resources and a 512 MiB FAT32 data
volume. It was not produced by a completed emulated iTunes restore. Its
firmware/data alignment and the unusual FAT marker were chosen from local
firmware checks and observed reads. Broader image/firmware compatibility
has not been established. The NOR hardware-version field is selected for
this firmware's checked minimum revision, ``0x00130200``.

15. Preferences and persistent settings
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Known incomplete behavior.** The initial Preferences file was captured
from the firmware's settings object after selecting English, then restored
offline to FAT. A fresh process reads it and starts configured. That proves
reading restored preferences, not automatic saving of later changes.
The normal dirty/flush/shutdown path still needs to save settings and
survive a new process. Generic filesystem write success is insufficient.

In the private-disk sleep test, changing Shuffle from Off to Songs and
holding Play reaches WFI without calling ``0x080636cc`` or the save call
in ``0x08063844``. After quitting that QEMU process, the on-disk Preferences
still matches the initial 2956-byte seed. This distinguishes shallow sleep
from a verified preference-flushing shutdown; it does not establish when
the real firmware is supposed to commit settings. The natural save trigger
and persistence across its shutdown path remain unverified.

Power, clocks, input, and remaining devices
-------------------------------------------

16. Sleep, wakeup, and power sequencing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Partly implemented.** The firmware's IRAM sleep routine ``0x22002dcc``
writes and polls SYSIC clock-control bits around WFI. Readback, PMU hold
input, and RTC alarm interrupts are modeled. Unlocking the hold switch
resumes the normal UI from WFI and restores menu input. The sleep alarm
also resumes firmware execution through the existing PMU interrupt route.
Power domains, oscillator switching, and ordinary button wakeup remain
incomplete. ``s5l8702-sysic.c`` retains guessed power behavior derived from
a different SoC. Automatic preference saving is also unverified.

A live Long Play trace reaches WFI at ``0x22002e70`` and remains at
``0x22002e74``. Sleep setup ``0x080d84e4`` saves interrupt configuration,
enables VIC0 sources 0, 3, and 19 (``0x00080009``), and disables VIC1.
The normal wheel source 23 remains raw-pending but masked. SYSIC group 3
bit 3 is configured as an active-low level for PMU GPIO 123; group 6 bit 28
is also enabled. The surviving group 0 bit 2 enable does not establish a
wake source because its VIC1 interrupt is disabled. No guessed connection
has been added for that bit or for the ``0x38e02000`` window.

Resume ``0x080d8e94`` reads the six PMU event bytes, decodes them through
``0x08362840``, and restores the saved interrupt configuration. INT6 bit 1
maps to event ``0x200``; ``0x080559b4`` handles it by reading GPIOSTAT bit 1
through ``0x082da704`` and changing wheel pin configuration. This hold path
now works through PMU GPIO2, INT6, GPIO 123, SYSIC and VIC0; no extra CPU
wake injection is used. Other external wake inputs and their electrical
connections still need implementation.

Further decompilation distinguishes possible external wake sources without
establishing their wiring. ``0x08362840`` maps INT2 bit 2 to event ``0x400``
and bits 0/1 to ``0x800``. Resume consumer ``0x0819a9c8`` responds to
``0x800`` by waiting 250 ms and reading GPIO 86 through ``0x080b2a84`` /
``0x080d837c``. This is headphone detection evidence, not grounds to wire
wheel buttons to ONKEY. The enabled group 6 bit 28 is GPIO 4, configured
for a falling edge; its physical wake source remains unconfirmed.

In an accelerated run (``-icount shift=4,sleep=off -rtc clock=vm``), the
firmware reads 00:46:49 and programs an alarm for 01:16:49. The PMU alarm
then fires, and resumed firmware reads INT1 ``0xc0`` and RTC 01:16:49.
That initial alarm run used no debugger or firmware patch. This establishes
alarm interrupt delivery and firmware resume; it does not establish a
complete power-state model or that a background alarm must light the UI.
A second trace stops at ``0x080ea1e8`` after the resume function returns:
its decoded event is exactly ``4``. The debugger in that run observes the
sleep entry and resume call; it does not single-step through WFI.

17. PMU, battery, hold switch, and RTC
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Partly implemented.** ``hw/misc/pcf5063x.c`` still reports fixed healthy
power status, ADC value 844, immediate conversion readiness, and no charger.
It has no battery/charger/conversion event model. INT registers clear on
read, and the output reflects pending unmasked events as an active-low
line connected to GPIO 123. The original firmware reads INT1..5 and INT6
at ``0x0836278c`` and programs their masks at ``0x0836280c``;
``0x080559ac`` masks GPIO 123 while its task handles the interrupt.

The hold switch is now a board input: machine property ``hold`` drives the
PMU's GPIO2 input, with low meaning locked. Either transition latches INT6
bit 1; GPIOSTAT bit 1 reports the actual input. Masking does not discard
an event, and reading INT6 releases its interrupt. Live firmware shows the
lock icon, ignores Select while locked, and restores input when unlocked.
An unchanged level does not create another event. Pin modes other than
the firmware's input configuration and physical debounce remain unmodeled.

The RTC now keeps writable BCD calendar counters, an independently writable
weekday, and alarm registers. It initializes from ``-rtc base`` and uses
QEMU's configured RTC clock (``-rtc clock=vm`` for deterministic tests).
Time/date writes load on the next one-second pulse; an alarm match latches
INT1 bit 6, and second pulses latch bit 7. The RTC/alarm registers and eight
GPM bytes survive a system reset. Only deadlines that can change a latch
or apply a pending write require a callback; other ticks are calculated
on access. Disabled and already-latched events do not cause perpetual
one-second callbacks.

The original driver ``0x083627c0`` reads seven bytes starting at ``0x59``
or ``0x60``. Alarm programming ``0x083628bc`` first writes year ``0xaa``
to prevent an intermediate match, then weekday ``7`` and the six BCD fields.
``0x08362840`` maps INT1 bit 6 to event ``4``. The date-setting entry
``0x0806390c`` uses a software offset for displayed time; its alarm branch
writes the hardware alarm after converting that offset. Register layout
and the invalid-year/weekday behavior agree with the related
`PCF50633 user manual, section 8.15
<https://www.freecalypso.org/pub/GSM/GTA02/PCF50633UM_6.pdf>`_.

Remaining RTC limits: pulse phase, partial-write carry ordering, and
retrigger behavior for a continuously matching partial alarm have not been
measured on the PCF50635. The model triggers on entering a match and holds
invalid time/date values without advancing until corrected. It does not
invent a burst-read snapshot latch. Crystal drift, backup-battery loss,
NoPower reset, and host-independent persistence of the battery-backed
state across QEMU process exits remain unmodeled. This is not a complete
PMU implementation.

18. Clock tree, timers, watchdog, and memory controller
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Clock propagation implemented; timing and coverage remain incomplete.**
The fixed 121.5 MHz PCLK has been removed. The clock controller now derives
CCLK/HCLK/PCLK from the selected oscillator or PLL, the system divider,
and the individual CPU/bus dividers. ECLK follows the source and two
divider fields in CLKCON4's upper half. QEMU clock inputs/outputs connect
these rates to the timer controller. Timer clock gating cancels deadlines;
changing a rate preserves elapsed integer and fractional counter ticks
before scheduling the next compare. Compare deadlines round up to QEMU's
nanosecond resolution. External clock 0 has no source; external clock 1
is connected to the 32768 Hz oscillator.

The original IRAM routine ``0x2200200c`` encodes source and divider fields;
``0x0835fe18`` programs HCLK/PCLK division. ``0x22002dcc`` saves and gates
clocks around WFI. Boot ROM ``0x200014f0`` / ``0x200015e4`` supplies further
PLL and bus-frequency calculations. The bootloader programs PLL2 PMS
``0x01002401``; the supplied retailOS image has source value 216 MHz at
``0x2200ad0c``. With OSC0 at 12 MHz this agrees with PLL2 divide mode.
The generic boot-ROM calculator's PLL2 case does not describe this later
setup correctly; its behavior must not be copied blindly. The separate
PLL2 divide-mode handling also agrees with the
`Rockbox clock driver
<https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/s5l8702/clocking-s5l8702.c>`_.

Live retailOS boot reaches the main menu with CCLK/HCLK/PCLK at
216/108/54 MHz during startup and 18/9/9 MHz in the checked idle state.
ECLK remains 12 MHz while awake; the original Timer E prescaler of 11
therefore gives 1 MHz. Long Play gates the timer clocks and reaches WFI;
hold-switch resume restores clock outputs and normal UI operation.
These are rates derived from firmware programming, not physical frequency
measurements. QEMU's ARM execution still uses ``-icount shift=4`` and is
not cycle accurate; the CCLK output does not rescale TCG execution.

PLLLOCK no longer returns all ones: it reflects enabled PLLs with usable
input/PMS settings. Locking is still instantaneous; PLLCNT delay, analog
lock loss and oscillator startup are unmodeled. ALT0/ALT1 inputs are
unconnected, and the auxiliary oscillator selector is incomplete. Unused
PMS encodings, clock-group restrictions and the interaction between bus
gating and external timer sources need hardware confirmation. The timer
controller consumes the clock rates; SPI now uses its input clock to honor
gating, but does not time serial transfers. I2S0 now uses gated PCLK and
the codec LRCK for its observed TX mode. I2C now honors its two PWRCON1
gates, but retains fixed active-byte timing. DMA and other devices retain
the timing/gating gaps recorded in their respective sections. Capture edges
and PWM outputs remain incomplete.

SWRCON recognizes the original firmware's ``0xaa5`` command; other nonzero
values are logged as unimplemented instead of all causing a reset. Its
implementation no longer inspects the firmware's image tag in guest RAM.
This does not establish all reset commands or their readback semantics.
RSTSR remains a register-file placeholder: reset causes and acknowledgement
semantics have not been established in the checked original boot paths.

The watchdog address range is an unimplemented device in the machine, despite
the existence of an empty watchdog source file. Boot ROM
``0x200000c0..0x200000c8`` writes ``0xa5`` to ``0x3c800000``.
Original retailOS reset routine ``0x0835d028`` writes zero there, then
writes the separate SWRCON reset command; updater routine ``0x08009aa8``
uses the same sequence. These accesses agree with a watchdog-disable
key and rearming before reset. They do not confirm the counter divider,
prescaler, counter-clear key, interrupt routing, or oscillator-start behavior
described for the older S5L8700. The S5L8702 watchdog still needs a model
supported by further evidence; the older specification has not been used
as a substitute for that confirmation. MIU remapping works, while
DRAM timing, self-refresh, and most MIU registers are only readback.

QEMU timers schedule device events in virtual time; their presence does
not imply an extra guest-visible timer peripheral. The current uses and
their limitations are:

.. list-table:: Event scheduling and remaining timing assumptions
   :header-rows: 1
   :widths: 18 32 50

   * - Device
     - Scheduled event
     - Remaining limitations
   * - Timer controller
     - Counter compare and overflow
     - Deadlines now follow the selected clock, divider and timer gate.
       Physical phase behavior, capture and PWM pins remain incomplete.
   * - LCD panel
     - TE synchronization signal to GPIO 55
     - 60 Hz is unmeasured; pulse width is zero and horizontal TE is absent.
       This timer does not copy frames.
   * - I2C
     - Byte-completion status and interrupt
     - Fixed 25 microseconds per byte, independent of the programmed divider.
       Clock gates pause this provisional duration. Slave effects and
       ACK/data are atomic at completion; bit-level timing, stretching,
       arbitration and alternate clock selection remain absent.
   * - I2S0 TX
     - Stereo frame consumption and subsequent DMA requests
     - Follows codec LRCK, with fractional progress and clock gating.
       Buffer capacity/request threshold is a transaction abstraction;
       bit edges, underrun status and stop latency remain unfinished.
   * - PMU RTC
     - Second pulse, deferred counter writes, and programmed alarm
     - One-second counter rate; oscillator phase, drift, partial writes and
       continuous-match retrigger behavior require hardware confirmation.
       Uses the configured RTC clock and skips callbacks for latched events.
   * - Clickwheel
     - State-report packets
     - Chosen 40 ms cadence, ten trailing reports after a button change,
       and 500 ms touch release. These are input-model approximations,
       not timing established from the original wheel hardware.

Removing these schedulers would not validate device behavior. Their event
causes, clock dependencies, cancellation rules, and deadlines need separate
verification. The GPIO bit-banged wheel still advances on register reads;
removing a ``QEMUTimer`` does not by itself remove a timing shortcut.

QEMU's `Clock API <https://www.qemu.org/docs/master/devel/clocks.html>`_
represents periods and propagates rate/gate changes; it does not generate
clock-pin edges. A device can use that period to calculate when its next
observable event occurs and schedule a virtual-time callback. For example,
a serial transfer's deadline should follow its remaining wire clocks and
the programmed divider, accounting for pauses and rate changes. The
firmware can establish register programming and observable sequencing;
undocumented oscillator rates, FIFO depths and physical signal timing
still need independent hardware evidence. A successful boot alone does
not establish those parameters.

19. Interrupt coverage
~~~~~~~~~~~~~~~~~~~~~~

**Partly implemented.** The old PL192 edge-glue device is deleted and
peripherals drive interrupt levels directly. An unimplemented register
window remains at ``0x38e02000``; the guest still writes acknowledgement
values at ``+8``. Its complete hardware semantics are not decoded.

PL192 priority/cascade behavior has qtests, but protection is still a TODO.
SYSIC now receives GPIO input changes, handles polarity, edge latching,
active levels, masking, and W1C acknowledgement, and recomputes its outputs
on configuration changes. The GPIO 55/VIC 1 display path is verified in
the original firmware. Group mappings used by firmware are 6->0, 5->1,
4->2, 3->3, and 0->33. Unused groups 1->32 and 2->31 follow the Rockbox
hardware definitions and are not independently exercised by this firmware.
The register conventions also agree with the
`Rockbox external interrupt driver
<https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/s5l8702/gpio-s5l8702.c>`_.
Other pin sources and PMU, LCD-controller, AES, SPI, and I2S interrupts retain
the gaps described above. Removing edge glue did not finish every IRQ source.

20. GPIO and clickwheel fidelity
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Register and input handling corrected; electrical behavior incomplete.**
The model implements all sixteen eight-pin ports with one register decoder.
Input-mode PDAT reads now use physical input levels in every port, including
port 15. Port 14 no longer unconditionally replaces the whole byte with
the synthetic wheel serial value, which used to lose unrelated pins.
PDAT writes update an output latch; changing PCON to output mode activates
that latch. Input-mode writes no longer drive the output line high.
Modes ``0xe`` and ``0xf`` set output-low/high and read back as mode 1.
GPIOCMD and PCON use the same configuration logic, with bounded port/pin
decoding and explicit little-endian, 32-bit MMIO accesses.

These paths are checked against ``0x083606d8`` (GPIOCMD configuration),
``0x083607f4`` (PDAT reads), and ``0x0836086c`` (PDAT writes). Sleep save
``0x0835eb10`` iterates sixteen ports, translating output mode 1 into
``0xe``/``0xf`` according to PDAT. Restore ``0x0835ead0`` writes the saved
pull-register bytes before restoring PCON. Recurring commands for pin
``0xc8`` are an absent-pin sentinel: ``0x080d7668`` initializes four
pin arguments to that value, and ``0x080c0794`` explicitly skips another
configuration call when its pin equals ``0xc8``. These writes do not
establish a physical port 25. The model logs and ignores them.

Pull registers remain readback storage. Floating/tri-state resolution,
alternate-function routing, electrical debounce, and GPIOCMD readback
semantics are unverified. Non-output pins currently drive zero on the
model's output connection; there is no resolved pin network. The retained
boot-time wheel serial workaround advances on PDAT reads only when the
four wheel pins have the expected GPIO modes. It affects only E3/E5 and
preserves E6/E7 and output bits, but still has no physical serial timing.

**Bootstrap button state now shares the normal input source.** The GPIO
bootstrap path previously had five button fields that never received
keyboard events. Its query replied with no buttons even while the regular
wheel controller reported Select pressed. The wheel input device now
distributes the five button levels to both protocol paths through named
QEMU connections. These connections carry internal input state; a physical
serial-bus model remains outstanding. Controller reset preserves held keys,
touch position, and pending host rotation. Separately held keyboard aliases
are tracked independently, and repeated key-downs do not postpone the first
event packet indefinitely.

Original EFI ``TouchWheel`` routine at module offset ``0x220`` transmits
``0xc000011d`` through GPIO E2/E4 while polling E3/E5, then accepts reply
``0x8000023a`` with button bits 16..20 in Select, Play, Previous, Menu,
Next order. The instructions at ``0x294..0x398`` implement the exchange;
``0x39e..0x3e6`` validates and decodes the reply. Those instructions match
the executing boot code at ``0x0befe220`` in a fresh private VM.
retailOS ``0x08362a9c`` decodes the same query-bit order through its regular
controller driver ``0x08362a0c``. Unsolicited event packets use a different
button order, which remains handled separately.

A regression reproduces the former missing Select bit, then checks every
button's press and release through both interfaces. Additional tests cover
simultaneous buttons, reset with held buttons/touch, independent aliases,
repeated key-downs and snapshot restoration. In the running original boot
driver, resetting with Select held produces raw reply ``0x8001023a`` and
decoded result ``0x01`` at ``0x0befe426``. No guest instruction or register
result was overridden. The new snapshot version records host aliases;
older snapshots retain only button state, so their original alias choice
cannot be recovered. Cross-version migration remains unverified.

The regular wheel has a bounded FIFO and working keyboard events, but
packet cadence, queue depth, rotation steps, and touch/release timing are
emulation choices. Long Play enters sleep; ordinary button wakeup and wider
repeat behavior need more work. The separate physical hold switch can be
tested through the machine's ``hold`` property, including lockout and wakeup.

21. SPI and NOR
~~~~~~~~~~~~~~~

**FIFO and receive accounting implemented; serial timing remains incomplete.**
The controller now has bounded TX/RX FIFOs and reports their actual occupancy.
Boot ROM ``0x20004b4c`` tests TX bits 8:4 against ``0x100`` (16 entries);
``0x20004af4`` polls RX bits 13:9. ``0x2000503c`` clears FIFOs with CTRL
bits 2/3. The supplied EFI SPI code also checks CTRL bit 1 when enabling
or stopping the controller. The model derives that read-only bit from
the enable state; clear commands do not remain latched in CTRL.

``0x20008f90`` initializes the receive limit. The first NOR header read
uses ``0x804`` at ``0x20002fd8``: four command/address bytes followed by
``0x800`` payload bytes. ``0x20004e5c`` sends those four bytes and then
sets SETUP bit 0 for automatic reception. The model now accounts for both
manual and automatic receive bytes, generates dummy TX bytes only while
automatic reception has a remaining budget, and stops at that limit.
Empty RX reads return zero without consuming more NOR data. A regression
reads a known pattern across FIFO boundaries, checks exhaustion, and
resumes at the next NOR byte after programming another receive limit.
It also saves/restores partway through reception, including buffered
bytes and the flash's prefetched address.

The clock controller drives separate SPI inputs through PWRCON1 gates
2, 11, and 15. Boot ROM ``0x20002fb4`` selects gate numbers ``0x22``,
``0x2b``, and ``0x2f``; ``0x2000147c`` clears the corresponding bits.
``0x20005088`` calculates a divider from the saved PCLK rate, populated
by ``0x200017ac``. A gated input preserves queued TX data and prevents
SSI transfers until the clock returns. All three ports have gate tests.

The original decrypted updater independently corroborates the SPI setup.
Its first-stage ``0x0800fb84`` stops CTRL, writes SETUP with bit 3 set,
programs the eleven-bit CLKDIV, sets SETUP bit 4, and enables CTRL.
``0x0800fd68`` uses this routine with divider 8; the embedded diagnostic
``0x08017040`` instead writes CTRL 3, SETUP ``0x1e``, and divider 2.
These are separate programs with overlapping addresses. The controller
now requires both SETUP bits 3/4 for manual and automatic master transfers,
preserving the FIFO and receive budget while either is clear.
All three ports have control tests; a NOR test suspends automatic reception,
drains buffered bytes without refilling, snapshots/restores, and resumes
at the correct flash address.

The names ``MASTER`` (bit 3) and ``SCKEN`` (bit 4) follow Samsung's
`S5L8700X preliminary specification, chapter 19
<https://files.freemyipod.org/misc/S5L8700X-DS.pdf>`_. Their use in the
S5L8702 model is an inference corroborated by Apple's sequence, not an
independent test of either bit on physical S5L8702 hardware. The checked
original code always enables both for transfers. Slave operation and
behavior when changing these controls mid-byte remain unverified.

Transfers still complete synchronously when FIFO space permits. The exact
divider encoding and byte duration, alternate clock sources, clock phase,
CPOL/CPHA, non-byte transfers, slave mode, and pin-control effects remain
unmodeled. CLKDIV and PIN have register readback; they do not establish
physical serial timing or pin routing. RX capacity is provisionally 16,
matching TX; the checked driver establishes its count field but not its
maximum occupancy. Stalling TX on a full RX FIFO, discarding writes to a
full TX FIFO, the zero underflow value, and receive-limit readback are
model choices needing hardware confirmation. Overrun/underrun event
causes, IRQs, and DMA requests remain absent. Filling/refilling the FIFO
in an MMIO or clock callback is not a cycle-accurate transfer model.

The ROM's ``0x20005088`` divides the saved PCLK rate by its caller's
argument, and ``0x20008f90`` passes 6,000,000 before storing the quotient
in CLKDIV. There is no subtraction or factor of two in this software path.
That establishes the programmed value, not the actual wire rate: the
argument might specify an upper bound. The original updater's constant
dividers do not resolve this ambiguity. `Rockbox's SPI driver
<https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/s5l8702/spi-s5l8702.c>`_
calls its
``PCLK / (div + 1)`` formula ``TBC``; Samsung's older S5L8700 manual gives
``PCLK / (2 * (div + 1))`` with a different register layout. Neither
formula is established for this S5L8702 by the checked Apple code.
Serial scheduling remains unfinished pending a supported divider model.

Reset clears registers, both FIFOs, and the receive budget. VMState saves
them and the input clock; post-load validates FIFO bounds and does not
transfer before other devices have restored their state. The board uses
QEMU's existing SST flash model. Successful NOR boot and a snapshot read
do not validate firmware updates, protection, or all write/erase flows.

22. USB, DFU, disk mode, and iTunes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Substantial workarounds; end-to-end behavior unverified.** The USB model
includes synthetic reset/enumeration/setup events, including enumeration
after a number of idle register polls and an always-connected high-speed
host-port status. The PHY is largely a register file. ``IPOD_DFU`` and
``IPOD_USB_CONNECTED`` inject board state through environment variables.

The model starts a USB/IP listener on localhost port 3240 as part of
realization, without an explicit backend option. USB transport/backend
configuration and actual cable/reset/endpoint events need a proper model.
Normal menu startup does not verify DFU restore, disk mode, iTunes syncing,
disconnect/reconnect, or transfer error handling.

23. RNG, chip ID, NAND, ECC, and serial accessories
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Source-confirmed simplifications; several paths untested.** The RNG
returns a deterministic linear-congruential sequence and fixed ready bits;
their individual meanings are not fully decoded. Chip ID supplies a fixed
revision at one register. NAND defaults to operation without backing files
and has unsupported commands that call ``hw_error``. ECC reports immediate
success and raises completion without calculating error-correction data.
These NAND paths are not needed for the demonstrated ATA boot.

The UARTs now use a separate S5L8702 device. The earlier iPod-specific
UTRSTAT changes have been removed from the shared Exynos4210 model. Original
retailOS ``0x08363120`` selects four ports at ``0x3cc00000 + port * 0x4000``;
the updater diagnostic initializer ``0x08018620`` initializes those same
four ports. Its generic table contains a fifth descriptor, but that alone
does not establish a fifth physical UART. The unsupported fifth Exynos
instance and the dependency on ``CONFIG_EXYNOS4`` are gone.

The register interface is based on the following original firmware
decompilation and disassembly:

* retailOS ``0x080b0b14`` waits for ``UFSTAT & 0x2f0`` to become zero before
  writing at most 16 bytes. The updater RX handler ``0x0801a274`` decodes
  a low four-bit count and adds 16 for bit 8. The model has bounded 16-byte
  RX/TX queues and separate full flags in bits 8/9.
* Updater ``0x0801b270`` and ``0x0801b384`` enable RX/TX interrupts with
  UCON bits 12/13. retailOS ``0x08362f50`` loops over ``UTRSTAT & 0x178``
  and acknowledges events by writing the corresponding status bits.
  Acknowledgement now clears the latched event without an immediate
  reassertion merely because unread data remains. Reading RX data does
  not substitute for acknowledging the interrupt.
* Updater ``0x0801b42c`` programs the RX/TX trigger fields in UFCON bits
  4/6 and resets the queues with bits 1/2. The model makes the reset bits
  self-clear, consistent with the related S5L8700 register description.
* retailOS ``0x08362f50`` treats status bit ``0x100`` as automatic baud
  detection, reads its count at ``+0x2c``, and writes baud/fine-tuning
  registers at ``+0x28/+0x34/+0x38``. The latter two retain values such as
  ``0x000cc330``; they are not Exynos interrupt-pending/mask registers.
* Updater ``0x0801ab30`` enables gate ``0x27`` through ``0x08005e10`` and
  ``0x080164f4``, clearing PWRCON1 bit 9. The modeled gate now stops byte
  delivery and resumes queued TX data when opened.

**UART TX serialization and receive timeout are now clocked.** The original
updater ``0x0801b8fc`` computes a divisor from clock rate, requested baud and
sample count, and stores ``16 - sample count`` above UBRDIV's low 16 bits.
``0x0801b6c4`` selects the source with UCON bit 10. The model now has PCLK
and UCLK inputs and a separate TX shift register. retailOS ``0x080d00c4``
waits for UTRSTAT bit 2 independently of its FIFO-empty polling. UTRSTAT
bits 1/2 now distinguish an empty TX queue from a completely idle
transmitter. A frame consumes source-clock cycles derived from its divisor,
data bits, parity and stop bits. The virtual timer schedules the next frame
completion or receive-idle deadline; it does not periodically inject IRQs.
Clock gating pauses the remaining cycles, including partial cycles, and a
frequency change resumes them at the new rate. Format/divisor values are
latched when a frame enters the shift register.

Completed TX bytes enter a separate, bounded 4096-byte host output queue.
Nonblocking backend writes and writable watches drain that queue without
changing the hardware FIFO or shift-register state. A stalled backend no
longer changes guest-visible TX timing. Once the host queue fills, further
completed bytes are discarded and counted in the read-only
``dropped-output`` QOM property. An absent/disconnected backend discards
output. These are explicit transport limitations, not hardware FIFO
capacities or UART error flags. RX remains a completed-byte backend
abstraction: bytes arrive at host speed, and backend backpressure substitutes
for physical wire overruns. The host serial port's framing/baud parameters
are not configured by this model.

**Several timing details remain hypotheses.** retailOS initializes
UCON=``0x405``/UBRDIV=12 and later uses automatic baud counts from 185 to
1375, with divisor/fine-tuning choices compatible with conventional rates
from a 12 MHz input. This does not prove the oscillator wiring. UCLK is
connected to the board's 12 MHz OSC0 as a provisional connection, also
identified as uncertain in Rockbox's
`serial-6g.c
<https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/s5l8702/ipod6g/serial-6g.c>`_.
The original updater directly confirms the PCLK rate parameter, sample-count
formula and clock-select bit, but its alternate-clock rate global has no
identified initialization. The UART now consumes clock inputs rather than
using the Exynos model's unrelated fixed 24 MHz rate.

The fine-tuning pattern applies a one-sample stretch/shortening to individual
bits according to the related
`UC87xx driver
<https://github.com/Rockbox/rockbox/blob/master/firmware/export/uc87xx.h>`_.
A nonempty RX FIFO below its trigger produces a receive-timeout event after
three frame times, following the S5L8700 description. UCON bit 7 enables
timing and bit 11 masks the IRQ; UTRSTAT bit 3 is write-one-to-clear. Mask
changes do not restart the counter. New RX activity or a read with data
remaining restarts it; acknowledgement alone does not repeat the timeout.
Firmware confirms these register fields and uses the event, but the exact
idle period, restart behavior and fine-tuning waveforms have not been
established on an S5L8702. Likewise, arbitrary mid-frame programming,
FIFO-reset preservation of the shifter, disabling TX mid-frame, the
one-byte non-FIFO holding register, loopback, reset defaults, exact trigger
levels and non-8N1 formats remain related-hardware inferences. Tests of
these behaviors establish model consistency, not physical measurements.

Autobaud edge detection, modem signals, GPIO multiplexing, per-byte error
FIFOs, parity/framing errors, DMA and infrared remain absent. FIFO mode
changes with queued data, reserved-bit masks and narrower MMIO accesses need
further investigation. Overrun/break status is a single read-to-clear latch.

Nine qtests cover the four IRQ routes, masking/acknowledgement, FIFO bounds,
real backend RX, exact frame deadlines, clock selection and changes,
fractional-cycle gate/snapshot restoration, idle timeout and its snapshot,
format/divisor changes and host-backpressure isolation. The deliberately
full socket test exhausts the bounded output queue while the transmitter
continues completing frames. Resettable and VMState version 2 retain the
active frame, remaining cycles, input clocks, queues and error/IRQ state;
post-load validates bounds and reconstructs the deadline. Version 1 states
start their previously untimed queued TX at the restored virtual time;
whole-machine migration and cross-version migration remain unverified.
Snapshots containing the former Exynos UART instances are incompatible
with this replacement.
Dock/accessory protocols, headphone insertion events, remote controls and
accessory detection have not been validated with connected hardware models.

QEMU engineering and validation debt
------------------------------------

24. Reset, migration, and resource lifetime
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Source-confirmed.** Several devices have no VMState, and ATA/USB do not
serialize their complete state. The PMU now serializes registers, I2C
pointer/transaction state, hold input and pending RTC writes. Post-load
reconstructs its RTC deadline and IRQ output; full migration is untested.
GPIO now serializes all port registers, external input levels, and its
remaining serial-workaround state. Reset clears registers and serial state
while retaining external pin levels. Reset hold and post-load resynchronize
output lines and the GPIO-to-SYSIC inputs. A savevm/loadvm regression
checks GPIO register, input, pull-register and output-line restoration.
The clock controller serializes registers and input clocks, reconstructing
its outputs without calling other devices during post-load. Timers save
their input periods and fractional tick state. A snapshot test checks
restoration halfway through a tick and subsequent frequency changes.
The timer VMState version is now 2 and rejects the old version 1 snapshots,
which lack those fields; no cross-version machine migration is claimed.
I2C uses VMState version 3, storing the pending byte's data, direction,
ACK configuration, input clock and suspended duration. A snapshot during
transmission restores the PMU register pointer and the remaining deadline;
a suspended byte remains suspended until its clock is enabled. Earlier
versions are rejected because they lack clock-gated state, and version 1
had already executed the slave operation.
SPI now serializes its FIFOs, receive budget, registers, and input clock.
Tests restore queued data after reset and resume a NOR read through the
flash's saved protocol state. This does not establish whole-machine or
cross-version snapshot compatibility.
The codec's active/frozen clock controls and MCLK input were introduced in
VMState version 2; the current transition model uses version 4 as described
below. Post-load rebuilds LRCK without invoking consumer callbacks.
A snapshot test restores pending controls separately from the active rate
and checks subsequent source changes. The codec has a named machine child
path and rejects the earlier version 1 state; cross-version compatibility
is not provided.
PL080 uses version 2 state, retaining the version 1 prefix and adding FIFO
contents, pending request counts, request levels and acknowledgements.
Version 1 input defaults those previously absent fields to empty state;
cross-version whole-machine compatibility is not established. Reset hold
and post-load recompute interrupt outputs, and the transfer callback is
cancelled on reset/unrealize and deleted at finalization. Unrealize destroys
the downstream address space. Snapshot regressions check a pending error
interrupt and preservation of already-fetched data while the source RAM
has subsequently changed. Full migration of a connected peripheral/DMA
handshake remains untested.
Whole-machine save/load or migration is not supported by the evidence.
Unsupported devices should be marked unmigratable until implemented.

The ARM CPU was absent from the global reset traversal because it has no
parent bus. A system reset cleared peripherals while leaving it at the
previous WFI instruction. The SoC now explicitly registers the CPU with
QEMU's Resettable reset adapter and unregisters the callback on unrealize.
A small TCG test ROM counts reset-vector entries, remaps IRAM over address
zero, and executes WFI. Paused and running resets now restore the boot ROM
mapping and restart execution; the pre-fix binary fails this regression.
The same test checks restart through SWRCON, using the ``0xaa5`` write to
``0x3c500050`` in the original reset routine ``0x0835d028``. The clock
model's reset-cause flags and other SWRCON values remain unverified.
The retail firmware also returns to its normal main menu after a paused
system reset. This verifies that path, not all device reset semantics.

LCD reset now uses fixed arrays and resets the active implementation state;
it no longer leaks allocations. LCD registers, GRAM, command state, and TE
timer are serialized, but whole-machine save/load remains unverified.
ATA buffers still lack a device finalizer. Pending IRQ/timer state and
output-line resynchronization require a machine-wide warm-reset audit.
Repeated resets can still preserve stale state in other devices; low build
concurrency does not address runtime leaks.

25. Bounds, APIs, coding conventions, and test coverage
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Partly corrected engineering debt.** LCD command indices and pixel windows
are bounded. Display DMA validates RAM ranges and MemTxResult; qtests cover
malformed windows, oversized commands, and MMIO used as a framebuffer.
Other DMA devices still ignore MemTxResult or can access arbitrary MMIO.
PL080 now checks transfer results, uses a fixed sixteen-byte FIFO per
channel, bounds work per callback, and rejects recursive writes to its own
controller. Further malformed-state and cross-device DMA review remains.
USB/IP lengths and teardown need a bounded-input and lifetime review.

The port mixes modern QOM/input/crypto/Resettable use with legacy reset
hooks, native-endian MMIO, environment-variable configuration, printf
debugging, hardcoded guest memory knowledge, and inconsistent formatting.
There are large commented-out code blocks. The shared PL080 change needs
particular care because it affects other machines. This is not yet an
upstream-ready device series or a completed modernization of the QEMU base.

The sixty-two qtests cover boot-related register/IRQ paths, selected planar
composition/blending, LCD bounds/GRAM behavior, TE signalling, GPIO
interrupt masking/polarity/acknowledgement, hold input, and RTC/alarm
behavior. New RTC cases include delayed writes, leap and non-leap rollover,
year 99 to 00, weekday independence, reset retention, invalid alarm dates,
wildcards, and delivery through PMU/GPIO/SYSIC/VIC to the CPU. All pass,
including new coverage for every GPIO port, output latches/configuration,
GPIO snapshot restoration, and CPU restart through the boot ROM from WFI.
Clock cases check PLL selection, bus/ECLK division, clock gating, retention
of a fractional tick during frequency changes, and snapshot restoration.
I2C cases check deferred ACK/data and read-to-clear effects, cancellation
by STOP/disable/reset, deadline resolution, both clock gates, paused-byte
retention, and running/suspended byte snapshots.
SPI cases cover FIFO accounting/clearing, NOR receive limits, empty reads,
all three clock gates and SETUP controls, reset, and controller/NOR snapshot
restoration, including suspended automatic reception.
I2S cases check stop acknowledgement, re-enable/reset, DMA-before-TX
ordering, payload/channel order, frame deadlines, clock pauses/rate changes,
and restoration of buffered samples and fractional phase.
Codec cases check MAP addressing, documented reset controls, rate/divider
and gate changes, freeze/unfreeze, and restoration of pending controls.
Playback cases cover gain boundaries, mute/routing/power, HPDETECT, line
output, frozen volume snapshots and bounded host backpressure.
Transition cases exercise ramps and independent analog crossings, including
clock pauses, frequency changes, timeout progress and pending snapshots.
PL080 cases cover all nine supported width combinations with fixed and
incrementing addresses, completed-versus-next descriptor interrupt bits,
LLI bus selection, bus errors at all three access sites, interrupt masks,
reserved widths, unimplemented flow modes, reset and snapshot IRQ state.
Request cases cover absent/held requests, clearing all request lines before
another acknowledgement, software single/burst latches, final short
destination bursts, packing across single requests, destination-based
count readback, halt/drain, FIFO snapshot contents, circular descriptor
list responsiveness and recursive controller writes.
The TVO case covers register retention, stop acknowledgement, reset and
snapshot restoration, plus bus errors outside the mapped control registers.
They do not establish functional completeness. Builds use
``ninja -j2``. The prior FAT check verifies one generated volume after the
tested writes. Missing validation includes wider display modes, full gameplay,
game loading from verified original plaintext,
real host device backpressure and long-duration audio drift,
storage backend errors, settings across reboot, sleep/wake, USB restore,
warm reset, migration, malformed MMIO/DMA, and long-run resource usage.

Reverse-engineering coverage is also unfinished. Some inherited comments
label reference-version behavior "VERIFIED" but give addresses from older
firmware. New behavior must be checked against the local relocation and
executable, as was done for the display and loader findings above.

Local audit evidence and next steps
-----------------------------------

The audit VM used the normal launcher with ``-snapshot`` so game attempts
and navigation did not write back to the restored disk/NOR. Ghidra was run
one process at a time. Peripheral builds use ``ninja -j2``; all seventy-six
current qtests pass.

The paths below identify local investigation artifacts in the surrounding
workspace. Generated firmware images, decompiled Apple code and raw runtime
captures are not packaged in the source repository. The machine guide,
firmware hashes, function addresses, portable analysis tools and qtests
provide the starting points for reproducing these observations.

Additional local evidence:

* ``ipod-work/audit-game-launch.png``: iPod Quiz launch error.
* ``ipod-work/audit-klondike-launch.png``: Klondike launch error.
* ``ipod-work/audit-game-gdb.log``: loader return values.
* ``ipod-work/audit-games-decompiled.c`` and
  ``audit-games-verify.c``: selected original-firmware functions.
* ``ipod-work/audit-display-decompiled.c``: current-version display routines.
* ``ipod-work/audit-black/`` and ``audit-black.log``: paused display registers,
  source buffers, and screenshot.
* ``ipod-work/audit-about-frames/``: black-to-complete transition sequence.

Post-audit evidence includes ``ipod-work/display-blend-decompiled.c``,
``display-timing-decompiled.c``, ``panel-te-firmware-trace.log``,
``panel-te-about/``, and ``panel-te-qtest.log``. The compositor and GPIO TE
submission changes were checked against the original firmware and the
normal ROM/NOR/OS boot. The reported About background failure is fixed in
the reproduced path. Game verification and decryption now succeed; the
separate damaged-image condition is described in item 2.

RGB565 crop evidence is in ``ipod-work/display-transitions/``:
``original-display.c`` and ``crop-disassembly.txt`` contain the original
firmware's address/offset calculation; ``crop-before.log`` records the
regression test failing on the previous compositor. ``full-qtest.log``
records all seventy-three passing tests after the fix. ``fixed-main.png``
shows a fresh boot of the original-input disk to the normal UI;
``original-input.txt`` records the unchanged firmware hash and damaged
library marker. These tests used ``-snapshot`` and a silent audio backend.

Wheel input evidence is in ``ipod-work/wheel-input/``: ``before.log``
reproduces the missing bootstrap button; ``retail-original.c`` and
``efi-thumb.c`` contain the decompiled routines, with original Thumb
instructions in ``efi-touchwheel.asm``. ``bootstrap-gdb.log``
and ``bootstrap-dram.bin`` locate the matching EFI instructions in the live
boot. ``held-bootstrap.log`` records the original driver's decoded Select
result across reset. ``full-qtest.log`` records all seventy-six passing
tests. Watchdog access evidence is in ``ipod-work/watchdog/``; this
investigation has not replaced the unimplemented watchdog.

Game/SHA evidence includes ``ipod-work/game-digest-reconstruction.log``,
``game-verifier-after-sha.log``, ``sha-dma-driver.c``,
``game-transform-trace.log``, ``game-module-trace.log``,
``game-marker-experiment.log``, ``game-jit-writer.log``,
``game-jit-emitter.c``, ``original-signature-verification.log``, and
``sha-dma-qtest.log``.
``klondike-marker-menu.png`` is guest output from the explicitly corrected
snapshot experiment, not an unmodified-input result.
``klondike-cmp-rd-deal.png``, ``klondike-cmp-rd-draw.png``, and
``klondike-cmp-rd-move.png`` show the subsequent deal, stock draw, and wheel
selection after the CPU decoding change. All fourteen peripheral qtests
and both CPU-model runs pass. SHA and CPU patches pass checkpatch without
errors or warnings.

Remaining priorities include verified recovery of the OS image's tail,
actual display timing, DMA handshakes, audio, persistence, and wakeup.
The complete 25-area audit has not been resolved.

Sleep/I2C evidence is in ``ipod-work/sleep-persistence/``:
``sleep-entry.log``, ``wake-pmu.c``, ``i2c-driver.c``, ``i2c-codec.log``,
``pmu-boot.log`` (before the STOP fix), ``i2c-fixed-boot.log`` (after),
and ``i2c-fixed-ui.png``. These runs use private writable disk/NOR copies.
``ipod-work/i2c-stop-baseline.log`` records the failing regression;
``i2c-stop-qtest.log`` records all fifteen passing peripheral tests.

Hold/RTC evidence in that directory includes ``rtc.c``, ``hold-on.log``,
``hold-on.png``, ``hold-blocked-key.png``, ``hold-unlocked-key.png``,
``hold-sleep-entry.log``, ``hold-wake-off.png``, ``pmu-rtc-boot.log``,
``rtc-initial-ui.png``, ``rtc-fast-boot.log``, and ``rtc-sleep-wake.log``.
``rtc-final-ui.png`` and ``rtc-final-music.png`` show a fresh default-clock
boot and working Select input (300 ms press) after the RTC changes. The
locked and blocked-key screenshots are byte-identical.
``ipod-work/pmu-rtc-baseline.log`` records failure of the pre-RTC model on
the new second-event regression;
``pmu-rtc-qtest.log`` records the eighteen passing tests. RTC code and
hold-input code both pass checkpatch without errors or warnings.

GPIO/reset evidence includes ``sleep-persistence/gpio-driver.c``,
``wake-dispatch.c``, ``button-wake.c``, ``gpio-high.log``,
``cpu-reset-check.log``, ``cpu-reset-initial.png``, ``cpu-reset-music.png``,
and ``cpu-reset-reboot.png``. ``ipod-work/gpio-data-baseline.log`` and
``gpio-output-baseline.log`` show the previous input/output failures;
``gpio-registers-qtest.log`` records the twenty-one tests after GPIO work.
``cpu-reset-baseline.log`` demonstrates the missing CPU reset;
``cpu-reset-qtest.log`` records all twenty-two passing tests after its fix.
``sleep-persistence/cpu-reset-hold-check.log`` records a subsequent Long
Play sleep at ``0x22002e74`` and hold-switch resume. The matching
``cpu-reset-hold-wake.png`` and ``cpu-reset-hold-music.png`` show the main
menu and working Select input. ``gpio-pull-fixed-reset.log`` observes the
earlier bootloader setting PUNB14 bit 6 and clearing PUNC14 bit 6 at
``0x2200e608..0x2200e614``; this establishes its register programming,
not electrical pull behavior. GPIO and CPU-reset code pass checkpatch
without errors or warnings. ``cpu-reset-swrcon-qtest.log`` additionally
checks software-reset restart. Both RST documents are checked together
with Sphinx, treating warnings as errors.

Clock evidence includes ``sleep-persistence/iram-clock.c``,
``bootrom-clock.c``, ``iram-clock-config.asm``, ``clock-driver.asm``,
``clock-awake.log`` (before the clock model), ``clock-tree-boot.log``,
``clock-tree-live.log`` and ``clock-tree-wake.log``. Screenshots
``clock-tree-ui.png``, ``clock-tree-music.png`` and ``clock-tree-wake.png``
show normal boot, Select input and resume with the new model.
``clock-tree-reset.log``, ``clock-tree-reset-result.log`` and
``clock-tree-reset.png`` verify a subsequent system reset to the main menu.
``ipod-work/clock-tree-baseline.log`` fails against the earlier binary;
``clock-tree-qtest.log`` records all twenty-four passing tests.
The clock/timer changes pass checkpatch without errors or warnings.

I2C completion evidence includes
``sleep-persistence/i2c-completion-original.asm`` and the earlier
``i2c-driver.c`` decompilation. ``ipod-work/i2c-completion-baseline.log``
and ``i2c-cancel-baseline.log`` record the early NACK and cancelled-write
failures. ``i2c-completion-qtest.log`` records all twenty-seven passing
tests, including pending-byte snapshot restoration.
``sleep-persistence/i2c-completion-boot.log`` traces a fresh ROM/NOR/OS
boot and codec transfers to address ``0x4a``. ``i2c-completion-ui.png``
and ``i2c-completion-music.png`` show the normal main and Music menus.
``i2c-completion-wake.log`` records Long Play reaching WFI at
``0x22002e74`` and hold-switch resume; ``i2c-completion-wake.png`` shows
the restored Music menu. These runs use private writable disk/NOR copies,
with no debugger attached or guest instruction/data corrections.
The I2C changes pass checkpatch without errors or warnings; both RST
documents pass Sphinx with warnings treated as errors.

Further I2C clock evidence is in ``sleep-persistence/i2c-clock-setup.asm``
and ``efi-smbus.asm``. The updater diagnostic is now independently mapped
by ``ipod-work/i2c-clock/extract-aupd-diag.py``; its input and extracted
section hashes are recorded in ``input-hashes.json`` in that directory.
``aupd-i2c-original.asm`` and ``aupd-i2c-original.c`` contain original
disassembly and Ghidra decompilation of the two controllers' setup and
gate dispatch. ``gate-baseline.log`` records the old completion while
disabled; ``gate-qtest-final.log`` records all sixty passing tests.
``audio-fixture/i2c-gates-boot.log`` and ``i2c-gates-live.log`` record the
normal firmware boot, Long Play WFI at ``0x22002e74`` with PWRCON1
``0xffffeff7``, and hold-switch wake restoring ``0x0003ed49``.
``i2c-gates-ui.png``, ``i2c-gates-playing.png`` and ``i2c-gates-wake.png``
show the main, Now Playing and resumed Songs screens. This run used private
disk/NOR copies and no debugger or guest data corrections.

SPI evidence is in ``bootrom-spi.asm`` and
``bootrom-spi.c`` (supplied patched ROM disassembly and Ghidra decompilation),
plus ``efi-spi.asm`` and ``efi-nor.asm``. ``ipod-work/spi-fifo-baseline.log``,
``spi-nor-baseline.log`` and ``spi-gate-baseline.log`` demonstrate the
previous fabricated FIFO counts and transfers while disabled/gated.
``spi-fifo-qtest.log`` records all thirty-one passing tests;
``spi-nor-state-qtest.log`` additionally checks the NOR snapshot case.
``sleep-persistence/spi-fifo-boot.log`` records normal ROM/NOR/OS startup;
``spi-fifo-live.log`` records menu input, Long Play sleep and hold-switch
wake. ``spi-fifo-ui.png``, ``spi-fifo-music.png`` and ``spi-fifo-wake.png``
show the main menu and working Music menu before/after sleep.
``spi-fifo-reset.log`` and ``spi-fifo-reset.png`` verify a subsequent
system reset through the ROM/NOR path back to the normal main menu.
The SPI changes pass checkpatch without errors or warnings. Both RST
documents pass Sphinx with warnings treated as errors.

``ipod-work/spi-control/`` adds disassembly and Ghidra exports of SPI
functions in the original decrypted updater's first stage and diagnostic,
with the first-stage extraction mapping and source hash in
``input-hashes.json``. Ghidra's inferred calling conventions miss some
preserved registers; the adjacent assembly establishes arguments and
literal MMIO addresses. ``control-baseline.log`` reproduces transfers with
SETUP controls clear; ``control-qtest.log`` records all sixty-one passing
tests after the fix. The first-stage and diagnostic Ghidra processes ran
sequentially with a 2 GiB heap and two-CPU limit.
``audio-fixture/spi-control-boot.log`` and ``spi-control-live.log`` record
normal startup, Music navigation, Long Play sleep at ``0x22002e74``, and
hold-switch wake. ``spi-control-ui.png``, ``spi-control-music.png`` and
``spi-control-wake.png`` show the resulting screens. This run used private
disk/NOR copies with no debugger or guest corrections; the private VM's
terminal exit was verified afterward. Both original firmware hashes remain
unchanged. The single empty-RX diagnostic also appears in the preceding
I2C-gate boot log and remains uninvestigated.

I2S evidence is in ``sleep-persistence/i2s-original.asm`` and the Ghidra
exports ``i2s-original-iram.c`` and ``i2s-original-dram.c``. The disassembly
also resolves register preservation that the decompiler's inferred calling
conventions miss. ``i2s-stop-baseline.log`` reproduces the absent CLKCON
acknowledgement; ``i2s-stop-qtest.log`` records all thirty-two passing tests.
``i2s-stop-ui.png`` records a fresh normal boot with the changed device.
``i2s-stop-live.log`` records Long Play reaching WFI at ``0x22002e74``;
``i2s-stop-wake.png`` shows the Music menu after hold-switch wake. No
debugger was attached and no guest instruction or data correction was applied.
The I2S patch passes checkpatch without errors or warnings. Builds use
``-j2``; decompilation uses one Ghidra process with a 2 GiB heap and two CPUs.

Codec evidence includes ``sleep-persistence/codec-original.c`` (Ghidra),
``audio-boot-gdb.log`` (original codec writes), and
``audio-fixture/codec-clock-original.asm`` (original codec and clock-group
programming). ``audio-fixture/codec-control-baseline.log`` reproduces the
incorrect power-down reset state. ``codec-qtest.log`` records all thirty-five
passing tests, including control-port addressing, rate/gate changes and
restoration of frozen settings. ``codec-checkpatch.log`` reports no errors
or warnings. ``codec-ui.png`` and ``codec-now-playing.png`` show a fresh
ROM/NOR/OS boot and selection of the signed WAV fixture, without an attached
debugger or guest instruction/data corrections. ``codec-live.log`` records
the derived LRCK rate and unchanged DMA stall. This run retains the
documented boot-ROM and fused-key workarounds. The two-job build succeeds;
the unrelated nano3g machine still emits two unused-variable warnings.

The fixture's ``hash58-original.asm``, ``hash58-gdb.log`` and
``hash58-console.log`` record the original signing path and identity input.
``make-library.c`` and ``tone.wav`` supply the private test library.
``playback-gdb.log`` records peripheral DMA setup; ``dma-stalled-state.log``
captures its later channel and descriptor contents. No firmware database
verification bypass was added. These files are local investigation artifacts;
they do not add Apple firmware or a restored disk to the repository.

PL080 evidence is in ``audio-fixture/dma-original.asm`` and
``dma-original.c`` (Ghidra), with the enable/descriptor ordering captured in
``dma-order-gdb.log`` and ``dma-order-console.log``. The four
``pl080-*-baseline.log`` files reproduce transfer-width, descriptor-IRQ,
bus-error, and unsupported-flow failures against the preceding binary.
``dma-display-bus-qtest.log`` records all forty passing tests.
``display-aux-lifecycle.c`` and ``display-aux-original.c`` decompile the
second display driver; ``display-aux-call.log`` records its original boot
call without modifying guest memory or registers. The restricted mapping's
fresh ROM/NOR/OS boot is recorded in ``dma-display-bus-boot.log`` and
``dma-display-bus-ui.png``; the supplied boot-ROM and fused-key workarounds
remain enabled. ``dma-display-bus-live.log`` and
``dma-display-bus-now-playing.png`` confirm menu navigation and selection
of the signed WAV fixture, with the audio DMA still stalled at count zero.
This does not validate external display operation or audio playback.

Request-handshake evidence includes ``audio-fixture/i2s-order-gdb.log``
and ``i2s-stream-original.c`` (Ghidra) for the original setup/enable order.
The first new request and FIFO test cases fail against the preceding
bypass binary in ``pl080-requests-baseline.log`` and
``pl080-fifo-state-baseline.log``. ``dma-count-readback-baseline.log``
reproduces the subsequent source-versus-destination counter error.
``dma-request-qtest-final.log`` records all forty-five passing tests.
The manual is saved locally as ``pl080-ddi0196g.pdf``; sections 2.4,
3.4, 3.8 and appendix B supply the FIFO, counter and handshake definitions.
This work does not establish the separate S5L8702 I2S FIFO or its serial
timing.

The request model's normal boot and signed WAV selection are recorded in
``audio-fixture/dma-request-final-live.log``, ``dma-request-final-ui.png``
and ``dma-request-final-now-playing.png``. That earlier build remained at
0:00 with DMA retaining its 2048-halfword count while waiting for the then
absent I2S request source. The later transport evidence below supersedes
that particular stall; audible output remains unfinished.

TVO shutdown evidence includes ``audio-fixture/tv-sleep-original.c`` and
``tv-sleep-ghidra.log`` for the original routines above.
``tvo-control-baseline.log`` reproduces the previously unmapped control
write; ``dma-tvo-qtest.log`` records all forty-six passing tests.
``dma-tvo-live.log`` records Long Play reaching WFI and hold-switch resume;
``dma-tvo-ui.png``, ``dma-tvo-music.png`` and ``dma-tvo-wake.png`` show
the normal UI before and after sleep. These runs use the private disk/NOR
fixture and existing boot-ROM/fused-key workarounds, without debugger edits
to the OS. The seven-register TVO VMState does not establish complete
machine migration or compatibility with older snapshots lacking the device.
``dma-tvo-reset.log`` and ``dma-tvo-reset.png`` additionally record a
subsequent system reset back to the normal main menu. The combined DMA/TVO
changes pass checkpatch with zero errors and warnings; both RST documents
pass Sphinx with warnings treated as errors.

I2S0 transport evidence includes ``audio-fixture/i2s-transport-original.asm``,
the earlier Ghidra exports ``i2s-original-iram.c``, ``i2s-original-dram.c``
and ``i2s-stream-original.c``, and ``i2s-order-gdb.log``. The connected
serializer's ``i2s-transport-qtest-final.log`` records forty-nine passing
tests, including requests held off while PCLK or codec LRCK is gated.
``i2s-transport-ui.png`` shows normal boot; ``i2s-tone-start.png`` and
``i2s-tone-after3s.png`` show Now Playing progress. ``i2s-tone-live.log``
records moving DMA addresses/counts and serialized sample values.
``i2s-serialized-analysis.log`` records the 51,016-frame exact source-WAV
comparison and spectral analysis. ``i2s-serialized-tone.raw`` contains the
captured stereo little-endian halfwords; ``i2s-serialized-tone.wav`` wraps
those same samples at the nearest integer rate, 44118 Hz, for inspection.
The capture is from the device's frame trace, not a host-decoded substitute.

``i2s-gated-sleep-wake.log`` records Long Play reaching WFI with PWRCON1
``0xffffeff7``, stopping the serialized-sample counter, followed by hold
wake with PWRCON1 ``0x0003ed49`` and renewed sample progress.
``i2s-deep-wake.png`` shows the restored normal main menu. In this sleep
path CLKCON retains its enable bit: PWRCON1 gating stops the interface.
The earlier ``i2s-deep-sleep.log`` check incorrectly required CLKCON to
read 2, and is retained as evidence of that rejected test assumption.
``i2s-transport-reset.png`` shows a preceding system reset to the normal
main menu. These runs retain the existing boot-ROM/fused-key workarounds.
I2S VMState version 2 preserves queued data, request state, clocks and
fractional phase; it rejects older device snapshots. Whole-machine
migration remains unproven. The code passes checkpatch without errors or
warnings, and both documents pass Sphinx with warnings treated as errors.

Codec output evidence includes ``audio-fixture/codec-output-original.c``
and ``codec-volume-pair-original.c`` (fresh Ghidra exports), the matching
``codec-volume-pair-original.asm``, and ``codec-volume-pair-live.log``
(original paired writes during UI volume changes). That output implementation
was checked with ``codec-output-qtest-final.log``: fifty-four passing tests.
``codec-output-resume.wav`` is output from the real QEMU WAV backend;
``codec-output-resume-waveform.log`` checks its sustained stereo spectrum.
``codec-output-resume-playing.png`` and ``codec-output-resume-volume.png``
show playback and user volume input without a debugger. The final
``codec-output-resume-sleep-wake.log`` records WFI at ``0x22002e74``,
PWRCON1 ``0xffffeff7``, hold wake to ``0x0003ed49``, renewed codec frame
progress, zero host drops, and preservation of the WAV file across wake.
``codec-output-resume-wake.png`` shows the normal Songs menu afterward.
The earlier ``codec-output-final.wav`` was truncated by reopening the WAV
backend on wake; it is retained as evidence of the corrected lifecycle
bug, not as a successful sustained-playback capture. Checkpatch and Sphinx
results are in ``codec-output-checkpatch.log`` and ``codec-output-sphinx.log``.

Codec transition evidence includes ``codec-transitions-original.c`` (fresh
Ghidra of initialization, masked updates and paired volume routines),
``codec-transitions-original.asm``, and the independently retrieved Cirrus
manual ``cs42l55-primary.pdf``. ``cs42l55-output-diagram.png`` renders its
figure 12, including the master controls available with DSP power down.
``codec-transitions-qtest-final.log`` records fifty-eight passing tests.
The first ``codec-transitions-qtest.log`` exposed a test setup error:
PCM value ``0xf4`` also sets mute; the unmuted -6 dB encoding is ``0x74``.

``codec-transitions-boot.log`` captures the running original firmware and a
bounded interval of both I2S and codec frame traces. The reproducible
``analyze-codec-transitions.py`` produces ``codec-transitions-analysis.log``:
165,667 paired frames, 331,334 gain checks, and four verified channel zero
crossings. ``codec-transitions.wav`` is the actual WAV backend output;
``codec-transitions-waveform.log`` checks its sustained stereo frequencies.
The ``codec-transitions-playing.png`` and ``codec-transitions-volume.png``
captures show normal playback and volume adjustment. No debugger, guest
instruction replacement or PCM substitution was used in this run.
``codec-transitions-sleep-wake.log`` records Long Play reaching WFI, the
PWRCON1 gate changing from ``0xffffeff7`` to ``0x0003ed49`` on hold wake,
codec frame progress resuming from 3,677,648 to 3,746,433, zero host drops,
and preservation of the WAV capture. ``codec-transitions-wake.png`` shows
the normal Songs menu after waking. The private VM was then quit with
terminal exit verified. Style and documentation checks are recorded in
``codec-transitions-checkpatch.log`` and ``codec-transitions-sphinx.log``.
