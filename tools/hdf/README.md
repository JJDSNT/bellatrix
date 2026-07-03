# tools/hdf — HDF tooling

Wrapper over [amitools](https://github.com/cnvogelg/amitools) (`external/amitools`,
git submodule, used from source — no pip install needed, pure-Python for the
xdftool/rdbtool paths we use).

## Usage

```bash
# Create a 100MB HDF: RDB + single bootable FFS-intl partition, formatted
python3 tools/hdf/hdf.py create image.hdf 100Mi              # DOS3, DH0
python3 tools/hdf/hdf.py create image.hdf 50Mi --dostype DOS1 --name WORK

# Full report: RDB checksums, geometry, partitions, FS info
python3 tools/hdf/hdf.py analyze image.hdf

# List files (works on HDF and ADF)
python3 tools/hdf/hdf.py ls image.hdf [PARTNAME]

# Copy entire ADF contents into the HDF
python3 tools/hdf/hdf.py adf2hdf disk.adf image.hdf [--dest Apps/WB] [--part DH0]

# Extract an ISO (needs 7z on the host) and copy its tree into the HDF.
# Sanitizes for FFS: strips ';1', truncates names to 30 chars, drops
# case-insensitive duplicates (with warnings).
python3 tools/hdf/hdf.py iso2hdf disc.iso image.hdf [--dest CD] [--part DH0]

# Copy single files/dirs in and out
python3 tools/hdf/hdf.py write image.hdf hostfile [S/Startup-Sequence]
python3 tools/hdf/hdf.py read  image.hdf S/Startup-Sequence [out]

# Raw passthrough for anything else
python3 tools/hdf/hdf.py xdf image.hdf open part=DH0 + delete C/Foo
python3 tools/hdf/hdf.py rdb image.hdf show
```

## Notes

- `create` writes an RDB whose partition dostype matches the volume format
  (DOS0..DOS5 mapped to ofs/ffs ± intl/dircache).
- rdbtool default geometry is `heads=1 sectors=32`; real-world imagers
  (Emu68-Imager) use larger CHS values for big disks. If lide.device or a
  Kickstart rejects a geometry, use `rdb ... create size=... chs=...`.
- No filesystem driver (LSEG) is embedded in the RDB; DOS0/DOS1/DOS3 are
  handled by the Kickstart/AROS ROM filesystem. If a partition needs a
  loadable FS, add it with `rdb image.hdf fsadd <driver>`.
- ISO→HDF is implemented via `iso2hdf` (ISSUE-0029, closed); protection
  bits don't exist on plain ISO, files land as `rwed`.
- Emu68 SD layout puts the RDB *inside* an MBR partition (type 0x76) rather
  than in a loose .hdf file — handling that arrangement is the SD-card
  stage, tracked as ISSUE-0030.

## Related (next stage: SD card)

- https://github.com/PiStorm/hdf2emu68 — converts HDF to Emu68 SD layout
- https://mja65.github.io/Emu68-Imager/ — full SD imager for Emu68
