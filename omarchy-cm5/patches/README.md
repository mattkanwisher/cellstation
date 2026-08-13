# patches/ — zero-patch policy

The point of this project is to run Quattro on the CM5 while **modifying the
smallest possible amount of upstream code**. As audited (see
`docs/feasibility.md`), zero upstream edits are needed for the build: x86
hardware scripts no-op behind their PCI/DMI probes, and everything
architecture-specific is expressed as overlay additions.

If a real build/boot run forces an upstream edit, it goes here as a
`git format-patch` file applied by the builder to `build/upstream/`, with a
comment explaining why an overlay couldn't express it — and it should be
PR'd upstream immediately. Current candidates we expect may eventually be
needed (but are NOT yet proven necessary):

- `install/hardware/vulkan.sh`: add a `[Broadcom]=vulkan-broadcom` entry to
  the vendor map (upstream-friendly: same shape as the existing Asahi entry).
- An `uname -m` guard if any install stage hard-fails on aarch64.

Empty directory = the goal state.
