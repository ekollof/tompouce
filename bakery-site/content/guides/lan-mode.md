---
title: "Playing on a LAN"
description: "No server, no accounts: two Tompouces find each other."
weight: 4
---
On a local network you do not need this server at all. Whoever hosts first spawns a local director; the others find it over built-in mDNS with no addresses to type.

1. Host: `/collab host` (or start Tompouce with the host option). Your screen shows the 12-character join code.
2. Join: `/collab join` lists discovered directors with their fingerprints. Pick yours, type the code, and say the 16-character passphrase aloud — a second channel, same as the internet method.
3. Both screens show a fingerprint for out-of-band comparison, across the room.

Traffic stays on `ws://`, which is fine here because everything sensitive is already sealed end to end — same handshake as the internet path.
