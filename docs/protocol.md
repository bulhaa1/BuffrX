# BuffrX engine protocol (alpha)

Public engine/client contract. Commands are registered in
`src/engine/protocol_commands.h`; `test_protocol_sync` checks this command table
against that registry. Keep both updated when adding or removing a command.

One transport, one format, no ambiguity. The engine is authoritative for all state; a client caches
nothing it can be told.

## Transport

- Named pipe `\\.\pipe\buffrx` — created by the engine with four instances, so several clients can connect at once
  (the editor, probes and scripts can all be connected).
- The engine's own interface (the panel, Settings, compact and the tray panel, drawn in-process with
  Dear ImGui) is a client too, without the pipe: it hands the same request frames to the same handler
  and receives every event a pipe client does, so a command behaves identically from both.
- Each frame on the wire: 4-byte little-endian payload length, then UTF-8 JSON.
- Request: `{ "id": <int>, "cmd": "<name>", "args": { … } }`
- Response: `{ "id": <int>, "ok": true, "data": { … } }` or `{ "id": <int>, "ok": false, "error": "<text>" }`
- Event (no id): `{ "event": "<name>", "data": { … } }`
- `hello` is the first request a client normally sends, but the engine does not require it; the reply
  carries `protocol`, `engine` version, `obs` version, `data_root`, `portable`, and `capabilities` — a
  list of command names this build supports. Clients render only what the capability list claims, so
  an older UI cannot break on a newer engine and the reverse.

## Commands (alpha set)

