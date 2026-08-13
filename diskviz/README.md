# diskviz version 33

Minimal libfdisk-based partition table visualiser and creator.

## Usage

```text
  ./diskviz /dev/DEVICE      Open a disk for viewing and editing (needs root)
  ./diskviz -h | --help      Show this help and exit
```

If the device has no partition table at all yet, diskviz offers to stage a fresh GPT label before anything else — like everything below, that's only committed to disk on [w]rite.

## Interactive Commands

Once a disk is open, use these interactive commands:

* n — create a partition in a chosen free-space segment (staged, not written)
* u — undo the most recently staged operation — create, delete, or restore
* d — delete an existing partition (staged, not written)
* b — back up the current on-disk partition table to a file
* r — restore a partition table from a backup file (staged, not written)
* w — write all staged changes to disk now — returns to this menu, doesn't exit
* q — quit — discards staged, unwritten changes (the only command that exits)

## Safeguards and Details

* [b] and [r] only work while nothing else is staged, so what they read from or write to always matches what's genuinely on disk.
* [d] and [r] also make you type the target device's own path back exactly, rather than a bare YES — either can affect real, already-written data once [w]ritten, so a name typed twice is the safeguard.
* Nothing touches the real disk until you type YES at [w]. diskviz never formats anything itself — it only edits the partition table; after a write, run the mkfs/mkswap command it suggests yourself if you want a filesystem on a new partition.
* A staged, not-yet-written create shows green right in the table, and a misaligned partition start shows yellow. The staged-operations list below the table uses the same green for a create and adds red for a delete, so the two views' colours line up.
* Colour only appears on a real terminal, and never if the NO_COLOR environment variable is set. Under sudo, set it like this — sudo's usual environment reset strips a plain "NO_COLOR=1 sudo ..." before diskviz ever sees it:

```text
  sudo env NO_COLOR=1 ./diskviz /dev/DEVICE
```

At most prompts, press Tab to autocomplete a suggested value, or type q to back out without changing anything.
