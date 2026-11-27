---
title: "How encryption works"
description: "What the server sees, what it never sees, and how to verify."
---
## The one idea

Every room has a 32-byte key the server never learns. It travels to you inside the invite link's **fragment** (`#…`) — the part of a URL browsers never send to a server — or unwrapped from a join code by your passphrase on your own machine. Everything that would reveal what you play or who you are is sealed with that key: code, slot text, nicks, chat, sample lists.

## What the server sees

Opaque room and member ids, roles, who is connected, message sizes and timing, key epochs. That is enough to order evaluations and enforce permissions, and nothing more. Operational logs keep no room ids at info level and no keys, codes, or names. IP addresses are masked and kept for at most 7 days.

## How to verify people

- The consent prompt shows the **owner fingerprint**. Compare it with the value the owner reads out. If it matches, the invite was not swapped.
- Member lists carry short key fingerprints. Your client remembers names per room and warns if a known name appears with a new key.
- Every evaluation is signed by its author and checked against the room's certificates, so the server cannot forge code or invent members. A server that tries is detected and shown as `⚠ unverified`.

## Limits worth knowing

- A kicked member keeps everything sealed under keys they already held. Kicking always rotates the key, so they hear nothing new.
- The operator cannot judge content — by design it holds only ciphertext. Takedowns happen by room id on reports about conduct (flooding, abuse of the service), never by reading the room. See the [rules]({{< relref "/abuse.md" >}}).
- Remote code runs under a restricted profile on your machine: no local files, no sample fetches, no OSC or MIDI unless you allow that exact target. Your Hydra visuals show only your own slots unless you opt into more.
