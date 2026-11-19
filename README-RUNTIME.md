# Experimental Urchin USB runtime editor

This branch extends ZMK v0.3 and its Studio RPC for the local Keymap Editor.
It has passed compilation and host tests, but has not yet been flashed and
verified on the user's keyboard. The normal `main` branch remains the rollback.

## Installation and activation

Flash **both** `1_urchin_left.uf2` and `2_urchin_right.uf2` from this branch.
The right half now contains the reserved behavior devices and supports forwarded
reset/soft-off settings. Do not mix it with the earlier basic Studio beta.
Do not flash the settings-reset artifact as part of a routine update.
Unlock remains on the num layer's right inner top key (logical position 5).

Connect the left half to the experimental local editor over USB, unlock, edit,
then save. Saving validates the complete definition set, writes a new flash
bank, verifies it, and requests a reboot. Reconnect after the restart to verify
the saved settings. It does not rebuild or flash firmware for each edit.

## Supported runtime definitions

- Layer bindings, names, insertion/removal: up to 12 layers.
- Combos: 64 entries, 2–8 distinct physical positions each.
- Conditional layers: 32 entries; cyclic references are rejected.
- Hold-tap, macro (including parameter controls), mod-morph, sticky-key,
  tap-dance, caps-word, key-repeat, key-toggle, toggle-layer, soft-off, reset,
  and two-axis pointing configurations.
- New definitions use a bounded pool of precompiled device slots. Changing a
  definition does not dynamically compile arbitrary C drivers or enable new
  hardware features. Existing external drivers such as adaptive-key remain
  compiled configurations; unsupported edits are rejected by the editor.

The preserved editor cannot author every external module's child-node syntax.
Full support for arbitrary external drivers is therefore not complete.
Sensor/encoder definitions and board/build settings are outside this prototype.
USB edits currently persist on the device; uploading their source to Git is a
separate integration task.

## Persistence and recovery

The left application occupies 0x26000–0xcc000 (664 KiB). Two 64 KiB runtime
banks occupy 0xcc000–0xec000. Existing settings and bootloader regions remain
at their original addresses. Each bank holds a bounded executable plan plus
the complete UTF-8 source document (at most 32768 bytes).

The inactive bank is erased and written, its body is read back and CRC-checked,
and its header is committed last. Boot chooses the newest valid generation;
an invalid bank or invalid program falls back to the previous valid bank or
compiled defaults. A schema hash prevents using plans with another firmware's
device IDs. Uploads interrupted before commit expire after 10 seconds.

An existing runtime bank can outlive reflashing the same firmware. Opcode 9
restores compiled defaults by erasing both runtime banks and rebooting. Keep
the earlier known-working firmware ZIP for recovery; flashing the stable main
firmware ignores this experimental partition.

## Protocol

Studio `keymap` request/response field 126 carries bounded bytes (512 maximum).
The standard Studio unlock check protects the handler. Every reply starts with
signed little-endian errno and unsigned generation (4 bytes each).

Commands: INFO=1, CAPS=2, SOURCE=3, BEGIN=4, WRITE=5, VALIDATE=6,
COMMIT=7, ABORT=8, RESTORE=9, BEHAVIOR_DEFAULTS=10.
Reads/writes are chunked at 448 bytes. BEGIN requires the expected generation,
schema hash, total length and CRC. A lost COMMIT reply is ambiguous; the client
requires reconnecting instead of automatically committing again.

Plan version 2 starts with `KMP2` and contains exactly one definitions, combos,
conditionals, and layers section. Hold-tap and combo timing fields are 32-bit;
disabled quick-tap/prior-idle values retain their signed -1 sentinel. Definitions
are checked for device identity, reference cycles, bounded recursion and pool
capacity before flash is changed.

## Validation and remaining device checks

The workflow compiles `runtime/rt_store.c` on the host with warnings as errors.
It injects a power cut at each of 584 programmed byte positions, checks that the
old generation survives, and tests corruption fallback and stale generations.
These tests exercise the same persistence implementation compiled into ZMK.

The PC suite verifies the actual Studio protobuf codecs, upload ordering and
ambiguous disconnect handling, and compiler validation. Browser simulation has
verified reading the existing Urchin definitions and saving/reloading edits.
Simulation is not a physical keyboard test.

Still required on the keyboard: initial USB connection; behavior execution;
combo, macro and conditional-layer edits; save/reboot/reconnect; reset/soft-off
forwarding to the right half; pointing HID reports; and physical power-loss
recovery. Do not promote this branch to normal firmware until those pass.
