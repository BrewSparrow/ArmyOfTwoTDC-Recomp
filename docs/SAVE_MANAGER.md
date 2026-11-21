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
asks the content manager for a *snapshot* (SDK patch 0014, `save_gate.h`). It succeeds only at an
instant when no save or profile package is open **and** no create, open, close, delete, header or
thumbnail operation on one is running. The snapshot has an owner token; from then until that token
releases it, every such operation waits, and a second snapshot is rejected. The gate is never opened
by a timer: only the backup that owns it releases it (always, through a scope guard, also after an
error), so saves are never written while files are being copied.

If no such instant comes within 3 seconds the backup is **queued** and runs the next time the game
starts, before it opens any save file. The list shows it as Queued, and as Complete only after it
exists and has been verified. Closing the F6 screen during a backup does not stop it; reopening shows
the same running state. When the game closes, a running backup stops at its next file (leaving no
partial backup) and releases the gate before the runtime goes away.

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
- **One game per saves folder.** A second copy started from the same release folder says so and
  exits: two copies would write the same saves. (Other release folders have their own saves.)
- **A rollback that cannot finish is never reported as done.** If a file cannot be put back (for
  example another program holds it), the old files stay in `user\.ao2dc\restore-*\pre`, with the
  journal and the verified recovery backup. No other restore or queued item runs and the game does
  not start (a message names the folder) until the next start has completed the rollback.
- The confirmation lists every profile the backup replaces, every profile that has saved progress
  now, and warns about a backup profile that is not present in this installation. Nothing is
  remapped to another profile.

## Input

While the screen is open, keyboard, mouse and pad belong to it, not the game (SDK patch 0015 gives
every input driver, XInput included, a process-wide capture switch; the SDK's own overlay rules are
unchanged).

## Tests

`tests/save_store_test.cpp` runs the backend on disposable folders (round trip, fresh release folder,
missing saves, corrupt backups, failed writes, interrupted restore recovery, queue, import, locks):

```
clang++ -std=c++20 -O1 tests/save_store_test.cpp src/save_store.cpp -o save_store_test.exe
save_store_test.exe
```

The test also runs the SDK's header-only save gate (concurrent writers against snapshots, owner
tokens, overlap) and the rollback, cancel and lock failure paths.

`AO2DC_TEST_BACKUPS_ROOT` redirects the backup folder for automated runs only.

## Not covered

Controller navigation (the host UI has none), saves made by something other than the game's own
content packages, and a save package the game keeps open for longer than 3 seconds (the backup is
queued instead).
