# Ghost-Control Manba V2 USB Patch for Ghost control / StonedModder



The base remains StonedModder's Ghost-Control: read a supported USB controller, translate
its input reports, create a virtual PS5 controller, then inject
`ScePadData`.

The work added here mainly concerns:

- Manba V2 support over USB;
- Manba PC/XInput mode;
- Manba Switch USB/dongle mode;
- Y-axis correction in Manba Switch mode;
- clean behavior between the Manba and the official controller when they
  try to take the same user;
- A few separate Bluetooth research notes, because Bluetooth is not resolved
  in this ELF, still under research...

## Credits

- Original project/tool: StonedModder,
  `Ghostcontrol-PS5-USB-Controller-Patcher`
  - https://github.com/StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher
- Manba V2 NBJr USB tests, PS5 validation, axis verification, tests
  user switching and Manba/official tests.
- PS5 SDK John tromblom 

## Folder Contents

```text
ELF/
  GhostControl-Cleanup.elf
  GhostControl-ManbaV2-NBJr-USB-Patch.elf

SOURCE MODIFIEE PAYLOAD/
  Modified sources used to compile the final tested OK payload

Launch-GhostControl-ManbaV2-NBJr.bat
Launch-GhostControl-ManbaV2-NBJr.ps1

```

## What Works Over USB

- Manba V2 in PC/XInput USB mode.
- Manba V2 in Switch USB/dongle mode.
- Y-axis correction in Manba Switch mode in `payload/gc_main.c`.
- When the Manba becomes active, the official controller disconnects cleanly.
- If the official controller is turned back on on another user, both controllers
  can remain active to play with two players.
- If the official controller is turned back on on the same user as the Manba, the Manba
  VDA is released and the official controller takes control again.

## Bluetooth Research

During tests, the PS5 could see the Bluetooth part of the Manba, but it
behaved like an accessory, not like a real usable `scePad` controller.
The profile/user popup could sometimes open, and the console could
show two controllers/accessories, but the Manba Bluetooth path did not provide
a stable input stream

Points seen during the tests:

- The Bluetooth transport was visible as a MediaTek device:

```text
/dev/ugen0.2
VID:PID 0x0e8d:0x3603
manufacturer="MediaTek Inc."
product="Wireless_Device"
class=0xe0 sub=0x01 proto=0x01
```

- This MediaTek device indicates the Bluetooth transport/adapter, not the buttons
  of the controller.
- The receiver/update mode of the Manba was also seen as `1a34:f517`.
- Other devices like Realtek `0x0bda:0x9210` are USB/adapter noise and
  must not be taken for a controller.
- Over USB, the payload has a real `/dev/ugen*` device and real input reports on
  endpoint `0x81`.
- Over Bluetooth, we did not obtain the same readable input report path.
- The official controller becomes a real controller with `scePad` handles and user.
- The Manba Bluetooth remained on an accessory path, not a reliable `scePad`
  pad.

Areas researched:

- scan `/dev/ugen*`;
- USB descriptors;
- USB endpoints;
- klog lines `Open Pad`;
- klog lines `DEVICE_ADDED`;
- physical MBus IDs ending with `0x0300`;
- virtual IDs created by the payload;
- `scePadInit`;
- `scePadGetHandle`;
- `scePadVirtualDeviceAddDevice`;
- `scePadVirtualDeviceInsertData`;
- `scePadVirtualDeviceDeleteDevice`;
- `scePadSetProcessPrivilege`;
- `SceShellUI`;
- `SceShellCore`;
- `libScePad`;
- `libSceMbus`;
- `sceMbusDisconnectDevice`;
- `sceMbusBindDeviceWithUserId`.

What was used in the final USB patch:

- disconnection/release of the physical official controller;
- detection when the official controller takes back the same user;
- release of the Manba VDA when the official controller takes back this user.

What is not resolved:

- bind the Manba Bluetooth as a real controller;
- read real Manba Bluetooth input reports;
- assign the Manba Bluetooth to a user like a normal `scePad` controller.

 The Manba BT is seen by the PS5, but it remains blocked before the
 game input path.

Findings:

