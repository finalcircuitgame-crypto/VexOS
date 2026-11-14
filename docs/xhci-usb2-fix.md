# xHCI USB2 enumeration repair

The current workspace still uses Tiny64 names in its boot artifacts. The repair
is in `src/kernel/drivers/usb/xhci`, with register definitions in
`src/include/usb/xhci.h`.

## Root causes confirmed against the supplied specification

- PORTSC.PED is write-one-to-disable. The former read/modify/write operations
  echoed PED=1 while clearing change flags, disabling an enabled USB2 port.
  Writes now preserve only ordinary RW fields and explicitly acknowledge the
  observed change flags, including CEC. USB2 uses port reset; USB3 uses link
  training or warm reset. Debounce and reset recovery use specification timings.
- Setup TRT=2 means OUT. IN control requests require TRT=3. The old code also
  requested an event for Setup, Data, and Status and accepted the first event
  from the slot, potentially returning before descriptor data or Status finished.
  Only Status has IOC; short Data events update the actual length and wait for
  Status. Completion matching includes slot, endpoint, and TRB pointer.
- EP0 CErr was in the wrong context word. Both control and interrupt endpoints
  now put CErr=3 in DWORD1 bits 2:1. Full-Speed EP0 starts at 8 and is updated
  from the descriptor using Evaluate Context. LS=8, HS=64, SS=512 are validated.
- Endpoint bInterval was copied directly into the xHCI exponent field. FS/LS
  millisecond intervals are converted to microframe exponents; HS/SS use
  bInterval-1. Packet-size transaction bits and SS companion data are handled.
- Producer Link TRB cycles were never updated after a ring traversal, and wait
  routines discarded other devices' events. Shared enqueue now publishes the
  correct cycle at every wrap. One dispatcher retains HID and port-change events
  while synchronous commands/control requests wait.

Reference: the supplied xHCI PDF, sections 5.4.8 (PORTSC), 6.4.1.2 (control TRBs),
6.2.3 (endpoint contexts and intervals), and 7.2 (Supported Protocol capability).
Supported Protocol port ranges, Slot Type and Speed ID definitions are read;
Slot Context retains the controller's raw Speed ID while USB timing uses its
classified speed.

## Failure behavior

MMIO/DMA reads are volatile and ring publication/consumption has ordering
barriers. Polling mode leaves hardware interrupts disabled because there is no
xHCI IRQ handler. Controller reset, firmware handoff, command and control waits
are bounded. A command timeout stops the controller instead of reusing unknown
command state. Failed enumeration disables its slot; memory is released only
when Disable Slot completes. A stalled EP0 is reset and its dequeue pointer
advanced before an optional request can proceed. An interrupt endpoint error
stops its rearming and logs the failure. Connect/disconnect/disable events are
serviced without recursively enumerating inside a completion wait.

## Reproduce validation from Linux/WSL

    make
    gcc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
      -Isrc/include -Isrc/kernel/drivers/usb/xhci \
      tools/xhci_unit_test.c src/kernel/drivers/usb/xhci/xhci.c \
      src/kernel/drivers/usb/xhci/xhci_events.c \
      src/kernel/drivers/usb/xhci/xhci_hid.c \
      -Wl,--gc-sections -o build/xhci-unit
    ./build/xhci-unit
    python3 tools/xhci_qemu_test.py 1
    python3 tools/xhci_qemu_test.py 2

The QEMU tests use copies of the boot/storage disks and a disposable USB storage
image. Version 1 selects Full-Speed keyboard/mouse; version 2 selects High-Speed.
Both include a High-Speed tablet and SuperSpeed USB storage. They check descriptor
and configuration completion, USB typing plus the persisted `hello` file, more
than 256 mouse reports, and mouse removal/reconnection. Logs and screenshots
are in `build/xhci-review-1` and `build/xhci-review-2`.

The unit test exercises the actual production control builders and enqueue/event
code: safe PORTSC writes, LS/FS/HS/SS MPS and intervals, four producer wraps,
event wrap, interleaved HID input, short Data followed by Status, command pointer
matching, EP0 stall recovery, and command timeout. Driver sources compile with
`-O2 -Wall -Wextra -Werror` in that test.

## Coverage limits and hardware checks

QEMU provides FS and HS HID coverage and SS storage enumeration/configuration.
Low-Speed is covered by policy/control tests, not an attached physical device.
The current checkout has ATA storage but no USB bulk/mass-storage block driver;
USB flash-drive reads/writes are therefore not claimed by these tests.

On hardware, test a directly attached LS keyboard/mouse, an FS HID device
(including FS EP0 MPS 16/32/64), an HS HID device, and an SS flash drive. Check
`Port ready` retains PED=1, `Address context` has the controller Speed ID,
EP0/command completions have cc=1, interrupt intervals are speed-correct, and
`HID report` counters increase. Repeat unplug/replug on each connector and
verify other input devices continue responding. Validate firmware handoff,
64-byte contexts, optional-request STALL behavior, timeout diagnostics, and
controller-specific Supported Protocol mappings. USB hubs/transaction
translators and arbitrary composite/report-protocol keyboards remain outside
the existing driver's implemented scope.
