# nimd — Threaded Nim Game Server (NGP Protocol)

`nimd` is a TCP server that hosts multiple concurrent two-player Nim sessions using POSIX sockets and pthread threads.
Clients speak a simple framed text protocol (NGP). The server matches players into sessions, enforces turn rules, and
handles disconnects/forfeits.

## Features

- Concurrent games via a session registry (`sessions[]`) with reuse and dynamic growth
- Thread-per-connection (detached pthreads)
- Matchmaking: first connection is P1, second is P2; game starts once both successfully `OPEN`
- Strict framing and message validation (`recv_ngp_message`, `parse_client_message`)
- Graceful shutdown on SIGINT/SIGTERM; SIGPIPE ignored
- Disconnect handling: in-play disconnect triggers forfeit; sockets are shutdown to unblock reads

## Run

```bash
./nimd <PORT>
# example
./nimd 5050
```

## Concurrency Model

- Main thread: accept loop, assigns sockets to a `Game`, spawns detached threads
- Connection thread: reads framed NGP messages, enforces protocol ordering (`OPEN` first), processes moves, broadcasts updates

Synchronization:

- `registry_lock` protects the session registry and resizing/reuse logic
- Each `Game` has its own `lock` protecting sockets, names, board state, and state transitions

## Game Rules

- Piles start as: `1 3 5 7 9`
- A move removes `qty` stones from `pile` (1–5)
- Players alternate turns; taking the last stone wins
- Wrong-turn moves return `FAIL 31 Impatient` without ending the game

## NGP Protocol

### Framing

All messages are framed as:

```
0|LL|PAYLOAD
```

- Protocol id must be `0`
- `LL` is exactly two digits (payload length in bytes)
- Payload must end with `|`

Example:

```
0|09|WAIT|
```

### Client → Server

#### OPEN (must be first valid message)

```
0|LL|OPEN|<name>|
```

Constraints:

- `name` length: 1..72
- must be unique across active sessions

#### MOVE

```
0|LL|MOVE|<pile>|<qty>|
```

- `pile`: integer 1..5
- `qty`: integer >= 1 and <= stones in that pile

### Server → Client

#### WAIT

Sent after a successful `OPEN` while waiting for game start:

```
0|LL|WAIT|
```

#### NAME

Sent at game start to identify player number and opponent:

```
0|LL|NAME|<player_num>|<opponent_name>|
```

#### PLAY

Broadcast at game start and after each valid move:

```
0|LL|PLAY|<whose_turn>|<p1> <p2> <p3> <p4> <p5>|
```

#### FAIL

```
0|LL|FAIL|<code> <message>|
```

Codes used:

- 10 Invalid
- 21 Long Name
- 22 Already Playing
- 23 Already Open
- 24 Not Playing
- 31 Impatient
- 32 Pile Index
- 33 Quantity

#### OVER

Game ended (normal or forfeit):

```
0|LL|OVER|<winner>|<p1> <p2> <p3> <p4> <p5>||
0|LL|OVER|<winner>|<p1> <p2> <p3> <p4> <p5>|Forfeit|
```

## Typical Session Lifecycle

1. Client connects → thread starts
2. Client sends `OPEN|name|` → server responds `WAIT|`
3. Second client connects + `OPEN`
4. Server sends `NAME` to both, then `PLAY` with `whose_turn=1`
5. Players alternate `MOVE`; server broadcasts `PLAY` after valid moves
6. On terminal move, server broadcasts `OVER` and shuts down sockets
