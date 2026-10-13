# Multiplayer / co-editing (phase 1) — implementation notes

Compact reference for future work. Phase 1 status: implemented, builds clean
(clang-tidy + clang-format pass). Socket layer loopback-tested; full two-instance
GUI flow not yet verified at runtime.

## Scope
- 1 host + 1 guest, direct TCP (no server, no NAT traversal; LAN / port forward /
  Tailscale / ZeroTier). Default port 7654.
- Each side has its own simfile copy; only **notes of the host's current chart** sync.
- Not synced: tempo/timing, chart/song properties, chart creation. No chat/cursors.

## Files
| File | Role |
|---|---|
| `src/Editor/NetSocket.{h,cpp}` | Non-blocking IPv4 TCP wrapper (Winsock / POSIX `#ifdef`). `listen`, `beginConnect`/`finishConnect`, `accept`, `poll` (select), `send`/`recv` (0 = would block, -1 = closed/error), `localAddresses`. |
| `src/Editor/Session.{h,cpp}` | `gSession`. Protocol, `NetLink` worker thread, `SessionImpl` host/guest state machine, song check, chart id, checksum. |
| `src/Dialogs/Collaborate.{h,cpp}` | `DialogCollaborate` ("COLLABORATE"): status, Port+Host, Share+Copy, Join+Join, Disconnect. |
| `src/Managers/NoteMan.{h,cpp}` | Hooks + `applyRemoteEdit(add, rem)`, `replaceNotes(notes)` (no history; keeps selection). |
| `src/Editor/History.{h,cpp}` | `clearAll()` (keeps unsaved flag), `markExternalChange()`; Ctrl+Z/Y blocked when `gSession->blocksUndo()`. |
| Registration | `Action::OPEN_DIALOG_COLLABORATE` (Action.h/.cpp, Shortcuts.cpp name table), `DIALOG_COLLABORATE` (Dialog.h, Dialog.cpp id `"collaborate"`), Editor.cpp (factory, `Session::create/destroy`, `gSession->tick()` at start of `Editor::tick()`, `gSession->onChanges()` in `notifyChanges()`), Menubar File → "Collaborate...", `bin/settings/shortcuts.txt` Shift+K. |
| CMake | Files added to `src/Editor/CMakeLists.txt` / `src/Dialogs/CMakeLists.txt`; Editor links `ws2_32` on WIN32. |

## Hook points (key to the design)
- All note edits go through `NotesManImpl::modify()` → after `prepareEdit`, if
  `gSession->isGuest()` → `gSession->forwardEdit(result.add, result.rem)` and return
  (guest never applies locally, never creates history).
- All note applies (edit/undo/redo) go through `NotesManImpl::myApplyNotes()` →
  after sanitize calls `gSession->onNotesApplied(chart, add, rem)` → host broadcasts.
- `insertRows()` refused on guest.
- Host applies guest edits with `gNotes->modify(edit, false, &GuestEditDesc)` → normal
  history entry ("Guest edited N notes").

## Undo model
- Host history = single linear log of authoritative chart; host can undo anything,
  results are broadcast.
- Guest: undo/redo disabled during session; history cleared on join (SongCheck ok)
  and on session end; remote changes call `markExternalChange()` (title `*`).
- Join refused if guest `hasUnsavedChanges()`.

## Protocol v1
Frame: `[u32 size LE][u8 type][payload]`, size counts type+payload, max 16 MB.
Payload via `WriteStream`/`ReadStream` (native LE); note lists via `NoteList::encode(out,false)` / `decode(in,0)`.

| Type | Dir | Payload |
|---|---|---|
| 1 Hello | G→H | u32 magic `0x504D5641` ("AVMP"), u16 version, SongInfo |
| 2 SongCheck | H→G | u8 ok, str reason |
| 3 ChartSnapshot | H→G | u32 chartGen, u8 hasChart, [str styleId, i32 difficulty, i32 index-among-same-style+diff, notes], u32 checksum |
| 4 NoteAdd / 5 NoteRemove | both | u32 chartGen, u8 flags (`EDIT_MORE_FOLLOWS`=1), notes, u32 checksum (guest sends 0) |
| 6 Disconnect | both | str reason |
| 7 ResyncRequest | G→H | empty (host replies with snapshot) |

SongInfo: u64 FNV-1a of music file bytes, f64 duration (`gMusic->getSongLength()`,
requires `getSamples().isCompleted()`), str title, str artist, f64 offset
(`simfile->tempo->offset`). Tolerances: duration 1 ms, offset 0.1 ms.

Compound edits: NoteRemove(MORE_FOLLOWS) + NoteAdd → combined into one `modify()`.
Checksum: 32-bit FNV-1a over row, endrow, col|player|type|quant of `chart->notes`.
`chartGen` increments per snapshot; host drops guest edits with stale gen.

## Threading
- `NetLink` owns one `std::jthread`: host = accept loop (one peer; extras accepted
  and closed), guest = non-blocking connect (5 s timeout) then serve loop
  (`select` every 20 ms). Mutex-guarded `myInbox` (NetEvent: CONNECTED/LOST/FAILED/FRAME)
  and `myOutbox` (raw frames). `dropPeer()` closes after flushing; `stop()` flushes
  best-effort (~200 ms), joins, clears queues.
- Main thread only: `SessionImpl::tick()` drains inbox; all editor state touched there.

## State machine
- Host: IDLE → `host(port)` → LISTENING → CONNECTED event → HANDSHAKE → Hello ok →
  CONNECTED (send snapshot) / reject (SongCheck fail, dropPeer, back to LISTENING).
  Guest loss → LISTENING.
- Guest: IDLE → `join("ip:port")` → CONNECTING → HANDSHAKE (send Hello) →
  SongCheck ok → CONNECTED. Failure/loss/Disconnect → `myEndSession()`.
- `onChanges`: session ends if `VCM_FILE_CHANGED` or simfile pointer changes.
  Host sends snapshot if chart changed or checksum ≠ last broadcast (catches
  insert-rows etc.). Guest switching away from host chart sets `myFollowHostChart`,
  handled next tick (reportChanges inside onChanges would be lost —
  `notifyChanges` zeroes `myChanges` after dispatch).
- Guest with no matching local chart: `myChart = nullptr`, edits paused.

## Build notes
- Build: VS 18 vcvars64 + ninja in `out/build/windows-debug` (`ninja` builds all
  incl. `format` check). clang-tidy runs with `--warnings-as-errors`; avoid C-style
  casts, use-after-move on custom types (NoteList — copy instead), unused RAII.
- Repo files are LF; don't let scripted edits convert to CRLF.

## Manual test (one machine)
Copy song folder twice; run exe twice; A: open song, wait for audio, Shift+K → Host;
B: open copy, Shift+K → Join `127.0.0.1:7654`. Check: guest edits appear on both;
host Ctrl+Z undoes guest edits; guest Ctrl+Z warns; host chart switch → guest follows;
changed offset/title in B → rejected with reason.

## Ideas for phase 2
Tempo/timing sync, guest undo forwarded to host, >2 peers, cursors, IPv6, NAT traversal,
hash audio off main thread, guest-side chart creation.
