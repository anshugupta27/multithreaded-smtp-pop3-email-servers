# CIS 5050: Software Systems — HW2: Multithreaded Email Servers (SMTP + POP3)

**Author:** Anshu Gupta  
**SEAS Login:** anshuykg  
**Course:** CIS 5050, Spring 2026  
**Milestones:** MS1 due Feb 9 · MS2+3 due Feb 23, 2026

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Repo Name for Interviewers](#2-repo-name-for-interviewers)
3. [Files in This Submission](#3-files-in-this-submission)
4. [How to Build](#4-how-to-build)
5. [How to Run Each Server](#5-how-to-run-each-server)
6. [Architecture Overview](#6-architecture-overview)
7. [Milestone 1 — Echo Server](#7-milestone-1--echo-server)
8. [Milestone 2 — SMTP Server](#8-milestone-2--smtp-server)
9. [Milestone 3 — POP3 Server](#9-milestone-3--pop3-server)
10. [Shared Infrastructure](#10-shared-infrastructure)
11. [Concurrency & Locking Design](#11-concurrency--locking-design)
12. [Signal Handling (SIGINT / Ctrl+C)](#12-signal-handling-sigint--ctrlc)
13. [Mailbox Format (mbox)](#13-mailbox-format-mbox)
14. [Testing Guide](#14-testing-guide)
15. [Thunderbird Setup](#15-thunderbird-setup)
16. [Known Edge Cases Handled](#16-known-edge-cases-handled)

---

## 1. Project Overview

This assignment implements **three concurrent TCP servers** that together form a working email system:

| Server | File | Default Port | Protocol |
|--------|------|-------------|----------|
| Echo server | `echoserver.cc` | 10000 | Custom (ECHO/QUIT) |
| SMTP server | `smtp.cc` | 2500 | RFC 821 (simplified) |
| POP3 server | `pop3.cc` | 11000 | RFC 1939 (simplified) |

The **SMTP server** receives email from clients like Thunderbird, parses the SMTP protocol, and appends messages into local `.mbox` files on disk.

The **POP3 server** lets mail clients retrieve those messages — listing, reading, deleting, and computing unique IDs for each message in a user's mailbox.

Both servers are **fully concurrent** (thread-per-connection using pthreads), share the same mailbox directory, and use a **two-layer locking scheme** (pthread mutex + `flock`) to prevent corruption when SMTP and POP3 access the same mailbox simultaneously.

The **echo server** (Milestone 1) is the shared infrastructure template that both email servers are built on. Getting it right first — proper buffering, graceful shutdown, case-insensitive parsing — saves a lot of pain in Milestones 2 and 3.

---

## 2. Repo Name for Interviewers

```
multithreaded-smtp-pop3-email-servers
```

**multithreading**, **SMTP**, **POP3**, **server design** — all strong systems programming keywords.

---

## 3. Files in This Submission

| File | Purpose |
|------|---------|
| `echoserver.cc` | Milestone 1: multithreaded echo server (foundation) |
| `smtp.cc` | Milestone 2: SMTP server — receives and stores email |
| `pop3.cc` | Milestone 3: POP3 server — retrieves email from mailboxes |
| `Makefile` | Builds all three servers |
| `README` | This file |
| `description2.txt` | Protocol design notes and implementation summary |

> **Do not submit:** compiled binaries (`echoserver`, `smtp`, `pop3`), `.mbox` files, or any input/output test files.

---

## 4. How to Build

```bash
make
```

This produces three executables: `echoserver`, `smtp`, and `pop3`.

Under the hood, the Makefile compiles each `.cc` file with something like:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pthread smtp.cc   -lssl -lcrypto -o smtp
g++ -std=c++17 -O2 -Wall -Wextra -pthread pop3.cc   -lssl -lcrypto -o pop3
g++ -std=c++17 -O2 -Wall -Wextra -pthread echoserver.cc -o echoserver
```

The `-lssl -lcrypto` flags are needed for POP3's MD5 hash (used by the `UIDL` command).

To clean:
```bash
make clean
```

---

## 5. How to Run Each Server

### Echo Server (MS1)

```bash
./echoserver [-p port] [-v] [-a]
```

| Option | Meaning |
|--------|---------|
| `-p PORT` | Listen on PORT (default: 10000) |
| `-v` | Verbose debug output to stderr |
| `-a` | Print author name/login and exit |

```bash
./echoserver -p 10000 -v
```

### SMTP Server (MS2)

```bash
./smtp [-p port] [-v] [-a] <maildir>
```

| Option | Meaning |
|--------|---------|
| `-p PORT` | Listen on PORT (default: 2500) |
| `-v` | Verbose debug output to stderr |
| `-a` | Print author name/login and exit |
| `<maildir>` | **Required.** Directory containing `<user>.mbox` files |

```bash
# Create mailboxes first
mkdir mailtest
touch mailtest/alice.mbox
touch mailtest/bob.mbox

# Start SMTP server
./smtp -p 2500 -v mailtest
```

### POP3 Server (MS3)

```bash
./pop3 [-p port] [-v] [-a] <maildir>
```

Same options as SMTP. Default port is 11000.

```bash
./pop3 -p 11000 -v mailtest
```

> **Tip:** Run both SMTP and POP3 pointing at the same `<maildir>` directory. Send mail via SMTP, then retrieve it via POP3 (or Thunderbird).

---

## 6. Architecture Overview

```
┌─────────────────────────┐         ┌─────────────────────────┐
│     Thunderbird          │         │     Telnet / tester      │
│   (SMTP client)          │         │   (POP3 client)          │
└────────────┬────────────┘         └────────────┬────────────┘
             │ TCP :2500                          │ TCP :11000
             ▼                                    ▼
┌─────────────────────────┐         ┌─────────────────────────┐
│      SMTP server         │         │      POP3 server         │
│  main accept() loop      │         │  main accept() loop      │
│  → pthread per conn      │         │  → pthread per conn      │
│  HELO→MAIL→RCPT→DATA    │         │  USER→PASS→STAT/RETR...  │
│  mutex + flock(LOCK_EX)  │         │  mutex + flock held      │
└────────────┬────────────┘         └────────────┬────────────┘
             │ append                             │ read / rewrite
             └──────────────┬─────────────────────┘
                            ▼
               ┌────────────────────────┐
               │        maildir/         │
               │  alice.mbox  bob.mbox  │
               │  (mbox format on disk) │
               └────────────────────────┘
```

All three servers share the same basic structure: `make_listen_socket()` → `accept()` loop → `pthread_create()` per connection → `handle_client()` per thread. The email servers extend this with protocol state machines and mailbox I/O.

---

## 7. Milestone 1 — Echo Server

### What it does

The echo server is the skeleton that the email servers are built on. It listens on a TCP port, spawns a pthread for each connection, and handles two commands:

- `ECHO <text>` → `+OK <text>`
- `QUIT` → `+OK Goodbye!` then closes the connection
- Any unknown command → `-ERR Unknown command`

### Key design decisions

**Per-connection buffering.** TCP is a stream protocol — a single `recv()` call might return half a command, one full command, or three commands at once. The server maintains a string buffer per connection. Data is appended to the buffer as it arrives, and full `\r\n`-terminated lines are extracted and processed one at a time. This same buffering logic is reused in SMTP and POP3.

**Case-insensitive commands.** All commands are uppercased before comparison using a simple `toupper()` loop.

**Detached threads.** Each new connection spawns a `pthread_create()` with `pthread_detach()` so the main thread doesn't need to `join` each worker. Thread cleanup happens automatically when the thread function returns.

**Connection tracking.** All active file descriptors are kept in a global `set<int>` protected by a mutex. This is used during shutdown to send the goodbye message to all open connections.

### Verbose output format

When `-v` is enabled:

```
[4] New connection
[4] S: +OK Server ready (Author: Anshu Gupta / anshuykg)
[4] C: ECHO hello world
[4] S: +OK hello world
[4] C: QUIT
[4] S: +OK Goodbye!
[4] Connection closed
```

---

## 8. Milestone 2 — SMTP Server

### What it does

The SMTP server implements a simplified version of RFC 821. It receives email from mail clients (like Thunderbird), parses the SMTP command sequence, and appends each delivered message to the recipient's `.mbox` file on disk.

### SMTP State Machine

Every connection goes through these states:

```
CONNECTED
   │
   ▼  (send 220 greeting)
WAITING_FOR_HELO
   │
   ▼  HELO <domain>  → 250 OK
INITIAL  ◄────────────────────────── RSET resets here
   │
   ▼  MAIL FROM:<addr>  → 250 OK
MAIL_RECEIVED
   │
   ▼  RCPT TO:<user@localhost>  → 250 OK  (can repeat)
RCPT_RECEIVED
   │
   ▼  DATA  → 354 End data with <CRLF>.<CRLF>
READING_DATA
   │
   ▼  (receive "." on its own line)
   │  → lock mailbox → write mbox entry → unlock
   └──────────────────► INITIAL (ready for next message)
```

### Supported commands

| Command | Response | Notes |
|---------|----------|-------|
| `HELO <domain>` | `250 OK` | Required before MAIL FROM |
| `EHLO ...` | `500 Syntax error` | Rejected; Thunderbird falls back to HELO |
| `MAIL FROM:<addr>` | `250 OK` | Extracts sender address from angle brackets |
| `RCPT TO:<user@localhost>` | `250 OK` or `550 no such user` | Validates user.mbox exists |
| `DATA` | `354 ...` | Switches to data-reading mode |
| `RSET` | `250 OK` | Resets to INITIAL state |
| `NOOP` | `250 OK` | Does nothing |
| `QUIT` | `221 Bye` | Closes connection |

### Multiple recipients

The server accepts multiple consecutive `RCPT TO:` commands after a single `MAIL FROM:`. The message is delivered to all valid recipients. Duplicate recipients are deduplicated — the same user only receives the message once.

### Dot-unstuffing

As per RFC 821, if a line in the DATA body starts with a `.`, one dot is stripped before writing to the mailbox. A line containing only `.` terminates the DATA phase.

### What gets written to the mailbox

For each recipient, one entry is appended:

```
From <sender@domain> Mon Jan 20 12:34:56 2025
[entire DATA content as-is, without the final dot line]

```

The `From ` separator line (note the space, not a colon) is required by the mbox format and is added by the server — it is NOT part of the email content sent by the client.

---

## 9. Milestone 3 — POP3 Server

### What it does

The POP3 server lets mail clients retrieve messages from a user's `.mbox` file. It implements a simplified RFC 1939 with authentication, message listing, retrieval, deletion, and unique ID computation.

### POP3 States

```
AUTHORIZATION
   │  USER <name>  → +OK
   │  PASS cis505  → +OK (acquires mailbox lock, loads messages into memory)
   ▼
TRANSACTION  (mailbox locked for entire session)
   │  STAT  → message count + total bytes
   │  LIST [n]  → size of all / one message
   │  UIDL [n]  → MD5 hash IDs of all / one message
   │  RETR n  → full message content + ".\r\n"
   │  DELE n  → marks message as deleted (not yet removed)
   │  RSET  → undeletes all marked messages
   │  NOOP  → +OK
   ▼
UPDATE  (triggered by QUIT)
   │  Rewrites mailbox to temp file (undeleted messages only)
   │  Renames temp file over original
   │  Releases lock
   └──► connection closed
```

### Supported commands (after auth)

| Command | Response |
|---------|----------|
| `STAT` | `+OK <count> <total-octets>` |
| `LIST` | Multi-line: one `<n> <size>` per message |
| `LIST n` | `+OK <n> <size>` |
| `UIDL` | Multi-line: one `<n> <md5hex>` per message |
| `UIDL n` | `+OK <n> <md5hex>` |
| `RETR n` | Multi-line: `+OK`, full message, `.\r\n` |
| `DELE n` | `+OK` (marks deleted, deferred until QUIT) |
| `RSET` | `+OK` (unmarks all deletions) |
| `NOOP` | `+OK` |
| `QUIT` | `+OK` (commits deletions, releases lock) |

### UIDL — computing unique IDs

Each message's unique ID is computed as an **MD5 hash** over the entire message text — including the `From <sender> <date>\n` separator line that SMTP wrote. This ensures that two messages with identical body content still get different UIDs (since they were received at different times and from different senders). The MD5 digest is formatted as a 32-character lowercase hex string.

### Message deletion

Messages marked `DELE` are **not** removed immediately. They are only removed when the client sends `QUIT`. At that point, the server:

1. Writes all non-deleted messages to a temporary file in the same directory.
2. Renames the temp file over the original `.mbox` file atomically.
3. Releases the `flock` and the pthread mutex.

This approach is more fault-tolerant than editing the mailbox in place — if the server crashes before the rename, the original mailbox is untouched.

### Message sizes

The `LIST` and `STAT` commands report the size of each message in **octets (bytes)**. This count includes the raw message content exactly as written by SMTP, including `\r\n` line endings. It does **not** include the `From ` separator line (which is internal mbox metadata, not part of the message).

---

## 10. Shared Infrastructure

All three servers share these building blocks (copy-pasted and evolved from the echo server):

### `make_listen_socket(port)`

Creates a TCP socket, sets `SO_REUSEADDR` (prevents "address already in use" errors during testing), binds, and listens. Returns the listening file descriptor.

### `recv_line(fd, out)`

Reads bytes from `fd` one at a time until `\n` is found. Strips `\r`. Returns the command as a clean string. This handles the case where TCP delivers data in partial chunks.

### `write_all(fd, buf, len)`

Loops `send()` until all `len` bytes are written. Handles short writes (which can happen on slow or backpressured sockets).

### `register_conn(fd)` / `unregister_conn(fd)`

Thread-safe helpers to add/remove a connection file descriptor from the global tracking set. Used during shutdown to find all active connections.

### Thread entry point

```cpp
struct ThreadArgs { int conn_fd; bool verbose; };

void* client_thread(void* arg) {
    ThreadArgs* a = static_cast<ThreadArgs*>(arg);
    handle_client(a->conn_fd, a->verbose);
    delete a;
    return nullptr;
}
```

The main loop allocates `ThreadArgs` on the heap, passes it to `pthread_create`, and `pthread_detach`es the thread. The thread function frees the memory before returning.

---

## 11. Concurrency & Locking Design

### The problem

Multiple threads may try to write to the same `.mbox` file simultaneously (e.g., two SMTP connections both delivering to `alice.mbox`). The SMTP and POP3 servers may also access the same file at the same time.

### The solution: two-layer locking

**Layer 1 — Per-mailbox pthread mutex (intra-process):**

```cpp
static unordered_map<string, pthread_mutex_t*> g_mbox_mu;

pthread_mutex_t* get_mbox_mutex(const string& mbox_path) {
    // creates mutex on first access, returns existing one after
}
```

Each `.mbox` file has exactly one mutex associated with its path. Before any read or write, the thread acquires this mutex. This prevents two threads within the same process from corrupting the file.

**Layer 2 — `flock(LOCK_EX)` (inter-process):**

```cpp
int fd = open(mbox_path, O_RDWR | O_APPEND);
flock(fd, LOCK_EX);   // blocks until exclusive lock is granted
// ... read or write ...
flock(fd, LOCK_UN);
close(fd);
```

`flock` provides OS-level mutual exclusion between processes. This prevents the SMTP process and the POP3 process from accessing the same file simultaneously.

**Why both are needed:** `flock` is process-granular — it does not distinguish between threads within the same process. `pthread_mutex` is thread-granular but doesn't protect against other processes. Both together give full protection.

**Unlock order (important):**
```
flock(LOCK_UN)  →  close(fd)  →  pthread_mutex_unlock()
```

Releasing `flock` first lets the other process in before the thread mutex is released, which reduces contention between processes.

**POP3 session locking:**

In POP3, the lock is acquired at `PASS` and held for the **entire session** until `QUIT`. This is required by RFC 1939 — while a user's mailbox is open in a POP3 session, no other client should be able to modify it. SMTP delivery to that mailbox will block until the POP3 session ends.

---

## 12. Signal Handling (SIGINT / Ctrl+C)

When the user presses Ctrl+C, the OS delivers `SIGINT` to the process. The handler:

1. Sets a global `atomic<bool> g_shutting_down = true`.
2. Closes the listening socket (`g_listen_fd`), which causes the `accept()` call in the main loop to return with an error.

The main loop detects the error, checks `g_shutting_down`, exits the loop, and calls `shutdown_all_connections()`:

```cpp
// For SMTP:
send(fd, "421 Server shutting down\r\n", ...)
// For POP3 and echo:
send(fd, "-ERR Server shutting down\r\n", ...)
```

Every open connection receives its goodbye message before the socket is closed.

`SIGPIPE` is ignored (`signal(SIGPIPE, SIG_IGN)`) to prevent the server from crashing if a client disconnects mid-write.

---

## 13. Mailbox Format (mbox)

The `.mbox` files follow the standard **mbox format**, where messages are separated by `From ` lines:

```
From alice@localhost Mon Jan 20 12:34:56 2025
From: alice@localhost
To: bob@localhost
Subject: Hello

Hi Bob, this is the message body.

From charlie@localhost Mon Jan 20 13:00:00 2025
From: charlie@localhost
To: bob@localhost
Subject: Another message

Second message body here.

```

**Rules:**
- Each message starts with `From <sender> <ctime-formatted date>\n` (no colon after From).
- Everything after this line until the next `From ` line is the raw message data as sent by the SMTP DATA command.
- A blank line separates each message.
- The `From:` line inside the message body (sent by Thunderbird as an email header) is NOT the same as the `From ` separator. The separator has a space; the header has a colon.

The POP3 server uses the `From ` lines to parse individual messages out of the file when a session begins.

---

## 14. Testing Guide

### Echo server

```bash
# Start server
./echoserver -p 10000 -v

# In another terminal, test with telnet
telnet localhost 10000
ECHO hello world
QUIT

# Run the provided tester
cd test && ./tester_echo 10000
```

### SMTP server

```bash
# Setup
mkdir mailtest && touch mailtest/alice.mbox && touch mailtest/bob.mbox

# Start server
./smtp -p 2500 -v mailtest

# Test with telnet
telnet localhost 2500
HELO localhost
MAIL FROM:<test@example.com>
RCPT TO:<alice@localhost>
DATA
Subject: Test

Hello Alice!
.
QUIT

# Check the mailbox
cat mailtest/alice.mbox

# Run automated tester
cd test && ./tester_smtp 2500
```

### POP3 server

```bash
# Start POP3 (same maildir as SMTP)
./pop3 -p 11000 -v mailtest

# Test with telnet
telnet localhost 11000
USER alice
PASS cis505
STAT
LIST
UIDL
RETR 1
DELE 1
QUIT

# Run automated tester
cd test && ./tester_pop3 11000
```

### End-to-end test

```bash
# Terminal 1: SMTP
./smtp -p 2500 -v mailtest

# Terminal 2: POP3
./pop3 -p 11000 -v mailtest

# Terminal 3: send a test email
telnet localhost 2500
HELO test
MAIL FROM:<sender@example.com>
RCPT TO:<alice@localhost>
DATA
Subject: e2e test

This is a test message.
.
QUIT

# Then retrieve it
telnet localhost 11000
USER alice
PASS cis505
STAT
RETR 1
QUIT
```

---

## 15. Thunderbird Setup

To use a real email client:

**SMTP account setup:**
- Server: `localhost`, Port: `2500`
- Connection security: **None**
- Authentication: **No authentication**

**POP3 account setup:**
- Protocol: **POP3** (not IMAP)
- Server: `localhost`, Port: `11000`
- Username: `alice` (just the username, no `@localhost`)
- Connection security: **None**
- Authentication: **Normal password**
- Password: `cis505`

**Notes:**
- Thunderbird may try `EHLO` first. The SMTP server returns `500 Syntax error`, and Thunderbird correctly falls back to `HELO`.
- Click "Get Messages" manually to trigger a POP3 fetch.
- Uncheck "Check for new messages at startup/every 10 minutes" to avoid spurious connections during testing.

---

## 16. Known Edge Cases Handled

- **Partial TCP reads:** The per-connection buffer accumulates bytes until a full `\r\n` line is available before processing.
- **Multiple commands in one `recv()`:** The buffer loop processes all complete lines before returning to `recv()`.
- **Duplicate RCPT TO:** The server deduplicates recipients so the same user only receives one copy.
- **RCPT TO for nonexistent user:** Returns `550 no such user here` and stays in the current state.
- **EHLO:** Returns `500 Syntax error`; client falls back to HELO.
- **RSET before HELO:** Returns `503 Bad sequence of commands`.
- **DATA before MAIL/RCPT:** Returns `503 Bad sequence of commands`.
- **DELE on already-deleted message:** Returns `-ERR` message already deleted.
- **RETR 0 or negative index:** Returns `-ERR` invalid message number.
- **POP3 lock conflict:** If another session holds the mailbox lock, `PASS` returns `-ERR` mailbox locked.
- **Ctrl+C shutdown:** All open connections receive a graceful shutdown message before sockets are closed.
- **SIGPIPE:** Ignored globally — writing to a closed socket returns an error instead of killing the process.
