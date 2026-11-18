# Save Manager (F6)

A full-window screen, separate from the F4 settings window, to back up and restore saved progress.
Open or close it with `F6` (rebind: `bind_save_manager` in `settings/hardware.toml`; the game logs a
warning at start if another binding uses the same key). `Esc`, `F6` and **Return to Game** close it.
While it is open the keyboard, mouse and pad belong to it, not the game. Opening it does not save,
restore or pause anything.

## What is backed up

Saved progress, not the character's current position and not an emulator save state:

- every file under `user\<profile id>\454109AB\` (saved games, their `.header` records, profile data),
  for every profile folder (16 hex digits, not `0000000000000000`);
- `user\achievements\454109AB.toml` (unlocks).

Never included: the HD pack, DLC, title update, game files, shader cache, logs, `hardware.toml`.
Relative paths and the profile id are kept, so a backup restores into any release folder.

## Where backups are kept

`<Saved Games>\BrewSparrow\ArmyOfTwoTDC\Backups\<date>-<time>-<id>\` (Windows' Saved Games known
folder), the same for every release folder, so a new release finds them. Each backup holds
`manifest.toml` (format version, title id, profile ids, build, source, every file with size and
SHA-256) and `files\`. A backup is built in a `.tmp-` folder, every file is re-read and checked, and
only then renamed into place; a failed attempt leaves the existing backups alone.

## Save-write coordination

The game saves by opening a content package, writing, and closing it. **Back Up Saved Progress**
asks the content manager for a *snapshot* (SDK patch 0014): it succeeds only at an instant when no
save or profile package is open, and from then until the copy is verified any attempt to open, create
or delete one waits. If no such instant comes within 3 seconds the backup is **queued** and runs the
next time the game starts, before it opens any save file; the list shows it as Queued and as Complete
only after it exists and has been verified.

## Restore and import

- **Restore Backup...** asks for confirmation (backup, profile, target), then **queues** the restore.
  It is applied the next time the game starts, before any save file is opened, never while it runs.
- Before replacing anything the restore verifies the backup (checksums, title, profile, paths), makes
  and verifies a **recovery backup** of the current progress, stages the new files next to the
  targets, and swaps them under a journal. If it is interrupted, the next start rolls the old files
  back, so there is never a mixture of old and restored files.
- **Import From Older Release...** copies only the save data of the chosen older installation (its
  folder with `ao2dc_manifest.toml`, or its `user` folder) into a new backup; the original is only
  read. Close the older game first.
- A second instance using the same saves is detected; it neither restores nor runs queued items.

## Tests

`tests/save_store_test.cpp` runs the backend on disposable folders (round trip, fresh release folder,
missing saves, corrupt backups, failed writes, interrupted restore recovery, queue, import, locks):

```
clang++ -std=c++20 -O1 tests/save_store_test.cpp src/save_store.cpp -o save_store_test.exe
save_store_test.exe
```

`AO2DC_TEST_BACKUPS_ROOT` redirects the backup folder for automated runs only.

## Not covered

Controller navigation (the host UI has none), saves made by something other than the game's own
content packages, and a save package the game keeps open for longer than 3 seconds (the backup is
queued instead).
