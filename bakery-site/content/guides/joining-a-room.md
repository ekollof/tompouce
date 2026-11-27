---
title: "Joining a room"
description: "Links, join codes, and the consent prompt."
weight: 2
---
## What the host sends you

Either an **invite link** (starts with `https://bakery.andrath.nl/j/…`) or a **join code plus a passphrase** (the code looks like `K7QF-92XM-PD4T`). The link already contains everything; the code needs its passphrase, which arrives separately.

## Paste it into Tompouce

Press F2 and type one of these:

```
/collab join https://bakery.andrath.nl/j/…
/join https://bakery.andrath.nl/j/…
```

or for a code:

```
/collab join bakery.andrath.nl K7QF-92XM-PD4T
```

The passphrase is asked for in a hidden prompt. If your desktop registered the `tompouce://` handler at install time, clicking an invite link in a browser opens Tompouce with the same flow:

```
tompouce --join 'tompouce://join/bakery.andrath.nl/…'
```

## Read the consent prompt

Before anything connects, Tompouce shows the host, the room id, your role, and the **owner fingerprint**. Compare the fingerprint with the value the owner reads out — that is what defeats a swapped or forwarded link. One prompt at a time; nothing joins silently.

## Inside

- Your slots are yours; slot 0 is shared. F9 lists slots, Enter focuses one, F7 shows the roster.
- Evaluate with F5 as usual — everyone hears it at the same shared instant.
- `/collab leave` leaves. Rejoin with the same invite while it is still valid.