- Confirmed Manba BT address:
  - normal: `98:b6:ea:bd:cd:58`
  - reversed in memory: `58 cd bd ea b6 98`
- In `SceSysCore` / `SceMbusKmodEventPolling`, a Manba event was found:
  - Manba reverse hit around `+0x1ff30`
  - estimated event start around `+0x1ff10`
  - event signature: `0x08 / 0x04 / 0x03`
  - `manba_hits_total=1`
  - `manba_hits_heap=0`
- In `SceMbusHeap`, the official DualSense appears with its BT and its
  VID/PID, but the Manba does not have an equivalent pad entry.
- The official event vs Manba event comparison shows that the official event
  carries BT + VID/PID together, while the Manba event carries the MAC but no
  nearby Manba VID/PID. This points to a classification/promotion problem from
  accessory to pad, not to a simple address patch.
- The attempts, and the tests
  `KMOD_TO_HEAP … confirmed that these leads are not enough:
  - VID/PID ShellUI patch alone;
  - SIG8 ShellUI patch;
  - direct bind `0x190300`;
  - direct bind `0x30300`;
  - force `0x2030e` as pad;
  - fallback `0x2030e -> 0x190300`;
  - ShellUI table activation `active20/user24/type28` with or without user;
  - simple scans `/dev/hid` and `/dev/bluetooth_hid`.
- The game tests show that `eboot.bin` has its own `libScePad` table:
  the official controller is active there, but the Manba BT remains on the Shell/Cdlg/accessory side.
- The game calls `scePadRead`.
  ...

Current conclusion: Bluetooth requires separate research around the
PS5 Bluetooth stack, HCI/L2CAP/HIDP, the accessory-to-pad logic, or the
modules/PRX used by the official DualSense.


## Launch The Final Payload

1. Start the payload listener on the PS5, port `9021`.
2. Run:

```powershell
.\Launch-GhostControl-ManbaV2-NBJr.ps1
```

or double-click:

```text
Launch-GhostControl-ManbaV2-NBJr.bat
```

3. Enter the PS5 IP.
4. The launcher first sends `GhostControl-Cleanup.elf`, waits 2 seconds,
   then sends `GhostControl-ManbaV2-NBJr-USB-Patch.elf`.


## Where To Look For The Patches

The detailed guide is here:

```text
PATCH_MANBA_FINAL_DETAIL.txt
```

The important areas:

- `payload/controller_mamba.h`
  - Manba VID/PID.
  - PC/XInput mode.
  - Switch USB mode.
- `payload/controller_mamba.c`
  - Manba PC/XInput mapping.
  - XInput axes.
  - XInput buttons.
- `payload/gc_main.c`
  - Manba detection.
  - USB routing.
  - Manba Switch Y-axis correction after `nintendo_handle_packet()`.
  - VDA release/recreation logic.
  - detection of the official controller taking back the same user.
- `payload/shellui_pad.c`
  - ShellUI/MBus functions to cut off a physical controller.
  - handle/user verification.
- `payload/shellui_pad.h`
  - declarations of the new functions.
- `payload/Makefile`
  - addition of `controller_mamba.o`.

## Important Switch Correction

The Switch correction is not in `controller_nintendo.c`.

It is in:

```text
payload/gc_main.c
```

in `usb_hid_thread`, after:

```c
injected = nintendo_handle_packet(...);
```

with:

```c
if (injected > 0 && is_mamba_switch)
    pad.leftStick.y = (uint8_t)(255u - pad.leftStick.y);
```

Why here?

Because if we invert directly in `controller_nintendo.c`, we risk
breaking real Switch Pro controllers or other Nintendo-compatible pads ( I do not have controllers to test )
The correction must remain limited to the Manba recognized as `057e:2009`.

## Build

Compilation uses the PS5 payload SDK environment…

Script used during the tests:

```text
payload/build_correction.sh
```

Final ELF tested OK:

```text
ELF/GhostControl-ManbaV2-NBJr-USB-Patch.elf
```

## Warning

Research only for educational purposes . Use at your own risk . Tested only with my PS5 6.02 and Manba environment.
I unfortunately do not have tools, script or universal magic payload for deeper analysis for support of other controllers or BT adaptation, I proceed step by step, this takes time ...

