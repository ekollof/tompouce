---
title: "Hosting a room"
description: "Create a room, mint invites, manage members."
weight: 1
---
## Create the room

Press F2 and type:

```
/collab host my-jam
```

You are now the owner. The owner link is shown once — it is the room's master key, so save it somewhere safe. Losing it means nobody can ever invite, kick, or rotate the room key again; there is no recovery and no support desk.

## Invite people

The easiest invite is a join code plus a passphrase. Mint one:

```
/collab invite --code
```

You get a 12-character code (`XXXX-XXXX-XXXX`) and a 16-character passphrase. **Split them across two channels**: read the code aloud or put it in the chat, and send the passphrase separately — a second message, a call, across the room. Anyone holding both can join.

Other useful forms:

```
/collab invite --code --uses 5      # one code, five joins (listeners by default)
/collab invite --ttl 2h             # expires after two hours (default 24 h, max 7 d)
/collab invite --qr                 # draw the invite as a terminal QR code
/collab invites                     # list invites
/collab revoke <id>                 # kill an invite; members already in stay in
```

A single-use editor invite is the default. Multi-use invites join as listeners.

## During the jam

- `/collab lock on` pauses joining without kicking anyone.
- `/collab kick <name>` removes someone and rotates the room key at once, so they cannot keep listening. `/collab ban <name>` also blocks rejoin.
- `/collab tempo 132` sets the tempo (owner only, unless you grant it). Plain `/bpm 132` does the same in a room.
- `/collab quantum 1` makes evals land on whole-cycle boundaries instead of instantly. Default is `live`.
- `/collab save <path>` exports the room as a song file.

## Ending

`/collab leave` leaves the room. Rooms with nobody in them are deleted after 14 days.
