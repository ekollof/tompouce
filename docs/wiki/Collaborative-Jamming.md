# Jamming together (Jam mode)

Tompouce has a Jam mode for playing together over the network: each player
runs their own Tompouce, and a small room server (the *director*) keeps
everyone's code and timing in sync. Everyone hears the same mix because
every client evaluates the same shared program at the same shared instant.

The public room server is [Bakery](https://bakery.andrath.nl/), with
step-by-step [setup guides](https://bakery.andrath.nl/guides/) you can
follow and paste straight into the client: hosting a room, joining with a
link or a join code, picking a nickname, playing on a LAN, and what
happens when a connection drops.

Jam mode is end-to-end encrypted by default. The server relays ciphertext
and never learns the room key; invite links carry the key in the URL
fragment, which browsers never send to any server. See
[how encryption works](https://bakery.andrath.nl/safety/),
[privacy](https://bakery.andrath.nl/privacy/) and
[rules & abuse](https://bakery.andrath.nl/abuse/).

## Status: early days

The collaboration stack is under active development. Working today:

- the standalone director (`tompouce-director`, built with
  `cmake --preset default` plus `-DWITH_DIRECTOR=ON`);
- the encrypted transport handshake between client and director;
- nicknames (`/nick`, F2), with a generated pronounceable default;
- staged program activation with a no-rewind late path (locally
  equivalent to Alt+Enter);
- per-slot capability gates for OSC and MIDI output;
- the sandboxed `/hydra` visualizer (the page that evals room code runs in
  a tokenless iframe);
- the Bakery front door and guides.

Still being built: rooms and invites, shared live editing, the room roster
(F7) and slot list (F9), remote code evaluation with the restricted
profile, the web invitation pages, and the Flok bridge. The full design
brief is [`docs/multiplayer.md`](https://github.com/ekollof/tompouce/blob/master/docs/multiplayer.md);
`TODO.md` in the repo tracks what is actively being worked on.

## Trying what exists

Build with collab support (needs libcurl 8.11 or newer with WebSocket
support; otherwise the feature stays off with a message):

```sh
cmake --preset default
cmake --build --preset default --parallel 4 --target tompouce-director tompouce
./build/tompouce-director --listen 127.0.0.1:7420
```

In another terminal, connect a client and check the handshake:

```sh
./build/tompouce --collab ws://127.0.0.1:7420/v1 --collab-nick your-name
```

`--join` parses an invite link without connecting yet. Joining rooms,
shared editing, and the `/collab` commands arrive with the pieces above.

## Giving feedback

Jam mode is shaped by people actually trying to play together. Bugs go in
[Issues](https://github.com/ekollof/tompouce/issues) with reproduction
steps; musical experiments, session reports and ideas go in
[Discussions](https://github.com/ekollof/tompouce/discussions). Latency
feel, solo behavior and the web invitation flow are the areas where
player feedback matters most right now.