| Command | Args | Returns |
|---|---|---|
| `hello` | `client?` = `editor` (or `ui`, the in-process interface); `editor.open` uses it to reach an open editor | `protocol` (int), `engine` (version), `obs`, `data_root`, `portable`, `gpu{vendor,name}`, `capabilities[]` |
| `status.get` | — | `buffering`, `buffer_state` (`off`, `starting`, `on` or `stopping`), `game_wait` (true while games only holds the buffer for the next game: `buffer_state` is `off` and the buffer counts as on for the switch), `recording`, `recording_pending` (a record click waiting for the capture source's first frame), `recording_seconds`, `timeline{enabled,state,minutes,clip_open,writer}` (`clip_open` is always false for compatibility; `writer` means a Timeline session owns its output; a Timeline segment is never reported as `recording`), `capture{mode,target,display,display_requested,display_fallback,width,height,fps,refresh_hz}` (`display` is the GDI device name actually captured, `display_requested` the saved one, `display_fallback` true while the chosen screen is gone and the primary stands in; `target` names that primary in a fallback and is the folder label otherwise), `encoder_control{enabled,level,levels,effort,divisor,canvas_fps,fps,label}` (the dynamic encoder controller, plan 008: `level` 0 is the person's own settings and `levels` how many lie below it, `effort` 0-3 and `divisor` 1-3 are what runs, `canvas_fps` the rate the canvas ticks at and `fps` the rate the encoder keeps (canvas / divisor), `label` the words the panel shows such as `effort -1` or `120 -> 60 fps`, empty at level 0), `stats{capture_fps,encoder_fps,total_frames,dropped,restarts,saved_clips,video_frames,video_lagged,encoder_frames,encoder_behind,encoder_ratio,render_time_ms,active_fps,process_mb,ring{seconds,filled,bitrate_kbps,measured,used_bytes,held_seconds,avg_kbps,ram_bytes,disk_bytes,spilled_bytes,received_bytes,chunk_bytes,deduped_bytes,chunks_linked,chunks_waiting,disk,stalled},levels{desktop,mic}}` (`encoder_behind`: the encoder has encoded under 90% of the frames the video thread rendered, two 4 s windows in a row; `encoder_ratio`: the share it managed in the last window), `encoder`, `last_error`, `rebuilding` (true while the pipeline is being torn down and rebuilt), `is_elevated` (whether this process runs with administrator rights). The exclusion list left the status (about 110 fixed names, several KB, twice a second): ask `game.exclusions`. While no interface window is visible and no pipe client is connected the engine does not build the status at all; the moment one opens or connects a fresh `status` event is sent at once |
| `config.get` | — | full config object. Quality is one choice: `quality` (`low`\|`standard`\|`high`\|`highest`; a stored `custom` is read as `high` with a fixed peak), `peak_mode` (`auto`\|`fixed`) and `custom_kbps` (the fixed peak, used only while `peak_mode` is `fixed`), `encoder_speed_step` (read from an older config as the dynamic controller's starting level and never written; a `config.set` that carries it, as the Video page's "Try full quality again" does with 0, is a trigger: the controller forgets what this GPU settled at and the encoder goes back to the person's own settings; the level now running is `status.encoder_control`), `encoder_mode` (`auto`\|`fast`\|`balanced`\|`quality`\|`custom`: Fast/Balanced/Quality are the speed tiers whatever the quality level, Auto follows the level, Custom applies `encoder_options` on top of Auto; `encoder.set` makes the mode Custom), `encoder_governor` (the controller may lower the encoder's effort; default on), `display_method` (`auto`\|`dxgi`\|`wgc`) and `scale_filter` (`bilinear`\|`bicubic`\|`lanczos`, used when the output is smaller than the canvas), and nothing about memory: the ring keeps all of `buffer_seconds` at the quality chosen (`ring.seconds` is always the length asked for), and a saved clip is always that long. The ring holds at most 2 GiB of footage in RAM (three quarters of the safety ceiling, an eighth of the machine's RAM, on a small machine) and keeps the rest of a buffer that is longer than that in one reserved file beside the clips folder (`<clips_dir>\.buffrx\replay-<pid>.ring`, or `<data root>\ring` when that drive cannot take it), written in 8 MB blocks, read back by a save, removed when the buffer stops; none of it is a setting, and the engine never lowers the peak rate or cuts the length to fit memory (`ram_cap_mb` of an older config file is ignored). `ring.ram_bytes` is the footage in RAM, `ring.disk_bytes` what the file holds, `ring.spilled_bytes` what was written to it since the buffer started, `ring.received_bytes` what the encoder has handed the ring, `ring.disk` is `off` (the footage fits in RAM), `on`, or `unavailable` (the file could not be had; the ring then holds what memory allows), and `ring.stalled` is true while the disk could not take what was due (the footage waits in RAM). `ring.epoch` and `ring.swaps` count the encoder swaps the ring has been through (plan 008: the footage stays, each encoder's packets are an epoch); `ring.chunk_bytes`, `ring.deduped_bytes`, `ring.chunks_linked` and `ring.chunks_waiting` belong to the Timeline dedupe (off unless `BUFFRX_DEDUPE=1` while it is being proved; `BUFFRX_NO_DEDUPE=1` always turns it off): the footage held by Timeline chunk files instead of RAM or the ring file, the bytes that left RAM that way since the buffer started (and so were not written to the ring file), and how many probed chunks are matched to the ring / still waiting for a match. They are 0 with the dedupe off. `BUFFRX_NO_SPILL=1` gives the RAM-only ring, `BUFFRX_RING_RAM_MB=n` shrinks the RAM limit to n MB (probes, not settings). `ring.used_bytes`, `ring.held_seconds` and `ring.avg_kbps` are read from the ring itself once it has filled for a few seconds (`ring.measured`). A preset is a constant-quality target with a peak rate from the codec, size and frame rate. A version 3 file's `quality_tier`, `bitrate_kbps`, `duration_first`, `cqp` and `buffer_memory_mb` are translated on load and in any patch: a Fixed size rate becomes `custom`. A codec change carries a custom rate across by the codecs' bits per pixel. and `start_buffering` / `run_as_admin` (both off at the default except `start_buffering`, which is on) |
| `config.set` | `patch{}` (a bare object of fields is also accepted) | applied config object (partial patches merge). `ui_scale_percent` is 75–150, defaults to 100, and changes the native panel and quick panel without rebuilding capture; the expanded panel scales down when needed to fit the monitor work area |
| `config.reset` | — | applied config object (everything back to defaults) |
| `encoder.properties` | `configured?` | `{id, family, base?, properties:[{name,label,kind,min?,max?,step?,options?[],option_labels?[],value}]}`: the chosen encoder's own property list. `value` is what the encoder runs. With `configured:true` (and automatically while asleep, with no encoder started) it is the current plan with the governor's speed relief applied; otherwise it is the running encoder's own settings. `base` is the person's own choice and is present only on a configured reply, when the encoder exists: it holds one entry per listed property (names the encoder does not list never appear). Its values are the encoder's defaults with the plan's settings written over them before relief, through the same checks the encoder is built with, so a setting the encoder refuses shows the encoder's own default, a setting nobody set shows its real default, and a stored option that is refused does not appear as a choice. The plan includes the Performance layer, which both `value` and `base` follow. `hardware` (every reply): `{gpu, vendor, codecs[], available_codecs[], running_id?, reported_preset?, asked_preset?}`. `gpu` and `vendor` (`nvidia`\|`amd`\|`intel`\|`other`) are the adapter libobs renders on, `codecs` are the codecs that vendor's registered hardware encoders produce, `available_codecs` the codecs any registered hardware encoder produces plus `h264` (the CPU encoder): the Video page greys a codec outside it. While an encoder runs, `running_id` is its id, `reported_preset` the preset it logged in its own "settings:" block and `asked_preset` the preset BuffrX gave it; AMD's driver lowers the preset at init when its throughput caps say the card cannot keep up, and the page then says `Running: <reported> (the driver lowered it from <asked>)`. A stored `encoder_options` entry is not echoed here: the stored options stay in `config.encoder_options` (a null or a name no row lists is kept there as it is). When the Advanced page saves a row, the other rows it writes are their `base` values, never their `value`, so relief is not saved. |
| `encoder.set` | `name` (a libobs setting name from `encoder.properties`), `value` (any type; `null` clears it) | `encoder.properties` shape after the change; a value the encoder does not offer is refused and the encoder keeps its default |
| `diagnostics.report` | `copy?` | `{file, summary, copied?}`: the compatibility report (plan 009). Writes one file, `BuffrX compatibility report.json`, to the data root (overwriting an older one; the two files earlier builds wrote are deleted): `about` is the text summary, `report` the details. `copy: true` also puts the file on the clipboard as a file (CF_HDROP), so it pastes into Discord or a GitHub issue; `copied` says whether that worked. Returns the file's path and the summary. Local only: nothing is sent. Contents: BuffrX and libobs versions, Windows build, HAGS (`on`/`off`/`unknown`), whether two unlike adapters suggest a hybrid laptop, every DXGI adapter (vendor id, device id, name, driver, dedicated memory, the monitors it owns by device name, which one libobs uses and why), every registered video encoder id with its codec, whether it is hardware, the `rate_control` and `preset` values it lists and whether it lists `pre_analysis`, B-frames, `multipass`, `lookahead` and `target_usage` (read from the property list; no encoder is created), the running encoder's settings and the "settings:" block it logged at init, the controller's level and label, and the speed: with the buffer running, the encoder's encoded frames over rendered frames since it started (the controller's own counters; `speed.encoded_ratio`), with no wait; otherwise `speed` is `"not measured: start the buffer first"`. The file holds no file path, no game name, no window title and no user name (strings that look like a path are replaced by `(removed)`). Lock-free; it takes the engine lock only to read. Settings > System > Support > Copy report runs it with `copy: true`. `buffrx-engine --compat-report` does the same after libobs starts and exits 0 on success. |
| `sources.displays` | — | `[{id,name,device,width,height,primary}]`; `device` is the Windows GDI name (`\\.\DISPLAY1`), saved by `capture.set` as `display_name` so the choice survives a replug |
| `sources.windows` | — | `[{id,name,exe,minimized}]` |
| `sources.games` | — | `[{title,name}]` from the game-capture source properties |
| `capture.set` | `mode` = `display`\|`window`\|`game`, plus `display_id`/`window_id`/`game_title`; a display choice may carry `display_name` (the card's GDI device) beside its index: the pair is refused when the desktop changed, an index-only choice means today's display at that position | `status.get` shape |
| `buffer.start` / `buffer.stop` | — | `status.get` shape |
| `buffer.save` | `seconds?` (defaults to configured length) | `{queued, seconds}`; the file arrives in `clip.saved`, trimmed to the last `seconds` from the keyframe at or before, filed as `<clips>/<Game>/Replay <time>.<ext>` |
| `recording.start` / `recording.stop` | - | `status.get` shape. One press of the Record key either way (see below) |
| `screenshot.take` | `format?` = `png`\|`jpg` | `{file}` |
| `clips.list` | — | `[{file,name,size,mtime,duration?,thumbnail?,folder}]`: `folder` is the game ("Counter-Strike 2"), `Display Capture`, `Game Capture`, `Recordings`, `Edits`, or "" at the root. Thumbnails are generated only for the cards a client can draw (`thumbnail` is then a URL); the rest are asked for with `clips.thumbnail` |
| `clips.frames` | `file` | `{frames:[path]}` — up to 8 JPEG stills spread across one clip, cached beside its thumbnail, for the share sheet's looping preview; empty when the clip cannot be decoded. Same library check as `clips.thumbnail` |
| `clips.thumbnail` | `file` | `{thumbnail}` — the thumbnail URL for one clip, generated on demand; `ok:false, error:"that file is not in the BuffrX library"` for a path the library does not own. The path may be written with either separator |
| `clips.rename` / `screenshots.rename` | `file`, `name` | `{file}` |
| `clips.delete` | `file` | `{}` (permanent; internal and tests only) |
| `clips.trash` | `file` | `{}`: moves a clip to the Recycle Bin; refuses a path outside the library or a Timeline chunk, and never falls back to a permanent delete |
| `screenshots.trash` | `file` | `{}`: the same for a screenshot |
| `clips.reveal` | `file` | `{}` |
| `clips.open` | `file` | `{}` (shell opens the system player) |
| `screenshots.list` | - | same shape as `clips.list`; new screenshots carry their capture target as `folder`, older root files have an empty folder, and received screenshots have `folder` set to `Shared with me` |
| `storage.usage` | - | `{clips_bytes, clips_count, screenshots_bytes, screenshots_count, recordings_bytes, recordings_count, sessions_bytes, sessions_count, sessions_root, free_bytes, clips_dir, screenshots_dir}` |
| `sessions.list` | - | `{sessions:[{id, kind, app_id, profile, started_ms, ended_ms, closed, interrupted, bytes, chunk_count, marker_count}]}`, newest first |
| `session.get` | `id` | one session as above plus `chunks:[{file, bytes, start_ms, end_ms}]` and `markers:[{marker_id, at_ms, kind, label}]` (`marker_id` is the marker's own identity, `""` for one written before ids existed) |
| `timeline.get` | `session?` (an id from `sessions[]`; omitted takes the active writer, otherwise the newest available session), `list?` (`false` with a session reads that one index and leaves `sessions[]` out) | `enabled`, `state` (`off`\|`stopped`\|`writing`\|`paused_manual`\|`paused_rebuild`\|`paused_disk`\|`paused_no_game`\|`paused_idle`), `minutes` (the retention window), `chunk_seconds` (the nominal chunk length the writer was asked for), `current_session` (the writer's session id, or empty), `sessions:[{id,title,started_at,ended_at,bytes,published_chunks,duration_ms}]` newest first (`title` is the first game captured in it, "" for a desktop-only session; `ended_at` is 0 while it is open; `duration_ms` is the footage the index says is still in it, so a session whose chunks retention dropped reads shorter, and one whose writer has published nothing reads 0), `session` (the selected id, "" when there is none), `available_start_ms`, `available_end_ms` (the span the playable chunks cover), `bytes` (playable chunks only), `chunks:[{file,start_ms,end_ms,duration_ms,bytes,epoch,published}]`, where `epoch` moves only where the stream's layout changed (another encoder, picture size or number of audio tracks: a save never joins across it, and the strip draws the line; the encoder controller's effort and frame-rate steps keep the epoch), and `live:{file,start_ms,bytes}` for the chunk the writer holds right now (absent when nothing is being written, or when it belongs to another session). A `published:false` chunk is left out of `chunks[]`: its writer still holds the file. A published chunk whose file is no longer on disk is left out too, and so is it from the span - the page never gets a segment that will not decode, and a hole appears where the footage went. `live.bytes` is the writer's size right now (the file is opened to read it; a directory entry still says zero while the writer holds it), so the page can read the flushed fragments of the open chunk by byte range. Every time is milliseconds since the session began, never the index's raw tick clock |
| `timeline.thumbs` | `session?` (omitted takes the newest), `times_ms` (session clock, at most 24 used) | `session`, `thumbs:[{at_ms, file}]`: one JPEG per requested moment, `file` an absolute path under that session's own `thumbs` folder. Each moment is clamped into the published span, mapped to the published chunk that holds it, and decoded at the offset inside that chunk; two moments within the same second of one chunk are one picture, a moment in a gap has none, and a session with no published chunk returns an empty list. The picture is the cache: a repeat costs one stat, and at most 6 new JPEGs are made per call (the rest arrive on the next call). Only a published chunk on disk is ever decoded; a failing entry is left out silently rather than failing the command |
| `timeline.poster` | `session` (required) | `{session, file, at_ms}`: one JPEG for a session's card, the moment halfway through the footage that is still there. It is cached in that session's own `thumbs` folder and a repeat costs one stat. `ok:false` for a normal recording or a saved clip, for a session with no footage left, and when the frame cannot be decoded |
| `timeline.preview` | `session` (required) | `{session, forwarded:true}` when an editor is connected (it gets `editor.files` with the session's media folder), else `{session, pid}` of a new `buffrx-ui.exe --editor --preview` on that folder. The page recognises a folder under `sessions` and plays the session with the Timeline's own player. `ok:false` for a normal recording or a saved clip, and for a session with no playable chunk left |
| `timeline.edit` | `session` (required) | Opens the editor directly on a finished session, or forwards it to the existing editor. Returns `{session, pid}` or `{session, forwarded:true}`. Live footage is refused with guidance to trim and save a clip in Timeline. Takes an `editor:<session>` lease before the handoff; the editor renews and releases it. A lease left by a failed launch expires. |
| `timeline.edit_source` | `session` (required) | Returns the `timeline.get` snapshot for the editor. Live or empty sessions and missing indexed chunks are refused. The editor holds the lease while reading and exporting this snapshot. |
| `timeline.open` | `session` (required) | `{session, opened:true}`: the Timeline preview over the game, opened on that session, where a clip is cut and saved. `ok:false` when one is already open, and for a session with no footage left |
| `timeline.loudness` | `session`, `at_ms?` | `session`, `bucket_ms` (1000), `chunks:[{start_ms, levels:[0..255, …]}]`, `pending`: the sound of the published footage for the preview's loud-moment profile. `levels` is one byte per second of a chunk (RMS of its loudest audio track, -60 dBFS = 0, full scale = 255) from `start_ms` on the session clock. Each chunk is read once and the bytes are kept beside its thumbnails (`<chunk>.lv`), so retention that drops a chunk's pictures drops them too; at most 8 new chunks are read per call, starting at `at_ms` and then nearby chunks (newest first when omitted), and `pending` counts the chunks still unread (the page asks again until it is 0). No audio gives an empty `levels`. Only a published chunk on disk is read |
| `timeline.pin` | `session?` (omitted takes the newest), `start_ms`, `end_ms` (session clock), `owner?` (default `client`) | `{id, owner, session, start_ms, end_ms, ttl_ms, expires_ms}`: a lease of 10 minutes on a range, so retention keeps the files a preview, selection or export reads. The same `owner` pinning the same session renews that lease (the same `id`, the new span and expiry) rather than stacking |
| `timeline.unpin` | `id`, or `owner` (+ `session?`) | `{released}`: how many leases went. An unknown id releases nothing and is not an error |
| `timeline.save` | `session?`, `start_ms`, `end_ms` (session clock, the numbers `timeline.get` reports), `name?`, `notify?` (true: the engine's own notice says CLIP SAVED / CLIP NOT SAVED, for a preview that has already gone back to the game) | `{file, bytes, chunks, start_ms, cut_head_ms, gap_ms}`: the range stream-copied into one verified library clip (a remux when the container is not MP4), and a `clip` marker at `start_ms` in the session (`markers.changed`). A live range that reaches past the published edge asks the open segment to close and waits up to 10 s, then refuses with `"the newest moment is not saved yet"` rather than shortening. A range across a frame-rate change (the encoder controller's divisor, or the person's own setting) is joined whole, each part rescaled into the first one's time base. A range that spans a change the copy cannot carry (another codec, picture size or number of audio tracks) is stream-copied from the part in the layout that holds the range's end: `start_ms` is where the clip really starts on the session clock and `cut_head_ms` how much of the asked start it leaves out (0 when nothing was cut); the save notice then adds "saved from the settings change". `gap_ms` is the footage missing inside the saved range (a pause longer than 2 s between two chunks: the copy stitches across it, so the clip is that much shorter than the range; 0 when there is no hole). Nothing is re-encoded. The clip's audio labels describe the chunks the copy really holds: the dropped part is never read, and a join whose sound cannot be described (a full mix joined with only independent tracks) is refused before a clip is published |
| `timeline.erase` | `session` (required) | `{session, erased:true}`: stops that session's writer under the engine lock, then deletes its temporary store outside it. `ok:false` for no session, for a normal recording or a saved clip (only `kind:"timeline"` is ever erased), while an editor lease is active, while a save or a preview lease still reads the footage (the ring's own dedupe lease never blocks it), and while a file is still held - `error` names it and nothing is claimed removed |
| `markers.add` | `label?`, `kind?` (default `marker`); `session` + `at_ms` (session clock) mark that moment of a Timeline session, open or closed, instead of now in the open one | `{id, marker_id, at_ms, kind, label}`: `id` is the session, `marker_id` the new marker's own identity (8 random bytes as hex), so two markers at the same moment with the same kind and label are told apart; `ok:false, error:"no session open"` when nothing records into a session |
| `markers.list` | `id?` | `[{id, marker_id, at_ms, kind, label}]` across sessions, or one session; `id` is the session |
| `markers.remove` | `session`, and either `marker_id` or the old `at_ms` (session clock) + `kind` + `label` | Removes exactly that Timeline marker and emits `markers.changed`. A `marker_id` the session does not carry fails with `"marker not found"`; without one, the old identity removes the first marker matching it, which is what a client without ids can ask for. |
| `sessions.prune` | - | `{removed:[id], freed_bytes, total_bytes, budget_bytes}`: deletes the oldest finished sessions (crashed ones included) down to `recordings_budget_gb`; never the newest or the one being recorded; `budget_bytes` 0 means unlimited and removes nothing |
| `game.state` | - | `{observed:{exe, title}, auto_game_capture, capturing_game, active_game, exclusions[]}`; `observed` is the foreground window; `capturing_game` is the focus detector's state |
| `game.exclusions` | - | `{exclusions:[{name, builtin}]}`: what the product never treats as a game and what the user added (`game_exclusions`); read by Settings when the Games section opens and again after the user's list changes |
| `clips.export_range` | `id`, `start_ms`, `end_ms` (session clock), `name?` | `{file, bytes, chunks}`: the chunks covering the window stream-copied into one clip in `clips_dir`, from the keyframe at or before `start_ms`, verified before it is reported; refused for an unknown or still-recording session or a window with no footage. Its audio labels are decided from the chunks it copies, before any copy starts; a join whose sound cannot be described is refused before a file is published |
| `screenshots.reveal` | `file` | `{}` |
| `hotkeys.get` | - | `{save_replay, quick_clip, screenshot, toggle_record, toggle_ui, view_clip, add_marker}` keyboard chords, the same names with `_pad` for controller chords (`Pad:A+B`, empty when unbound), `problems{action: reason}` for keyboard chords Windows refused, `pad_problems{action: reason}` for a pad chord that does not parse or overlaps another, `controllers` (pads connected now) and `pad_available` (XInput was found) |
| `hotkeys.set` | `action`, `chord`, `input?` (`keyboard` by default, or `controller`) | updated map, as `hotkeys.get`. `action` is one of the seven above; `chord` is empty to clear that input's binding (a cleared binding is not a problem); a controller chord is `Pad:` and button names, in any case and order, stored in the canonical order, one to four buttons |
| `hotkeys.capture` | `on` | `{}`; while on, a controller press is reported as `hotkeys.captured` instead of running its action (the recorder does this while it listens) |
| `pad.navigate` | `on` | `{}`; while on, the pads are read even with no binding, so the interface can move on their buttons. Sent by the panel while one of its windows has the focus, and turned off when it hides or the recorder listens |
| `devices.list` | — | `{outputs:[{id,name,default}], inputs:[…]}` |
| `audio.get` / `audio.set` | full audio config | audio config: `desktop_enabled`, `desktop_volume`, `desktop_muted`, `desktop_device`, `mic_enabled`, `mic_volume`, `mic_gain_db`, `mic_muted`, `mic_device`, `mic_noise_suppress`, `mic_noise_gate`, `separate_tracks`, `audio_tracks` (1-6), `audio_bitrate_kbps`, `audio_capture_mode` (`auto`, `desktop` or `apps`; an unknown value is ignored), `audio_app_prefs` (automatic mode's per-program controls), `audio_other_volume` / `audio_other_muted`, and `apps`: `[{window?, exe?, title, kind?, track?, volume?, muted?}]`, **four slots at most**. `volume` is the program's own gain, 0..1 and 1.0 when the key is missing, and `muted` its own mute, false when missing: both are saved with the route and both apply live to the running capture, so a fader or a mute changes the sound of the frames recorded next without rebuilding the encoders, emptying the ring or splitting a recording; changing only these keys is never a routing change (a missing or unreadable gain records at full volume, never a NaN). A slot's track follows its position: the first app records on track 3, then 4, 5 and 6; track 2 is the microphone and track 1 the mix, and `track` is written for older builds only (it is ignored). `kind:"game"` is the legacy Current game slot: an older file may still hold one, it keeps recording the game BuffrX is actually capturing (never a detected candidate, the desktop or another program), and it keeps the same lane and the generic `BuffrX Game` label while the game changes, so no encoder is rebuilt and a clip spanning two games is one file. New selections do not offer it: Add app lists named programs only, and switching to Selected apps no longer seeds it; a saved game slot stays readable and can be removed or replaced. A game also selected by name makes no second capture (its lane stays a silent stem). `window` is the saved win-capture id, `exe` the stable identity a restart is matched on. `desktop` (the default) records the output loopback and the microphone and keeps `apps` saved without recording them. `apps` records only the listed slots and the microphone: the desktop loopback is not created (Windows cannot leave those apps out of it), and other programs' sound is not recorded. An empty `apps` list in `apps` mode is not a fallback to the desktop: nothing but the microphone is recorded. Adding a fifth slot is refused at this boundary; a legacy file with more than four keeps every entry, the first four record, and `status.audio.legacy_over` reports the rest (never silently dropped or rewritten). Changing `audio_capture_mode` or a recorded slot re-bits the mixer and adds encoders like a track change: the buffer restarts, and the change is refused while a recording runs. Changing `apps` while the mode is `desktop` does not restart the buffer. Performance mode's effective config keeps automatic mode's six tracks (the video preset must not merge the person's separate tracks); for a legacy `desktop`/`apps` file it is always `desktop` with one mixed track and the stored mode and slots are kept. The page's first switch to `apps` adds the Current game slot only while no application selection was ever written: `config.get` carries `audio_apps_chosen`, and any write of `apps` - including an empty list - sets it, so a list the person emptied is never refilled. See "Audio tracks in a recorded file" below |
| `audio.sessions` | - | `{sessions:[{pid, exe, active, system, peak}], error?}`: every audio session on every active render output, merged per program, with the session's own peak meter (`-1` when it exposes none). `active` is `AudioSessionStateActive`, which only means a stream is running; `peak` is the only evidence of sound. Cached for 250 ms; asked for only while the Audio page draws: four times a second with the app picker open, twice a second for the selected rows' activity marks, and never with the panel hidden or on another Settings page |
| `profiles.list` | `-` | `[{patch}]`, four seeded on first run |
| `profiles.add` / `profiles.save` | none / `index` | profile list; a patch carries only capture and encode settings (buffer, fps, size, stretch, codec, encoder, tier, rate control, encoder options); at most four |
| `profiles.update` | `index` + `patch{}` | profile list; updating the active profile applies it |
| `profiles.remove` | `index` | profile list; the last one stays |
| `profiles.apply` | `index` | `status.get` shape; `config.active_profile` becomes `index` |
| `clips.copy` | `files[]` (library files only) | `{}`; the files are on the clipboard as files (paste into Explorer, Discord) |
| `share.prepare` | `file` (library files only), `limit_mib?` (default `config.share_limit_mib`) | a share job: `{id, file, path, state, progress, copy, duration, source_bytes, bytes, limit_bytes, width, height, video_kbps, kept, used, error?}`. `state` is `fitting` (a `buffrx-engine --media-export` process re-encodes it below the limit, below normal priority, track 1 only) or `ready` (`copy:true`: the file already fits and is sent as it is). Refused when the clip cannot fit at any size (`fits` below 250 kbit/s). Progress arrives as `share.changed` |
| `share.get` | `id` | the job |
| `share.copy` | `id` (state `ready` or `posted`) | `{}`; the job's file is on the clipboard as a file. The job is `used`: its file outlives the sheet |
| `share.discord` | `id`, `message?`, `webhook_id?` | the job, `state: posting` and `destination`; then `share.changed` with `posted`, or back to `ready` with `error`. An omitted webhook id selects the first saved destination; an unknown id is refused. One HTTPS POST to the selected webhook in `share.json`, nowhere else (no redirects, TLS 1.2+); a 429 waits Discord's `retry_after` (10 s at most) and tries once more. `posted` follows Discord's confirmation (`wait=true`) |
| `share.cancel` | `id` | `{}`; a running fit or post stops, and a job nobody used has its file deleted (a used one stays until the next start clears anything a day old) |
| `share.webhook.get` | — | `{set, hint, webhooks:[{id, name, hint}], error?}`; names are entered by the user, ids are webhook ids, and hints show only the last four digits. Tokens never leave the engine |
| `share.webhook.set` | `url` (only `https://discord.com`, `ptb.`, `canary.` or `discordapp.com` `/api[/vN]/webhooks/<id>/<token>`), `name?`, `add?` | The destination list. With `add:true`, a name is required; an existing id is updated. Without it, the original command replaces the list. URLs are sealed with DPAPI for this Windows account in `<data root>/share.json`, never in `config.json`. Existing single-webhook files remain readable and migrate on the next write |
| `share.webhook.clear` | `webhook_id?` | The updated destination list; an omitted id retains the original command's clear-all behavior |
| `link.share` | `file` (library files only, up to 2 GB), `minutes` (10 to 10080; the sheet offers 60, 240, 720 and 1440) | `{id, url, expires, note?, existing?}`; a BuffrX link, served by `buffrx-share.exe` while it lives. `url` is the link page link, or its short form when the short-link service answered; `note` is one line naming why the long link was used instead (never silence). A file with a live link gets that link back (`existing:true`). The first link waits for the sharing helper to reach the network (up to 25 s) and the short form adds at most 2 s |
| `link.copy` | `id` | `{}`; copies an unexpired live link to the clipboard without offering this PC its own link |
| `link.stop` | `id` | `{}`; the link stops working at once |
| `link.list` | - | `{links:[{id, file, name, created, expires, viewers, watching, url}], receives:[{id, state, name, size, bytes, error?, file?, via?, attempt?, tries?, url?}], page_set, tick_age_ms?}`. `created` and `expires` are unix seconds; `created` is 0 for a link made before it was kept; `url` is the short form once the service answered, the long one otherwise. `via` is `direct` or `relay`, known from the moment the sharer's header arrives, so a slow transfer can say why. `tick_age_ms` is how long ago the link timer last ran: a stalled receive with a stale number here means the timer stopped, not the wire. A receive state is `connecting`, `asking` (over 300 MB), `receiving`, `retrying` (with `attempt` of `tries`), `done`, `failed` or `cancelled`; a finished one stays until it is dismissed with `link.cancel`, and `url` comes back on `failed` only, so the interface can offer Try again |
| `link.open` | `url` (`buffrx://l/…` or the link page's `https://…#…`) | `{id}`, or `{own:true, file}` for a link of this PC's own. The clip is fetched into `<data>\incoming`, checked, and moved into the library's `Shared with me`. A refusal is also published as a failed receive, because a click on a link has no reply to read |
| `link.answer` | `id`, `accept` | `{}`; a clip over 300 MB waits for this (`state:"asking"`) |
| `link.cancel` | `id` | `{}`; stops a download, or removes a finished one from the list |
| `app.pick_folder` | `title`, `start` | `{path}` ("" when cancelled); the system folder picker |
| `app.pick_file` | `title`, `start` | `{path}` ("" when cancelled); the system picker for one `*.exe` |
| `library.notify` | — | `{}`; broadcasts `library.changed` (the editor wrote a file) |
| `hotkeys.suspend` | `suspended` | `{}`; every hotkey is let go while the interface records a chord (lapses after 2 min) |
| `ui.show` | `view` = `main`, `tray`, `compact`, `hide` or `timeline` | `{view}`; the engine's native interface brings that view forward, or with `hide` closes its window (a second BuffrX launch sends `main`). `timeline` is the Timeline preview, a process of its own: the press Alt+4 makes (open the preview, on the newest footage or its empty state, or close one that is open) |
| `ui.share` | `file` (library files only) | `{file}`; broadcasts `ui.share{file}` and the native interface brings the panel forward with the Share sheet open on that file (the editor's clip preview uses it) |
| `ui.capture` | `view` = `main`, `compact`, `tray`, `settings:<section>`, `share` (the share sheet on the newest clip) or `current` (what is on screen, untouched), `file` (a PNG path) | `{file}`: the interface draws that view and saves it, so a panel regression is checked without hands; waits up to 15 s |
| `ui.input` | `window?` = `main`\|`tray`, and one of `click:[x,y]` (+ `button?`, `double?`, `stay?`), `move:[x,y]` (hover only), `keys:["Escape","E","F2","3"…]` (+ `mods?`: `ctrl`, `shift`, `alt`, joined by `+`), `chord:"Alt+4"` (to the hotkey recorder), `text`, `wheel:n` (notches, negative scrolls down; send with a `click` + `stay` so the pointer is over the page) | `{}`; input goes straight to the interface's own event queue, never to the system cursor, so a probe can drive it while someone uses the machine |
| `ui.frame_stats` | `reset?` (forget the numbers after reading them) | `{paced, game_running, passes, frames, gap_avg_ms, gap_p95_ms, gap_max_ms, inputs, latency_avg_ms, latency_p95_ms, latency_max_ms}`: how steadily the native interface drew since the last reset. A gap is the time between two presented frames (silences over 100 ms are idle time, not gaps); latency is the time from an input (a window message or `ui.input`) to the frame that shows it; `paced` says a visible window follows the display's refresh; `game_running` that a fullscreen app runs while a window is open, so a window that is not the focused one is capped at about 60 a second; `passes` counts the engine's main-loop passes (a busy wait would show as thousands a second). Also `presented` (every frame handed to the display: `frames` counts only gaps under 100 ms and misses a window drawing a few times a second), `skipped` (frames built and found identical to the one on screen, not presented), `build_avg_us` / `build_max_us` (NewFrame to Render), `open_ms` (the time from a window's show request to the frame that first appeared: the newest open, `tools/ui-frame-probe.ps1 -Mode open` reads it) and `reasons` (a count per cause of the frames drawn: `store`, `dirty`, `moving`, `settle`, `forced`, `images`, `anim:<name>`; `settle` is the one frame drawn 600 ms after the last input), `focused` (a visible window of ours is the foreground window: only it follows the display for pointer moves; an unfocused one draws data at most 30 a second and no hover), `latency_by_kind` (`{pointer,click,key,wheel,pad}` each `{inputs,avg_ms,p95_ms,max_ms}`), `digest_avg_us` / `draw_avg_us` / `present_avg_us` (the frame's own parts, averaged), `hitches` (`{over_12_ms,over_500_ms}`: window-thread passes over those times) with `worst` (`{ms,message,report}`: the longest pass, its last window message and the frame report) and `open_breakdown` (`{device_ms,window_ms,swap_ms,interface_ms,total_ms}`: where the last window open spent its time). `tools/ui-frame-probe.ps1` drives and reads it; `BUFFRX_NO_PACING=1` starts an engine with the old fixed-sleep loop for an A/B |
| `editor.open` | `files[]` (may be empty: the editor opens ready for a drop; each file must be in the library, as `clips.open` requires), `preview?` (play the clip first: the panel's clip click) | `{forwarded:true}` when an editor is already connected (it gets `editor.files`), else `{pid}` of the new `buffrx-ui.exe --editor [--preview]` process |
| `app.open_folder` | `kind` = `clips`\|`recordings`\|`screenshots`\|`data`\|`logs`\|`timeline`, `session?` (a Timeline session id; with `kind:"timeline"` it opens that session's media folder instead of the store's root) | `{}` |
| `app.open_url` | `url` | `{}`; opens one of BuffrX's GitHub, releases, Discord or website addresses. Other addresses are refused |
| `update.check` | `force?` (default `false`) | `{found, latest, notes, url, digest, error, checked_ms, install}`; checks GitHub on a worker, or returns this run's cached answer within 24 hours. `force:true` performs a manual check. `found:false` means the check failed and `error` explains why. `install` is `{state, percent, version, error}`: `idle`, `downloading`, `ready`, `installing`, `failed` or `manual` (a release without a digest opens in the browser instead). In a build with automatic updates, a newer release with a digest starts downloading here |
| `update.install` | none | `{accepted:true}` means the restart was queued. The engine verifies the installer and checks for a manual recording; the action worker checks again before stopping Timeline and the buffer. A recording started in between cancels the restart with a notice. A short-lived copy of the launcher waits for the engine to exit, runs Setup silently, and opens BuffrX again. Setup failure reopens the old app with an error. Missing or altered installers are refused and can be downloaded again |
| `update.notes` | none | `{found, version, notes, error}`; fetches notes for the running version from GitHub when requested. |
| `engine.restart` | (none) | `{accepted:true}`: the rebuild (libobs shutdown, initialize, and the buffer started again if it was running) is handed to the engine's action worker, so the reply is written before it begins and a `status` event follows when it ends. Refused with the usual error while a recording or a Timeline session is being written |

`status.get` includes `update` (`{state, percent, version, error}`, the same object `update.check` returns), so a window that opens after a download sees it is ready without asking GitHub. It also includes `stats.levels.active`, true when at least one audio meter is attached. The desktop meter scales
peaks from -60 dBFS (0) to 0 dBFS (1); the microphone meter scales RMS from -42 dBFS (0) to 0 dBFS (1), so quiet
background noise stays off its display. The desktop updates every 100 ms and the microphone every 200 ms. Meters attach
only while the expanded deck or Settings > Audio is visible; they detach when it is minimized, hidden, or switched to the
compact panel. These rules do not change audio capture.

## Audio tracks in a recorded file

A recording's mixer layout is the one the encoders were built with, never the live config: changing the
layout restarts the buffer, and is refused while a manual recording runs.

`status.get` carries the selected slots as they really are: `audio{mode, recorded, connected, problem, problem_ms,
problem_steady, game_exe, slots_used, slot_limit:4, legacy_over, slots:[{game, covered, track, exe, title, volume,
muted, state, allocated}]}`. `problem` is the same pure policy's result (`game_missing`, `game_waiting`, `all_unavailable`, or
empty) and `game_exe` the game the recording covers that it judged - the captured game in game capture, else the one
the engine's detectors see while the display shows it, and the game itself for a window capture of its window, never
an unrelated game behind a window capture of another program. That is not the Current game routing target: a display
capture with no game captured still names the game whose sound is missing. The page keeps saying what the one card
said after the card is gone (and while notices are off); `game_missing` covers an empty selection too, because a
captured game is not recorded when no slot covers it; `problem_ms` is how long it has lasted and `problem_steady`
turns true after 1.5 s, so a game that is still starting or a route that is still connecting never flashes a line
under the slots.
`state` is
`connected` (Windows has the process hook), `waiting` (allocated but not hooked, or the game lane with no
game), `failed` (the capture source could not be made), `covered` (the game is already captured by a named
slot) or `off` (desktop mode, or Performance mode's own mix). `recorded:false` while the saved slots are not
what is being written. A program that is merely closed is `waiting` and earns no notification; one card per
unresolved problem follows `audio_health.h` (a 10 s grace, once per game, cleared by recovery), and it honors
`config.notifications`.
`status.audio.auto` is the automatic mode the product now uses: `{on, watching, unsupported, error, sources,
performance_override, lanes:[{track, exe, name, state, volume, muted, other, apps:[{exe, name, state,
volume, muted}]}], uncaptured:[{exe,name,state,error}]}`. `on` is the setting and stays true on a quiet
desktop; `watching` is the event-driven session watcher, which runs only while the buffer, a manual recording or
Timeline wants sound, so `false` with `on:true` is quiet, not broken. `lanes` holds only what is visible now, on
tracks 3..5 for isolated programs and track 6 for `Other audio` (whose `apps` are its members, each with its own
effective volume and mute). A program with two independent process trees (two browser profiles) is one row, one
preference and one bus, and every tree is captured once; a tree that already sits inside another capture is not
taken twice. A program is added when Windows reports its session active and its row leaves about
1.5 s after it goes inactive or exits. `state` is `connected`, `waiting` or `failed`; a failure that means this
Windows cannot do process audio also sets `unsupported`, and it is never replaced by a desktop fallback.
`uncaptured` also reports a tree that was refused with `state:"blocked"`: Windows' process loopback takes the whole
tree below its root and cannot subtract one descendant, so a tree known to hold an excluded program, or BuffrX's
own playback, is stopped. The reason names the program. A child that starts under an already captured tree can be
recorded until a session event or the next process snapshot detects it; stable active trees are checked on the
existing audio poll after five seconds. Windows does not provide atomic per-descendant exclusions for this capture.
`performance_override` is false: automatic separation stays on under Performance mode. Automatic recordings are
always six tracks - mix, microphone, 3..5 and Other audio - whose sidecar sources are the stable bus ids
`auto:bus:3` .. `auto:bus:6`; a lane's title lists every occupant of that lane during the file's span
(`A + B`), so an old clip keeps its truthful names after the live row is gone, and the mix still decomposes
exactly into its stems for the editor.
`audio.get`/`audio.set` carry `audio_app_prefs` as an object keyed by lower-case executable with
`{volume:0..1, muted:false, excluded:false}`. A patch merges field by field and key by key: sending
one program's fader keeps every other program's mute and exclude. Legacy pin fields are ignored. `volume`,
`muted` and `excluded` all apply live - none rebuilds the encoder, clears the ring or cuts a manual
recording. `excluded` also checks descendants without their own session, subject to the detection interval above. `audio_other_volume` and
`audio_other_muted` are applied once to each Other audio member on top of its own preference. A program that
starts playing after capture is already running can lose the first few tens of milliseconds while its process
capture activates; the lane appears as soon as Windows reports the session, and no later sound is dropped.

The Current game lane's identity is fixed when the encoders start: `app:game` and `BuffrX Game` are written
even while no game is captured, so a game that appears later is a real stem in the sidecar and an editor
does not hide it or rebuild the mix without it. A different game keeps the same lane and label.

The container does not carry a stream title through OBS's `ffmpeg_muxer` (the muxer takes codec
parameters only), so a **multitrack** file (`audio_track_count > 1`) is accompanied by one sidecar
beside it:

`<file><ext>.buffrx-audio.json` (for example `Clip.mp4.buffrx-audio.json`)

```json
{
  "version": 2,
  "generator": "BuffrX",
  "media": {"size": 1048576, "mtime": 133900000000000000, "head": "0f1e…16 hex…", "tail": "…16 hex…"},
  "tracks": [
    {"index": 1, "role": "mix", "title": "BuffrX Mix", "sources": ["microphone", "app:Discord.exe", "app:cs2.exe"]},
    {"index": 2, "role": "microphone", "title": "BuffrX Microphone", "sources": ["microphone"]},
    {"index": 3, "role": "application", "title": "BuffrX Discord", "sources": ["app:Discord.exe"]},
    {"index": 4, "role": "application", "title": "BuffrX cs2", "sources": ["app:cs2.exe"]}
  ]
}
```

- `index` is 1-based and matched to the file's audio streams in container order (stream 1 = track 1).
- `role` is `mix`, `desktop`, `microphone`, `application`, `combined` (more than one source on the
  track) or `empty`.
- `sources` uses `desktop`, `microphone` or `app:<exe>`.
- `media` binds the labels to the exact bytes: the final size, the NTFS last-write time (FILETIME
  ticks), and FNV-1a 64 hashes of the first and last 16 KiB. It is captured only after a writer has
  closed, never while a file is growing. A replacement with the same stream count, size and time is
  still refused when its bytes differ.
- The sidecar describes recording time. It is written for replay saves, manual recordings (both the
  shared and the fallback encoder path) and Timeline chunk files; a single-track file gets none, and a
  file without one is never treated as a BuffrX mix. A chunk's labels are written pending when it
  opens and bound when its writer closes.
- Windows process loopback captures a process tree. Two routes whose programs share a tree (a
  launcher and the game it starts) would each contain both; BuffrX does not claim to separate those.
- A file's layout is read by `--media-info`, and only when the sidecar is version 2, its `media`
  identity still matches the file, and it names exactly its audio streams, in order, with known
  roles. Anything else (missing, a version 1 sidecar from before identity, stale after a re-mux or an
  external replacement, hand-edited, another program's) reads as an import: generic tracks, no assumed
  aggregate. A legacy sidecar is left on disk, not deleted or trusted.
- **The aggregate mix.** Track 1 is the whole mix, and the tracks after it are the sources it was mixed
  from. When every source of the mix is exactly one of those tracks, an editor may rebuild the mix from
  them, so a mute or a level is in the ordinary sound a player reads (track 1) and in that track on its
  own. A mix naming a source no track holds cannot be rebuilt: its recorded mix is kept when nothing is
  changed, and a level change is refused rather than written to a file whose ordinary sound would then
  be wrong. Nothing about this is inferred from the current capture settings or from the track count.
- A sidecar follows its file: a rename moves it (and refuses a target where another sidecar already
  sits), a trash recycles it, an export writes its own or removes one it owns, and a save or editor
  join from Timeline carries the layout of the chunks the copy really holds — a part dropped at a
  settings change is never read — or none. A file this product did not write at the sidecar name is
  never overwritten or deleted. Moving a clip and its sidecar elsewhere keeps the labels when size,
  time and the fingerprints still match; a copy outside BuffrX carries the sound without needing the
  sidecar: track 1 is the mix, so a player that ignores labels still plays the edit exactly once.

## Events

`status` (every 500 ms) · `clip.saved{file,name,folder,seconds}` · `screenshot.saved{file}` ·
`ui.show{view}` (`main`, `tray` or `compact`: bring that view forward; the native interface acts on it) · `editor.files{files,preview}` (to an open editor: a preview plays, otherwise the files are loaded for editing) · `app.quit{}` (tray Quit: the editor closes too) ·
`recording.changed{recording}` · `library.changed{}` · `sources.changed{}` ·
`timeline.changed{enabled,state,minutes,everywhere,game,clip_open,clip_seconds,writer}` (a writer state change, never one per chunk) ·
`markers.changed{id,at_ms,kind,label}` · `game.changed{decision,exe,title,handover}` (the focus detector entered or left game capture) · `share.changed{…the job}` (every state change and about every 2% of a fit or a post) ·
`hotkeys.captured{chord}` (a controller press while `hotkeys.capture` is on: the buttons held accumulate, `Pad:A+B`)
`link.changed{links, receives, page_set}` (the same shape as `link.list`, the same link fields) · `link.received{id, file}` (a clip from a friend is in the library; the panel plays it) · `error{component,message}` · `log{level,message}`

`config.changed{}` is sent whenever the configuration changes, whoever changed it: the panel, a
hotkey, a profile switch or another client on the pipe. A client that keeps its own copy of the
settings reads them again when it arrives - otherwise a settings page shows the old values after a
profile switch, and a change looks like it did nothing.

Session times (`started_ms`, `ended_ms`, a chunk's `start_ms`/`end_ms`, a marker's `at_ms`) are all the
engine's one tick clock (`GetTickCount`), not offsets; a display that wants "12 s in" subtracts
`started_ms`. The engine writes a marker itself for every clip save (`clip`), screenshot
(`screenshot`), recording start (`recording`) and profile apply (`profile`), from the protocol, the
tray and the hotkeys alike, while a session is open; with no session open they are dropped.

## Rules

1. The engine never blocks its capture path on a client: responses are written from a small worker,
   and a dead client is dropped, never waited on.
2. `buffer.save` is bounded: queue capacity is enforced, filenames are reserved (never overwritten),
   and a save that exceeds its timeout is reported as a partial file rather than silently lost.
3. Recovery is automatic: if the replay output stops unexpectedly, the engine restarts it and reports
   the restart as an event. A client should never have to tell the user to restart buffering.
4. The command transport is a local named pipe. Networking is limited to updates
   (the daily check and, when enabled, automatic installer download) and explicit
   sharing: Discord posting or BuffrX links. The sharing helper accepts encrypted
   connections only while a link is live or a clip is arriving.

The pipe is local only: it is created with `PIPE_REJECT_REMOTE`, so no connection crosses the network,
and it carries no token. Every process running as the same Windows user is trusted - the interface and
the editor are peers, not guests. The one command that writes a file on a client's word, `ui.capture`,
accepts only a path ending in `.png`, in any folder (probes write to `%TEMP%`).

## Local file delivery

The native interface reads thumbnails and screenshots itself (decoded through the runtime's FFmpeg).
The editor's page fetches them from the shell's schemes: `http://media.localhost/<path>` (clips and
screenshots, byte ranges, inside the configured folders only) and `http://thumbnails.localhost/<name>`
(the engine's cached thumbnails, by name). `clips.list` names a thumbnail by URL for that page; a client
that reads files maps the name into `<data_root>\thumbnails\`.

`recording.start` and `recording.stop` operate the standalone recording, regardless of Timeline state.
The Record hotkey toggles it. While Timeline writes, both outputs normally share the replay buffer's video
and audio encoders; the recording is written to its own file and continues when the game closes or Timeline
stops, subject to the selected capture source. Separate files add disk writes and muxing work. If shared
encoders are refused, the existing fallback uses separate encoders and pauses the buffer and Timeline.
`status.get` reports the manual recording as `recording`; `timeline.clip_open` and `clip_seconds` remain
for older clients and are always false and zero. Timeline clips are selected and saved in the Timeline UI.

`recording.start` is refused with a reason (and an `error` event) when the clips volume has less than
`min_free_gb` free. When the capture source has no frame yet (an idle engine released it), the request
is accepted with `recording_pending` true; the engine starts it on the first frame, up to 15 s.
Retention runs when the engine starts, before every recording and after every
session closes, but only when `recordings_budget_gb` is above 0. Session age is the wall clock
(`started_at`, seconds since 1970) — never the tick clock, which restarts at every boot.

### Timeline

Timeline writes only while the buffer runs and a game is detected. Detection uses the existing fullscreen
heuristic and exclusions even when automatic game capture is off; it does not enable automatic capture.
When a game remains running after Alt-Tab, the desktop may be recorded, including unmuted microphone audio.
`paused_idle` is the same for an everywhere Timeline (`timeline_everywhere`, below) that has seen no keyboard or mouse for `timeline_idle_minutes`.
`paused_no_game` means no new Timeline footage is being written; older footage remains available until retention
removes it. A standalone recording continues when the game exits until the user stops it. Sleep produces no
footage. After wake, capture is rebuilt when needed and Timeline waits for a detected game.
A program found by the scan is followed by its window: it stays a game while that window exists, or the program still has a
fullscreen window of its own, not while it has any window. `status.timeline.game` names the program Timeline is recording
as a game (empty unless `state` is `writing`); Settings offers "Not a game", which adds it to `game_exclusions` and lets it
go at once. `timeline_windowed_games` (default off) also takes the foreground window of a framed, not fullscreen program
when its process has loaded a Direct3D or Vulkan library together with a controller, sound or platform library
(`windowed_game.h`); a process that refuses to show its modules (anti-cheat) is not recognised, and that game is picked
under Capture. `timeline_everywhere` (default off) makes the desktop footage too: with no game running Timeline writes the display instead of waiting (`paused_no_game` never happens), and `status.timeline.everywhere` says so. `timeline_idle_minutes` (0, 5, 15, 30, 60; 0 never) pauses that desktop footage (`paused_idle`, its gap is labelled `Idle`) after that long without keyboard or mouse (`GetLastInputInfo`, nothing is hooked); a game is never paused for it, because a controller is not input to Windows. `timeline_keep_hours` (0, 1, 6, 24, 168) is read back from the config; with Timeline off the engine sweeps
footage older than it at start and then hourly.

The Timeline's footage lives in its own sessions (`kind:"timeline"`) under the sessions root, is never in
`clips.list` or the recordings budget, and expires by its own wall-clock window (`timeline_minutes`, 0 off,
30/60/120). `timeline.get` is the read: a bounded snapshot of the index, with the selected session's
`chunks` and the `available_start_ms`/`available_end_ms` the published chunks actually cover. A chunk is
published only after its writer has closed the file, so the open tail is never named as playable and an
`available_end_ms` past the newest published chunk is never reported. Every time is milliseconds since
the session began (`chunk.start_ms`/`end_ms` in the index are absolute ticks, which no client can use);
`timeline.save` takes the same numbers.

In the Timeline preview, seeking stays in the session being reviewed. Only Live returns to the current
writer. Scrolling into a gap moves to footage in the scroll direction; clicking a gap chooses the nearest
footage. Playback skips gaps, and an older session remains open when a new one starts until Live is chosen.

A reader that must not lose its files takes a lease: `timeline.pin` for 10 minutes, renewed by the same
`owner` on the same session while it is needed (`timeline.get` refreshes supply new footage; there is no
per-chunk event), and `timeline.unpin` on close. Leases are owner-scoped because a preview, a selection and
an export can hold the same footage for different reasons and release independently; an expired lease
protects nothing, so a crashed reader cannot keep old footage for ever. Retention never deletes a chunk an
active lease covers, and never an unpublished one. `timeline.changed` is emitted when the writer's state
changes (enabled, writing, paused, off), not for each new chunk.

`timeline.erase` is the one destructive Timeline command: it stops that session's writer under the engine
lock and deletes its temporary store outside it, with the files Windows still holds reported instead of
claimed as gone. The whole session is claimed under the retention gate before anything is deleted, so a
save, a preview or an editor lease granted first keeps the footage (the erase is refused and names the
reason) and a lease asked for during the erase is refused rather than granted over files that are going;
the ring's continuously renewed dedupe lease is ignored, so an explicit erase is never held out by the
buffer. It never touches a normal recording or a saved clip.

`timeline.thumbs` is the strip-of-frames command the preview no longer calls: the page draws one thin
track now, so today only `tools/timeline-e2e.ps1` exercises it. It decodes through the engine's existing
single-frame thumbnail (`media_thumbnail`), one call at a time under the media lock, cut at an offset inside
the published chunk that holds the moment. It runs outside the engine lock, never opens an unpublished chunk
or a missing file, and makes at most 6 new pictures per call, so a strip that is still filling cannot stall a
rotation, a save or the panel. The JPEGs are derived, not footage: they live in the session's own `thumbs`
folder, are deleted with the session (and with an expired chunk), and losing them costs one decode.


Idle sleep: with the buffer off and no recording, the capture and audio sources are released 15 s after the engine goes
idle, and the video canvas then drops to 5 fps with the outputs and encoders released (`status.get` `asleep: true`;
capture rates of 10 fps or less never sleep). `buffer.start`, `recording.start` and `screenshot.take` wake it and build
everything again from the config (31-190 ms; the buffer reports `on` 220-430 ms after `buffer.start`, against 90-335 ms
when the sources were only released).
`config.set` while asleep only stores the change - nothing is rebuilt until something wants frames, and `encoder.properties`
still reports the encoder that will run. Measured idle cost at 144 fps: 30.6% of a core awake, about 0.5% asleep.

Games only (`game_only`, with `auto_game_capture` on) captures nothing but games: a start with no game on screen waits (`game_wait`), the buffer pauses 5 s after the game is gone (a manual recording is never ended) and starts on the next fullscreen game; `buffer.stop` or turning the setting off ends the wait.

Game capture (`auto_game_capture`) samples the foreground: while the
buffer or a recording wants frames the engine samples the foreground window every tick (250 ms). A fullscreen,
non-excluded window (client area at least 90% of its monitor on both axes) that has held the foreground for 500 ms
enters game capture; flickers reset the clock. While capturing, an alt-tab to something that is not a game keeps the game,
the game's window closing leaves on the next sample, and a different fullscreen game takes over after 1.5 s as one
`game.changed{decision:"enter", handover:true}` (no trip back to the display). The capture target is swapped under the
running ring: the source being replaced stays under the new one until the new one has a frame (at most 4 s), so a game that
takes a second to hook leaves no gap in the footage. A buffer that starts while a game runs (`buffer.start`, or the engine's
own start) starts on that game: the fullscreen foreground game, else the one seen in the last minute that still runs, else any
fullscreen game already on screen.

Performance mode: `performance_when` (`off` | `games` | `always`) and `performance_games` (executables, lowercase, no `.exe`,
blanks and duplicates dropped) are config fields and never profile keys. `off` never uses the plan, `always` is the user's manual
switch that stays on when a game closes, and `games` is automatic: on only while a listed game is captured, off when it closes,
crashes or leaves game capture (needs game detection; an alt-tab keeps it on while the game is still captured). An old file with
no `performance_when` migrates: `performance_mode:true` becomes `always`, `false` with a non-empty `performance_games` becomes
`games`, otherwise `off`; `performance_fps` is gone and an old value is ignored. The engine layers the plan over the user's
settings (`apply_performance_plan`) and never rewrites them: while it is on the clip is 1920x1080 at 60 fps, H.264 on the
machine's own hardware encoder, the fast tier, bilinear scaling, one 128 kbps audio track and a fixed bit rate from
`preset_kbps`. The user's own Video settings come back when it stops. A change caused by a game coming or going waits while a
manual recording runs, and is a pipeline rebuild like any video change. `status.get` carries `performance{active,
reason: "always" | "game" | "off", width, height, fps, peak_kbps}` (the plan for this capture target, also when it is off).

The configurable `hotkey_marker` (default `Alt+5`, action `add_marker`) marks the current writing session. Its brief silent confirmation follows notification and fullscreen preferences.

Timeline preview: `hotkey_view_clip` (default `Alt+4`, never `Alt+F4`) opens the Timeline (`buffrx-ui.exe --timeline [session]`,
started by the engine, which keeps its process handle); pressed again it closes. It opens on the newest session with a
published chunk or, with none, on its empty state, which adopts the first session that appears. No plain key is ever
registered. Commands: `timeline.get {session?}`, `timeline.pin`, `timeline.unpin`, `timeline.save {session,start_ms,end_ms}`,
`timeline.erase`, and `ui.share {file}` for the clip it saved.

Notifications: `notifications` shows the engine's card; `notification_sound` plays a chime for a notice that asks for one
(clip or screenshot saved, recording started, recording stopped, a problem - four different sounds) and only then - and
a card skipped over a fullscreen game chimes even with the setting off, because it is then the only feedback;
`notify_over_fullscreen` (default false) draws the card over a fullscreen game as well - off, the card is skipped there
because a window on top can take the game out of independent flip, and warnings still draw. A saved clip or screenshot
leaves its card to the panel only when a BuffrX window is the foreground window and not minimized (a panel that is merely
open does not count); a failed save, quick clip or screenshot from a hotkey shows a warning card and the alert chime. Each
notice writes one log line saying what was decided (`notice "Clip saved": card=drawn chime=played`).

## Editor commands (engine command line, run by the editor process)

The editor never shares the engine's process: it runs `buffrx-engine.exe` for each job and the process exits when
the job does. Each prints one line of JSON (or `PROGRESS x` lines then one result line).

| Command | Output |
|---|---|
| `--media-info <file>` | `{ok, duration, width, height, fps, video_codec, video_kbps, bytes, modified, audio_buffrx, audio_aggregate, audio_complete, audio:[{index, channels, sample_rate, title, role, sources}]}` |
| `--media-keyframes <file>` | `{ok, keyframes:[seconds, ...]}`; up to 5000 video keyframes, sorted and unique |
| `--media-peaks <file> <buckets> [raw]` | `{ok, tracks:[[0..1, …], …], decoded:[bool, …], raw}`; one lane per audio stream in stream order. Without `raw` each track is normalised to its own loudest point (the older contract); `raw` keeps one absolute scale so lanes compare. `decoded[i]` is false when that lane's decoder did not open: its zeros are unreadable, not silence, and the ordinals still match the file's streams |
| `--media-loudness <file>` | `{ok, levels:[0..255, …]}`: one byte per second, the same bytes `timeline.loudness` keeps |
| `--media-export <job.json>` | `PROGRESS 0.42` lines, then `EXPORT_OK encoder=<name> <output>` or `EXPORT_FAIL <reason>` |

A job: `{input, output, start, end, segments?, crop_x, crop_y, crop_width, crop_height, out_width, out_height, quality, grade_brightness, grade_contrast, grade_saturation, volumes[], replace_original?}`.
A join uses `inputs` instead: `{output, inputs:[{input, start, end, segments?, crop_x, crop_y, crop_width, crop_height, grade_brightness, grade_contrast, grade_saturation, speed, volumes[]}, ...], quality?, out_width?, out_height?, fps?, audio_rate?, audio_channels?, video_codec?, video_encoder?, fit_pad?}`.
`segments` is `[[from, to], …]` in seconds: the parts of the input to keep, joined in order (the editor's split and
delete); without it the job keeps `start`..`end`. The three `grade_*` fields are percentages of the file's own
picture (100 is the file untouched), run through FFmpeg's `eq`. A single part may be stream-copied; an MP4 edit list
hides a short keyframe lead-in, with re-encoding as the fallback when the lead-in cannot be represented. A volume
change copies the video and re-encodes the audio. Crop, scale, grade, reduced quality, multiple parts, and speed
changes re-encode as needed. The file is written
as `<output>.part`, verified, then renamed. `replace_original` is the editor's Replace: the verified file is
moved beside the original, the original is renamed aside, the new file takes its place, and the old one goes to
the Recycle Bin; a failed move puts the original back. Without it, the output path is overwritten only after the
new file verifies.

A join takes the clips in `inputs` in play order. The first clip decides the size; `fps` (0 = its rate),
`audio_rate`, `audio_channels` (1 or 2), `video_codec` (empty keeps the source, `"h264"` forces H.264) and `fit_pad`
shape the common target. Each clip is trimmed and re-encoded to that target in `<output folder>\.buffrx\stitch\`,
with its own crop, grade, speed and per-track volumes applied in its own part before the common target; a
retimed clip contributes its kept parts divided by its speed to the join's length. `quality` (share of the
source bitrate) applies to every part. Every clip must have the same number of audio tracks,
or none; `video_encoder` pins the encoder after the first part, so concat never sees two different SPS/PPS.
