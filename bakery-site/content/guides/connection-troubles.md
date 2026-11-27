---
title: "Bad connections"
description: "What happens when the network misbehaves."
weight: 5
---
Short version for the artist: **the music does not stop.** If your connection drops, your client keeps playing the last shared program, shows a `✕` mark, and queues your edits. When you are back it resyncs by itself — usually within a scheduler window — and your queued edits merge in.

Details, so nobody panics mid-set:

- Drops are detected by socket errors or three missed pongs (about 6 s). Reconnects back off from half a second up to 30 s with jitter.
- A returning client replays what it missed and late-activates anything newer at the next window; the status line shows `late by …ms` on the author's side, not an error.
- If the server itself restarted, everyone re-authenticates, replays the stored snapshots, and re-anchors the clock. Saved rooms survive the restart.
- Someone slower than the room just hears the change a moment later. Nobody stalls the room.
- If you feel consistently behind, `/collab status` shows round-trip time and clock error, and `/collab latency [±ms|auto]` trims your personal output latency.

On a LAN none of this should bite. Across the internet, distance shows up as a slightly later shared instant for everyone — the room stays together either way.
