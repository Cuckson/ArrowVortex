# Multiplayer / co-editing (phase 2) — implementation notes

Compact reference for future work. Phase 2 status: implemented, builds clean
(clang-tidy + clang-format pass). Verified at runtime on Windows with two real
instances plus scripted protocol clients (join, full-state adoption, note /
tempo / metadata / chart property sync both ways, guest undo, host undo not
touching guest notes, presence + cursor rendering, late joiners). Not verified:
Linux and Wine builds (no environment available), real LAN/internet.

## Scope
- 1 host + up to 16 guests, direct TCP (no server, no NAT traversal; LAN / port
  forward / Tailscale / ZeroTier). Default port 7654. The host relays everything.
- A guest without the host's song (nothing open, or different music) downloads
  it first: music, the host's simfile as saved on disk (.sm/.ssc/.dwi/.osu),
  and banner/background/CD title if they are plain file names. Files go to
  `<exe dir>/saves/<host song folder>/` (a folder with the same music is
  reused, otherwise " (2)" etc. is appended; an overwritten simfile is kept as
  `.bak`). The guest then opens it, waits for the music to decode (≤ 120 s) and
  rejoins automatically. Skipped when the same music is already open.
- Join with the same music file (FNV-1a hash + decoded length): the
  guest's simfile is **replaced by the host's**: metadata, song timing, chart
  list (style, difficulty, meter, step artist, split timing, notes). Join is
  refused if the guest has unsaved changes.
- Everyone can edit everything that is synced, on any chart:
  notes, song timing (offset, BPM, stops, delays, warps, time signatures,
  ticks, combos, speeds, scrolls, fakes, labels, display BPM, attacks), chart
  split timing, chart properties (step artist, difficulty, meter), song
  metadata (title/subtitle/artist + translits, genre, credit, banner,
  background, CD title, lyrics path, preview, selectable).
- Not synced: music path, BG/FG changes, chart creation/deletion. Charts the
  host adds mid-session are only sent to guests that join later; charts a guest
  adds stay local.
- Each user has a name (Collaborate dialog, saved in settings.txt `session`).
  Other users' mouse cursors are drawn over the notefield (semi-translucent
  arrow + name) when they are on the same chart, and marked on the minimap
  (scrub bar) with their name (hover position, or cursor position if not
  hovering; dimmed "(other chart)" if on another chart).
- Other users' receptors (where 1-4 place notes, i.e. their cursor time) are
  drawn tinted in their color with a name tag, and the notes / region they
  have selected are highlighted in their color (purely visual).
- The host can kick a guest (Collaborate dialog: Guest list + Kick).

## Files
| File | Role |
|---|---|
| `src/Editor/NetSocket.{h,cpp}` | Non-blocking IPv4 TCP (Winsock / POSIX). `pollAll` (select over many sockets), `localAddresses` (getifaddrs on POSIX, since the host name resolves to 127.0.1.1 on most Linux distros). Windows uses `SO_EXCLUSIVEADDRUSE`. |
| `src/Editor/Session.{h,cpp}` | `gSession`. Protocol, `NetLink` worker thread, `SessionImpl`, serialization, cursor drawing. |
| `src/Dialogs/Collaborate.{h,cpp}` | "COLLABORATE": status, Name, Port+Host, Share+Copy, Join+Join, Guest+Kick, Disconnect. |
| `src/Editor/Notefield` | `drawPeerReceptors`, `drawPeerSelection` (tinted selection boxes). |
| `src/Editor/Minimap.cpp` | `drawPeers()`: scrub-bar markers and stacked name tags. |
| `src/Managers/NoteMan` | `applyRemoteEdit(chart, add, rem)`, `replaceNotes(chart, notes)`: no history, any chart. `myApplyNotes` calls `gSession->onNotesApplied`. |
| `src/Managers/TempoMan` | `onRemoteTempoChanged(tempo)` (null = refresh all). |
| `src/Managers/SimfileMan` | `getForRemoteEdit()`, `onRemoteChartsChanged(open, reload)` (sorts, keeps the open chart). |
| `CMakeLists.txt` | `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` on Windows (see Wine below). |

## Sync model
- **Chart ids**: the host numbers its charts (`myChartIds`, Chart* → u32) and
  sends them in the full state; guests map the same ids onto their (replaced)
  charts. All note/section messages use ids, so chart re-sorting is harmless.
