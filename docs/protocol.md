# BuffrX engine protocol

This is the public command surface of the engine in this checkout. Its source of
truth is `src/engine/protocol_commands.h`; `test_protocol_sync` checks that the
command names here and the registry agree. Request arguments and response fields
are implemented in `src/engine/commands.cpp`. Update the registry, this reference
and affected clients together when changing the protocol.

## Transport

The Windows engine serves local clients over the named pipe `\\.\pipe\buffrx`.
Each frame is a four-byte little-endian payload length followed by UTF-8 JSON.
The native ImGui interface sends the same requests directly to the protocol handler.
The experimental Linux port adapts the transport; see its branch's implementation.

- Request: `{"id":1,"cmd":"status.get","args":{}}`
- Success: `{"id":1,"ok":true,"data":{}}`
- Failure: `{"id":1,"ok":false,"error":"explanation"}`
- Event: `{"event":"event.name","data":{}}`

Responses echo the request id. Events have no id. Send `hello` to discover the
build's `capabilities`; clients should check this list before using a command.
Engine JSON field names use snake_case. Keep calls off the window thread and obey
the engine lock policy; a lock-free command still has its own synchronization rules.

## Commands

The second column records whether the registry requires the engine lock. This
describes internal dispatch, not a locking responsibility delegated to pipe clients.
Some commands modify config, record media, delete files or access the network; inspect
their handlers and use isolated test state rather than invoking the list blindly.

| Command | Engine lock required |
| --- | --- |
| `hello` | Yes |
| `status.get` | Yes |
| `config.get` | Yes |
| `config.set` | Yes |
| `config.reset` | Yes |
| `sources.displays` | Yes |
| `sources.windows` | Yes |
| `sources.games` | Yes |
| `capture.set` | Yes |
| `buffer.start` | Yes |
| `buffer.stop` | Yes |
| `buffer.save` | Yes |
| `recording.start` | Yes |
| `recording.stop` | Yes |
| `screenshot.take` | Yes |
| `clips.list` | No |
| `clips.rename` | Yes |
| `clips.delete` | Yes |
| `clips.trash` | Yes |
| `clips.reveal` | No |
| `clips.open` | No |
| `clips.thumbnail` | No |
| `clips.frames` | No |
| `screenshots.list` | No |
| `screenshots.reveal` | No |
| `screenshots.rename` | Yes |
| `screenshots.trash` | Yes |
| `hotkeys.get` | Yes |
| `hotkeys.set` | Yes |
| `devices.list` | Yes |
| `audio.get` | Yes |
| `audio.set` | Yes |
| `profiles.list` | Yes |
| `profiles.add` | Yes |
| `profiles.remove` | Yes |
| `profiles.apply` | Yes |
| `profiles.save` | Yes |
| `editor.open` | No |
| `app.open_folder` | Yes |
| `app.open_url` | No |
| `update.check` | No |
| `update.notes` | No |
| `update.install` | No |
| `engine.restart` | Yes |
| `storage.usage` | Yes |
| `sessions.list` | Yes |
| `session.get` | Yes |
| `markers.add` | Yes |
| `markers.remove` | Yes |
| `markers.list` | Yes |
| `clips.export_range` | No |
| `sessions.prune` | Yes |
| `game.state` | Yes |
| `game.exclusions` | Yes |
| `timeline.get` | No |
| `timeline.pin` | No |
| `timeline.unpin` | No |
| `timeline.save` | No |
| `timeline.erase` | No |
| `timeline.thumbs` | No |
| `timeline.loudness` | No |
| `timeline.poster` | No |
| `timeline.preview` | No |
| `timeline.open` | No |
| `timeline.edit` | No |
| `timeline.edit_source` | No |
| `encoder.properties` | Yes |
| `encoder.set` | Yes |
| `ui.show` | Yes |
| `ui.share` | Yes |
| `ui.capture` | No |
| `ui.input` | No |
| `ui.frame_stats` | No |
| `profiles.update` | Yes |
| `clips.copy` | No |
| `app.pick_folder` | No |
| `app.pick_file` | No |
| `library.notify` | No |
| `hotkeys.suspend` | Yes |
| `hotkeys.capture` | Yes |
| `pad.navigate` | Yes |
| `share.prepare` | No |
| `share.get` | No |
| `share.copy` | No |
| `share.discord` | No |
| `share.cancel` | No |
| `share.webhook.get` | No |
| `share.webhook.set` | No |
| `share.webhook.clear` | No |
| `link.share` | No |
| `link.copy` | No |
| `link.stop` | No |
| `link.list` | No |
| `link.open` | No |
| `link.answer` | No |
| `link.cancel` | No |

## Working with the protocol

Use `tools/engine-ask.ps1` for a single request on Windows. Test new behavior with
the native regression harness and the affected UI/client. A successful logic test
does not prove capture, device or driver behavior; use the platform's development
guide for the relevant full-app checks.
