# OHCI for OPENSTEP

OHCI is an OPENSTEP driver for PCI OHCI USB 1.1 controllers. It supports USB
HID boot-protocol keyboards and mice, including separate interfaces on a
composite device. Low-speed and full-speed devices can connect directly or
through one tier of full-speed hubs.

Supported input includes keyboard modifiers, Caps Lock LED feedback, relative
pointer movement and left/right mouse buttons. Devices and hubs can be attached
and removed while running; removal releases held keys and buttons. The first
keyboard and mouse attached remain active. Additional providers are ignored
until disconnected and reconnected after the active provider is removed.

Storage, networking, arbitrary HID report formats, tablets, multitouch, mouse
wheels, middle buttons, nested hubs, isochronous transfers, suspend/resume and
live unloading are not supported.

## Requirements

- OPENSTEP 4.2 for Intel processors, running on one CPU
- PCI OHCI USB 1.1 controller
- Firmware-assigned controller registers and DMA memory addressable below 4 GB
- A routed PCI interrupt for the default INTx mode
- USB keyboard or mouse supporting the HID boot protocol

PCI autodetection includes NVIDIA `10de:0d9c` and QEMU's Apple `106b:003f`.
The driver requires OHCI PCI class code `0c0310`.

OHCI and EHCI are separate drivers for separate PCI controller functions;
they can be loaded together. OHCI does not depend on EHCI. On systems with an
EHCI companion, firmware and the companion controller must route low-speed
and full-speed ports to OHCI.

The driver uses shared INTx by default. Keep `Share IRQ Levels` set to `Yes`.
Polling is an explicit compatibility mode that keeps hardware interrupt
sources disabled and claims no DriverKit IRQ. INTx failures do not automatically
select Polling. Neither mode requires PCIMSI; MSI and MSI-X are not supported.

| `Interrupt Mode` | Requirement |
| --- | --- |
| `INTx` (default) | Valid PCI interrupt routing; `Share IRQ Levels = Yes` |
| `Polling` | None beyond the controller requirements; nominal 5 ms polling |

## Installation

Open `OHCI.config`.
Configure.app should open and confirm the driver was installed.
Click Add, select `PCI OHCI USB HID Controller`, and add the driver.
If the driver is not shown, check `Show All Installed Drivers` and select it.

Keep `Interrupt Mode` set to `INTx`, or explicitly set it to `Polling` if needed.
The driver obtains its IRQ from PCI configuration; do not add an IRQ manually.

Set `USB Input = Yes` on the controller connected to your keyboard and mouse,
and `USB Input = No` on other OHCI instances. Disable competing PS/2, serial
mouse, EHCI, xHCI or other native input providers. Select two-button behavior
in OPENSTEP's mouse preferences to use the right mouse button separately.

Add one instance per controller in Configure.app. Each needs its own
`InstanceN.table` in `OHCI.config`; a single `Instance0.table` selects only one
controller. To create the tables manually, copy `Default.table` and retain
its other settings.

For portable autodetection of two controllers, use:

| Table | `Instance` | `Location` | `USB Input` |
| --- | --- | --- | --- |
| `Instance0.table` | `"0"` | `""` | `"Yes"` |
| `Instance1.table` | `"1"` | `""` | `"No"` |

Keep the same full `Auto Detect IDs` list from `Default.table` in both tables.
With an empty `Location`, `Instance` selects a zero-based match in PCI
bus/device/function order. Do not narrow the detection list for the second
instance: `Instance = 1` selects the second match of that list. Use successive
instance numbers for additional controllers. If the keyboard and mouse are on
the second controller, swap the `USB Input` values above.

If the controller is not automatically detected, click Expert and set `Location`
to the controller's PCI coordinates using this exact syntax:

```text
Dev:<device> Func:<function> Bus:<bus>
```

For example, PCI bus 0, device 4, function 0 is `Dev:4 Func:0 Bus:0`.
Use the coordinates reported for your OHCI controller. If its PCI ID is absent
from `Auto Detect IDs`, add it in device/vendor order: NVIDIA `10de:0d9c`, for
example, is `0x0d9c10de`.

Click Done, click Save, and Quit.

Verify the `Instance`, `Location`, `Auto Detect IDs`, `Interrupt Mode`,
`Share IRQ Levels` and `USB Input` settings in each `InstanceN.table` under
`/private/Devices/OHCI.config`. Include the instance tables on boot media under
`/private/Drivers/i386/OHCI.config` as well.

In `/private/Devices/System.config/Instance0.table`, list `OHCI` once in
`Boot Drivers`, after `PCIBus` and the existing root-storage controller.
Keep the existing storage driver entries.

Restart OPENSTEP to load the driver. To update an existing installation,
back up `OHCI.config`, replace its driver and resources with the new bundle,
and preserve your `InstanceN.table` settings before restarting.

## Building

With OPENSTEP 4.2 DriverKit installed, run `make clean` followed by `make`
in this directory to produce `OHCI.config`.