- **Notes (operations)**: every local edit/undo/redo goes through
  `myApplyNotes` → `onNotesApplied` → `NOTE_EDIT(origin, chart, add, rem,
  checksum)`. Guests apply locally first and count it as pending; the host
  applies guest edits with `applyRemoteEdit` (**no history**, so host Ctrl+Z
  only reverts the host's own edits) and relays to all guests including the
  sender (= ack). A guest compares the host checksum once it has no pending
  edits for that chart and sends `RESYNC_REQUEST` on mismatch (host replies
  with `NOTE_REPLACE`). Changes that bypass `myApplyNotes` (insert rows) are
  caught in `onChanges` by comparing each chart's checksum with the last known
  one and sent as `NOTE_REPLACE`.
- **Everything else (sections)**: `SONG_META(0)`, `TEMPO(0 = song, id = chart
  split timing)`, `CHART_PROPS(id)`. On relevant change flags the session
  serializes all sections and sends those whose FNV hash changed (flushed at the
  start of `tick`, before incoming messages). Last writer on the host wins: the
  host applies, then broadcasts its own version to everyone *including the
  sender*. A guest ignores incoming versions of a section while it has pending
  (unechoed) sends for it, and on the last echo applies the host's version if
  it differs → all peers converge. After applying, the local re-serialization's
  hash is stored, so nothing echoes back.
- Remote changes call `History::markExternalChange()`. Local history is kept
  through and after the session; diffs are applied tolerantly (notes by
  row/col, segments by row) on top of others' changes.
- Chart tempo that is removed remotely is cleared, never deleted (history
  entries may point at it).

## Protocol v3
Frame: `[u32 size LE][u8 type][payload]`, size counts type+payload, max 64 MB.
Payload via `WriteStream`/`ReadStream`; blobs = `writeNum(len)` + bytes; notes
via `NoteList::encode(out,false)`. Host = id 0, guests 1.., `origin` is
overwritten by the host with the sender's connection id.

| Type | Dir | Payload |
|---|---|---|
| 1 Hello | G→H | u32 magic `0x504D5641`, u16 version, str name, u8 has song, u64 audio hash, f64 length |
| 2 Welcome | H→G | u8 result (0 rejected, 1 ok, 2 download), str reason, u32 guest id; if 2: str folder, str music name, u64 music hash, u64 music size |
| 3 FullState | H→G | blob meta, blob song tempo, u32 n, n × [u32 id, str style, blob props, blob tempo, notes] |
| 4 PeerList | H→G | u32 n, n × [u32 id, str name] (host first) |
| 5 NoteEdit | both | u32 origin, u32 chart, notes add, notes rem, u32 checksum |
| 6 NoteReplace | both | u32 origin, u32 chart, notes, u32 checksum |
| 7 State | both | u32 origin, u8 section, u32 id, blob data |
| 8 Presence | both | u32 origin, u32 chart, u8 hover, f64 hover time, f64 hover x (noteskin units from receptor center), f64 cursor time |
| 9 ResyncRequest | G→H | u32 chart |
| 10 Disconnect | both | str reason (also used to kick) |
| 11 Selection | both | u32 origin, u32 chart, i32 region begin, i32 region end, num n, n × [num row, u8 col] (sent ≤ 10/s, on change) |
| 12 DownloadRequest | G→H | u8 need music (0 if the saves folder already has it) |
| 13 FileData | H→G | u8 kind (0 music, 1 simfile, 2 image), str name, u64 size, u64 offset, num n, n bytes (1 MB chunks, in order) |
| 14 DownloadDone | H→G | str simfile name (may be empty), str music name |

A downloading guest is not "joined" on the host (not listed, gets no edits).
The guest validates file names (plain names only, no separators/reserved
characters), sizes (music ≤ 1 GB, others ≤ 32 MB) and chunk order.

Section blobs: meta = 12 strings + f64 preview start/length + u8 selectable;
tempo = u8 present, f64 offset, i32 display BPM type, f64×2 range,
`SegmentGroup::encode`, attacks, keysounds, misc; props = str artist,
i32 difficulty, i32 meter. Checksum: 32-bit FNV-1a over row, endrow,
col|player|type|quant.

## Threading
- `NetLink` owns one `std::jthread` running `myRun`: host = listener + any
  number of connections, guest = one connection (after a 5 s non-blocking
  connect). `NetSocket::pollAll` every 20 ms. Mutex-guarded inbox (NetEvent
  CONNECTED/LOST/FAILED/FRAME with peer id) and outbox (peer id + frame).
  `dropPeer(id)` closes after flushing; `stop()` flushes best-effort, joins.
- Main thread only: `SessionImpl::tick()` (start of `Editor::tick`) flushes
  sections, drains the inbox, sends presence (≤ 30/s, on change), rebuilds
  `getPeers()`. `drawCursors()` is called after the notefield is drawn.
- Winsock is started lazily on the first host/join.

## Platform notes
- **Wine**: MSVC 17.10+ `std::mutex` has a constexpr constructor that needs a
  matching `msvcp140.dll`; Wine's builtin (and old redistributables) crash on
  the first lock. Fixed with `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`.
- **Linux**: share address uses `getifaddrs` (was showing 127.0.0.1 only);
  `send` uses `MSG_NOSIGNAL`; `EINTR` from `select`/`send`/`recv`/`connect`
  is treated as "try again" instead of a dropped connection; socket errors
  are reported with `strerror`. Editor links `Threads::Threads`. Downloaded
  songs fall back to `SDL_GetPrefPath` (`~/.local/share/ArrowVortex/saves`)
  when the folder next to the executable isn't writable (system-wide install).
  Allow the port in the firewall (ufw/firewalld).

## Build notes
- Build: VS 18 vcvars64 + ninja (`ninja` builds all incl. `format` check).
  clang-tidy runs with `--warnings-as-errors`; avoid C-style casts.
- Repo files are LF; don't let scripted edits convert to CRLF.

## Manual test (one machine)
Copy the song folder twice; run the exe twice; A: open song, wait for audio,
Shift+K → Name, Host; B: open the copy, Shift+K → Name, Join `127.0.0.1:7654`.
Check: B's title/offset/charts become A's; edits on either side appear on both;
moving the mouse shows a named cursor on the other side and a marker on the
scrub bar; A's Ctrl+Z never removes B's notes; B's Ctrl+Z reverts B's edits on
both. Start a third instance to check more than one guest.

Download: start B with no song open, Join → B saves the song to
`bin/saves/<folder>`, opens it and joins. Selection: Ctrl+A on B highlights
the notes in B's color on A.

## Ideas for phase 3
Chart creation/deletion sync, BG changes, IPv6, NAT traversal, hashing audio off
the main thread, selection/region sharing, chat.
