#!/usr/bin/env python3
"""Exercise production worker v2 authentication, deadlines, activation and cleanup."""
import errno
import hashlib
import hmac
import io
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

MAGIC = 0x31574146
VERSION = 2
PORT = 9142
TOKEN = b"0123456789abcdef" * 4
PROBE, STORE, EXTRACT, CLEAN, CLEAN_ALL, QUIT, ACTIVATE = range(1, 8)
READY, PROGRESS, COMPLETE = range(1, 4)
HELLO = struct.Struct("<II")
CHALLENGE = struct.Struct("<II32s")
AUTH_REPLY = struct.Struct("<IIi32s")
REQUEST = struct.Struct("<II32sQQ32s")
REPLY = struct.Struct("<IIIiQQQQQQ32sI")


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        block = sock.recv(size - len(data))
        if not block:
            raise EOFError("worker connection ended early")
        data.extend(block)
    return bytes(data)


def tag(challenge, domain, token=TOKEN):
    return hmac.digest(token, challenge + domain, "sha256")


def begin_auth():
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    sock.sendall(HELLO.pack(MAGIC, VERSION))
    magic, version, challenge = CHALLENGE.unpack(exact(sock, CHALLENGE.size))
    assert (magic, version) == (MAGIC, VERSION)
    return sock, challenge


def finish_auth(sock, challenge, proof):
    sock.sendall(proof)
    magic, version, result, server_proof = AUTH_REPLY.unpack(exact(sock, AUTH_REPLY.size))
    assert (magic, version) == (MAGIC, VERSION)
    if result == 0:
        assert hmac.compare_digest(server_proof, tag(challenge, b"S")), "worker impersonation"
    else:
        assert server_proof == bytes(32), "unauthenticated caller received server proof"
    return result


def connect(token=TOKEN):
    sock, challenge = begin_auth()
    return sock, finish_auth(sock, challenge, tag(challenge, b"C", token))


def request(sock, command, operation="", limit=0, expected=0, digest=bytes(32)):
    encoded = operation.encode("ascii")
    assert len(encoded) in (0, 32)
    sock.sendall(REQUEST.pack(MAGIC, command, encoded.ljust(32, b"\0"), limit, expected, digest))


def reply(sock):
    values = REPLY.unpack(exact(sock, REPLY.size))
    assert values[0] == MAGIC and values[1] == VERSION
    assert values[-1] <= 4096
    path = exact(sock, values[-1]).decode("utf-8") if values[-1] else ""
    return values, path


def complete(sock):
    while True:
        values, path = reply(sock)
        if values[2] == PROGRESS:
            continue
        assert values[2] == COMPLETE
        return values, path


def command(command_id, operation=""):
    with connect()[0] as sock:
        request(sock, command_id, operation)
        return reply(sock)[0]


def make_zip(entries=None):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in (entries or {
            "Good/Good.toc": "## Interface: 16001\n",
            "Good/Good.lua": "print('native')\n",
            "README.md": "fixture\n",
        }).items():
            archive.writestr(name, data)
    return output.getvalue()


def wait_absent(path, timeout=5):
    deadline = time.monotonic() + timeout
    while path.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert not path.exists(), f"abandoned native session retained: {path.name}"


def wait_jobs():
    deadline = time.monotonic() + 5
    while True:
        values = command(CLEAN, "f" * 32)
        if values[3] != -errno.EBUSY:
            return
        assert time.monotonic() < deadline, "native job lock was not released"
        time.sleep(0.02)


def store(operation, archive):
    digest = hashlib.sha256(archive).digest()
    sock, result = connect()
    assert result == 0
    with sock:
        request(sock, STORE, operation, len(archive))
        values, _ = reply(sock)
        assert values[2] == READY and values[3] == 0
        for offset in range(0, len(archive), 137):
            piece = archive[offset:offset + 137]
            sock.sendall(struct.pack("<I", len(piece)) + piece)
        sock.sendall(struct.pack("<IQ32s", 0, len(archive), digest))
        values, archive_path = reply(sock)
        assert values[2] == COMPLETE and values[3] == 0
        assert values[4] == values[5] == len(archive) and values[10] == digest
        assert archive_path.endswith(operation + "\\archive.zip")
    return digest


def extract(operation, archive, digest):
    with connect()[0] as sock:
        request(sock, EXTRACT, operation, expected=len(archive), digest=digest)
        values, _ = reply(sock)
        assert values[2] == READY and values[3] == 0
        progresses = []
        while True:
            values, extracted_path = reply(sock)
            if values[2] == PROGRESS:
                progresses.append((values[4], values[5]))
                continue
            assert values[2] == COMPLETE and values[3] == 0
            assert extracted_path.endswith(operation + "\\x")
            assert progresses and progresses[-1] == (values[6], values[6])
            return values


def activation_payload(channel, entries):
    encoded_channel = channel.encode("utf-8")
    payload = bytearray(struct.pack("<II", len(encoded_channel), len(entries)))
    payload.extend(encoded_channel)
    for source, name in entries:
        source, name = source.encode("utf-8"), name.encode("utf-8")
        payload.extend(struct.pack("<II", len(source), len(name)))
        payload.extend(source)
        payload.extend(name)
    return payload


def activate(operation, entries, channel="_retail_"):
    with connect()[0] as sock:
        request(sock, ACTIVATE, operation)
        sock.sendall(activation_payload(channel, entries))
        return reply(sock)[0]


def reset(sock):
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    sock.close()


def assert_closed(sock):
    try:
        assert sock.recv(1) == b"", "connection remains open past its absolute deadline"
    except (ConnectionResetError, BrokenPipeError):
        pass


def trickle_until_closed(sock, first, budget, interval=0.35):
    """Keep each read active; an SO_RCVTIMEO-only server would never terminate."""
    started = time.monotonic()
    sock.settimeout(0.15)
    sock.sendall(first)
    while time.monotonic() - started < budget + 3:
        try:
            if sock.recv(1) == b"":
                break
            raise AssertionError("unexpected response to partial frame")
        except socket.timeout:
            pass
        except ConnectionResetError:
            break
        try:
            sock.sendall(b"\0")
        except (BrokenPipeError, ConnectionResetError):
            break
        time.sleep(interval)
    else:
        raise AssertionError("trickle renewed an absolute deadline")
    elapsed = time.monotonic() - started
    assert budget - 1 <= elapsed <= budget + 3, elapsed
    sock.close()


def authentication_cases():
    bad, result = connect(b"f" * 64)
    with bad:
        assert result == -errno.EACCES
        assert_closed(bad)
    old, challenge = begin_auth()
    proof = tag(challenge, b"C")
    assert finish_auth(old, challenge, proof) == 0
    old.close()
    replay, fresh = begin_auth()
    with replay:
        assert fresh != challenge
        assert finish_auth(replay, fresh, proof) == -errno.EACCES
    reflected, fresh = begin_auth()
    with reflected:
        assert finish_auth(reflected, fresh, tag(fresh, b"S")) == -errno.EACCES
    # No raw bearer appears in any client frame, and a public hello or echoed
    # client proof is insufficient to impersonate the authenticated server.
    assert TOKEN not in HELLO.pack(MAGIC, VERSION) + proof
    assert not hmac.compare_digest(tag(challenge, b"C"), tag(challenge, b"S"))
    for version in (1, VERSION + 1):
        with socket.create_connection(("127.0.0.1", PORT), timeout=5) as bad:
            bad.sendall(HELLO.pack(MAGIC, version))
            assert_closed(bad)


def admission_cases():
    held = []
    try:
        # Receiving all challenges proves all 16 slots have been admitted.
        for _ in range(16):
            held.append(begin_auth()[0])
        with socket.create_connection(("127.0.0.1", PORT), timeout=2) as rejected:
            assert_closed(rejected)
    finally:
        for sock in held:
            sock.close()
    time.sleep(0.25)
    assert command(PROBE)[3] == 0
    slow = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    trickle_until_closed(slow, HELLO.pack(MAGIC, VERSION)[:1], 5, interval=1)


def deadline_cases(root, archive):
    # The fixed request and STORE chunk use distinct absolute frame budgets.
    partial, result = connect()
    assert result == 0
    trickle_until_closed(partial, REQUEST.pack(MAGIC, PROBE, bytes(32), 0, 0, bytes(32))[:1], 30)
    for number, trickle in ((0x31, False), (0x32, True)):
        operation = f"{number:032x}"
        sock, result = connect()
        assert result == 0
        request(sock, STORE, operation, 1 << 20)
        assert reply(sock)[0][2] == READY
        started = time.monotonic()
        # A stalled STORE must not block PROBE, and competing jobs fail boundedly.
        assert command(PROBE)[3] == 0
        assert command(CLEAN_ALL)[3] == -errno.EBUSY
        if trickle:
            trickle_until_closed(sock, struct.pack("<I", 1 << 20),
                                 30 - (time.monotonic() - started))
        else:
            sock.settimeout(33)
            assert_closed(sock)
            assert 29 <= time.monotonic() - started <= 33
            sock.close()
        wait_absent(root / operation)
    assert command(PROBE)[3] == 0


def cleanup_cases(root, archive):
    # Partial STORE, a complete STORE frame followed by request-side EOF, and
    # an extraction client disappearing all have native-owned cleanup.
    for number, finished in ((0x12, False), (0x13, True)):
        operation = f"{number:032x}"
        sock, result = connect()
        assert result == 0
        request(sock, STORE, operation, len(archive))
        assert reply(sock)[0][2] == READY
        sock.sendall(struct.pack("<I", len(archive)) + archive)
        if finished:
            # Flush the trailer together with FIN so EOF is already observable
            # when native finish publishes the archive (no scheduler race).
            sock.sendall(struct.pack("<IQ32s", 0, len(archive), hashlib.sha256(archive).digest()),
                         socket.MSG_MORE)
            sock.shutdown(socket.SHUT_WR)
            assert complete(sock)[0][3] == -errno.ECANCELED
            sock.close()
        else:
            reset(sock)
        wait_absent(root / operation)
    operation = f"{0x14:032x}"
    digest = store(operation, archive)
    sock, result = connect()
    assert result == 0
    request(sock, EXTRACT, operation, expected=len(archive), digest=digest)
    assert reply(sock)[0][2] == READY
    reset(sock)
    wait_absent(root / operation)
    operation = f"{0x15:032x}"
    digest = store(operation, archive)
    with connect()[0] as sock:
        request(sock, EXTRACT, operation, expected=len(archive), digest=digest)
        sock.sendall(b"\1")
        assert reply(sock)[0][2] == READY
        assert complete(sock)[0][3] == -errno.ECANCELED
    wait_absent(root / operation)
    operation = f"{0x16:032x}"
    store(operation, archive)
    with connect()[0] as sock:
        request(sock, EXTRACT, operation, expected=len(archive), digest=bytes(32))
        assert reply(sock)[0][2] == READY
        assert complete(sock)[0][3] != 0
    wait_absent(root / operation)
    wait_jobs()


def activation_cases(root, wow, archive):
    addons = wow / "_retail_/Interface/AddOns"
    addons.mkdir(parents=True)
    (addons / "Good").mkdir()
    (addons / "Good/old.lua").write_text("original\n")
    operation = f"{0x20:032x}"
    digest = store(operation, archive)
    assert extract(operation, archive, digest)[6] == 3
    assert (root / operation / "x/Good/Good.toc").is_file()
    values = activate(operation, [("Good", "Good")])
    assert values[2] == COMPLETE and values[3] == 0
    assert values[4] == values[5] == values[6] == 1
    assert (addons / "Good/Good.lua").read_text() == "print('native')\n"
    assert not (addons / "Good/old.lua").exists()
    assert command(CLEAN, operation)[3] == 0
    assert not (root / operation).exists()
    assert (addons / "Good/Good.toc").is_file()
    # Validate all destinations before moving the first folder.
    bad_archive = make_zip({"Good/Good.toc": "replacement", "Z..bad/Z..bad.toc": "bad"})
    operation = f"{0x21:032x}"
    digest = store(operation, bad_archive)
    extract(operation, bad_archive, digest)
    assert activate(operation, [("Good", "Good"), ("Z..bad", "Z..bad")])[3] != 0
    wait_absent(root / operation)
    assert (addons / "Good/Good.toc").read_text() == "## Interface: 16001\n"
    for number, entries, channel in (
        (0x22, [("../Good", "Good")], "_retail_"),
        (0x23, [("Good", "Good")], "../_retail_"),
        (0x24, [("Good", "Good"), ("Good", "good")], "_retail_"),
    ):
        operation = f"{number:032x}"
        digest = store(operation, archive)
        extract(operation, archive, digest)
        assert activate(operation, entries, channel)[3] != 0
        wait_absent(root / operation)
    # Complete valid payload and FIN arrive together: an abandoned request must
    # not replace an installed folder before its first cancellation check.
    operation = f"{0x29:032x}"
    digest = store(operation, archive)
    extract(operation, archive, digest)
    original = (addons / "Good").stat()
    with connect()[0] as sock:
        request(sock, ACTIVATE, operation)
        sock.sendall(activation_payload("_retail_", [("Good", "Good")]), socket.MSG_MORE)
        sock.shutdown(socket.SHUT_WR)
        assert complete(sock)[0][3] == -errno.ECANCELED
    wait_absent(root / operation)
    restored = (addons / "Good").stat()
    assert (restored.st_dev, restored.st_ino) == (original.st_dev, original.st_ino)
    assert (addons / "Good/Good.toc").read_text() == "## Interface: 16001\n"
    # An interrupted/invalid activation journal must never be silently erased.
    operation = f"{0x25:032x}"
    session = root / operation
    session.mkdir(mode=0o700)
    (session / "old").mkdir(mode=0o700)
    (session / "old/retained.lua").write_text("irreplaceable backup")
    (session / ".activation").write_text("invalid interrupted journal\n")
    assert command(CLEAN, operation)[3] != 0
    assert (session / "old/retained.lua").read_text() == "irreplaceable backup"
    assert command(CLEAN_ALL)[3] != 0
    assert session.exists()
    # The fixture deliberately has no recoverable real transaction.
    shutil.rmtree(session)

def interrupted_activation(root, wow, number, name, committed=False):
    """Reproduce the durable journal plus exact post-rename inode placements."""
    operation = f"{number:032x}"
    session = root / operation
    source = session / "x/Good"
    source.mkdir(parents=True, mode=0o755)
    session.chmod(0o700)
    (session / "old").mkdir(mode=0o700)
    (source / "new.lua").write_text("new")
    addons = wow / "_retail_/Interface/AddOns"
    target = addons / name
    target.mkdir()
    (target / "old.lua").write_text("old")
    new_info, old_info, addons_info = source.stat(), target.stat(), addons.stat()
    journal = session / ".activation"
    journal.write_text(
        f"AWACT1\n_retail_\n{addons_info.st_dev} {addons_info.st_ino}\n"
        f"Good\t{name}\t{new_info.st_dev} {new_info.st_ino} "
        f"{old_info.st_dev} {old_info.st_ino}\n"
    )
    journal.chmod(0o600)
    target.rename(session / "old" / name)
    source.rename(target)
    if committed:
        marker = session / ".activated"
        marker.touch(mode=0o600)
    return operation, target


def recovery_cases(root, wow):
    operation, target = interrupted_activation(root, wow, 0x26, "Recover")
    assert command(CLEAN, operation)[3] == 0
    assert not (root / operation).exists()
    assert (target / "old.lua").read_text() == "old"
    assert not (target / "new.lua").exists()
    operation, target = interrupted_activation(root, wow, 0x27, "Committed", committed=True)
    assert command(CLEAN, operation)[3] == 0
    assert not (root / operation).exists()
    assert (target / "new.lua").read_text() == "new"
    assert not (target / "old.lua").exists()


def wait_ready(server):
    deadline = time.monotonic() + 5
    while True:
        assert server.poll() is None, "worker exited during startup"
        try:
            assert command(PROBE)[3] == 0
            return
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.02)



def shutdown_ordering_case(executable, env, root, temporary):
    # Interpose actual filesystem cleanup, not production test hooks. Two pipe
    # notifications establish that CLEAN is paused and the main thread has
    # reached its handler-draining loop before checking exclusive port ownership.
    shim_source = temporary / "cleanup-gate.c"
    shim = temporary / "cleanup-gate.so"
    shim_source.write_text(r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_bool holding;
static atomic_bool notified;

static void notify(char event) {
    int fd = atoi(getenv("AW_TEST_ENTER_FD"));
    ssize_t result;
    do { result = write(fd, &event, 1); } while (result < 0 && errno == EINTR);
    if (result != 1) _exit(120);
}

int unlinkat(int directory, const char *name, int flags) {
    static int (*real_unlinkat)(int, const char *, int);
    if (!real_unlinkat) real_unlinkat = dlsym(RTLD_NEXT, "unlinkat");
    if (!real_unlinkat) _exit(121);
    if (!strcmp(name, "hold-cleanup")) {
        atomic_store(&holding, 1);
        notify('C');
        char release;
        ssize_t result;
        int fd = atoi(getenv("AW_TEST_RELEASE_FD"));
        do { result = read(fd, &release, 1); } while (result < 0 && errno == EINTR);
        if (result != 1) _exit(122);
        atomic_store(&holding, 0);
    }
    return real_unlinkat(directory, name, flags);
}

int nanosleep(const struct timespec *request, struct timespec *remaining) {
    static int (*real_nanosleep)(const struct timespec *, struct timespec *);
    if (!real_nanosleep) real_nanosleep = dlsym(RTLD_NEXT, "nanosleep");
    if (!real_nanosleep) _exit(123);
    if (atomic_load(&holding) && !atomic_exchange(&notified, 1)) notify('D');
    return real_nanosleep(request, remaining);
}
""")
    subprocess.run(["cc", "-std=c11", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
                    "-Werror", str(shim_source), "-o", str(shim), "-ldl"],
                   check=True, capture_output=True, timeout=30)
    entered_read, entered_write = os.pipe()
    release_read, release_write = os.pipe()
    gated_env = dict(env, AW_TEST_ENTER_FD=str(entered_write),
                     AW_TEST_RELEASE_FD=str(release_read),
                     LD_PRELOAD=":".join(filter(None, (str(shim), env.get("LD_PRELOAD")))))
    server = subprocess.Popen([executable], env=gated_env, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True,
                              pass_fds=(entered_write, release_read))
    clean = None
    try:
        wait_ready(server)
        operation = f"{0x30:032x}"
        session = root / operation
        session.mkdir(mode=0o700)
        (session / "hold-cleanup").write_text("pause native deletion")
        clean, result = connect()
        assert result == 0
        request(clean, CLEAN, operation)
        assert select.select([entered_read], [], [], 5)[0], "CLEAN never reached unlinkat"
        assert os.read(entered_read, 1) == b"C"
        assert command(QUIT)[3] == 0
        assert select.select([entered_read], [], [], 5)[0], "shutdown never drained handlers"
        assert os.read(entered_read, 1) == b"D"
        assert server.poll() is None and session.exists()
        with socket.socket() as replacement:
            replacement.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                replacement.bind(("127.0.0.1", PORT))
            except OSError as error:
                assert error.errno == errno.EADDRINUSE
            else:
                raise AssertionError("worker released port before native cleanup completed")
        os.write(release_write, b"1")
        assert complete(clean)[0][3] == 0
        assert server.wait(timeout=5) == 0
        assert not session.exists()
    finally:
        if clean is not None:
            clean.close()
        if server.poll() is None:
            server.kill()
            server.wait()
        stderr = server.stderr.read()
        for descriptor in (entered_read, entered_write, release_read, release_write):
            os.close(descriptor)
        shim_source.unlink()
        shim.unlink()
        assert TOKEN.decode("ascii") not in stderr
        if server.returncode != 0:
            raise RuntimeError(stderr)
    # A new worker can start only after the previous cleanup really completed.
    replacement = subprocess.Popen([executable], env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
    try:
        wait_ready(replacement)
        assert command(QUIT)[3] == 0
        assert replacement.wait(timeout=5) == 0
    finally:
        if replacement.poll() is None:
            replacement.kill()
            replacement.wait()
        stderr = replacement.stderr.read()
        assert TOKEN.decode("ascii") not in stderr
        if replacement.returncode != 0:
            raise RuntimeError(stderr)


def handoff_case(executable, env, root, wow, temporary):
    # Pause AFTER real COMPLETE delivery but BEFORE the owning mutex unlock.
    # A second pipe event proves the next handler actually contended for that
    # mutex before releasing the owner; this is not a scheduler-timing test.
    source = temporary / "handoff-gate.c"
    shim = temporary / "handoff-gate.so"
    source.write_text(r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static atomic_bool holding;
static atomic_int remaining = 3;
static __thread int completed;
static __thread int queued;

static void notify(char event) {
    int fd = atoi(getenv("AW_TEST_ENTER_FD"));
    ssize_t result;
    do { result = write(fd, &event, 1); } while (result < 0 && errno == EINTR);
    if (result != 1) _exit(120);
}

ssize_t send(int fd, const void *buffer, size_t size, int flags) {
    static ssize_t (*real_send)(int, const void *, size_t, int);
    if (!real_send) real_send = dlsym(RTLD_NEXT, "send");
    if (!real_send) _exit(121);
    ssize_t result = real_send(fd, buffer, size, flags);
    const unsigned char *bytes = buffer;
    if (result == 100 && size == 100 &&
        bytes[0] == 'F' && bytes[1] == 'A' && bytes[2] == 'W' && bytes[3] == '1' &&
        bytes[8] == 3 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 0 &&
        bytes[12] == 0 && bytes[13] == 0 && bytes[14] == 0 && bytes[15] == 0)
        completed = 1;
    return result;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    static int (*real_trylock)(pthread_mutex_t *);
    if (!real_trylock) real_trylock = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    if (!real_trylock) _exit(122);
    int result = real_trylock(mutex);
    if (result == EBUSY && atomic_load(&holding) && !queued) {
        queued = 1;
        notify('Q');
    }
    return result;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    static int (*real_unlock)(pthread_mutex_t *);
    if (!real_unlock) real_unlock = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    if (!real_unlock) _exit(123);
    if (completed && atomic_fetch_sub(&remaining, 1) > 0) {
        completed = 0;
        atomic_store(&holding, 1);
        notify('L');
        char release;
        ssize_t result;
        int fd = atoi(getenv("AW_TEST_RELEASE_FD"));
        do { result = read(fd, &release, 1); } while (result < 0 && errno == EINTR);
        if (result != 1) _exit(124);
        atomic_store(&holding, 0);
    }
    return real_unlock(mutex);
}
""")
    subprocess.run(["cc", "-std=c11", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
                    "-Werror", str(source), "-o", str(shim), "-ldl", "-pthread"],
                   check=True, capture_output=True, timeout=30)
    entered_read, entered_write = os.pipe()
    release_read, release_write = os.pipe()
    gated_env = dict(env, AW_TEST_ENTER_FD=str(entered_write),
                     AW_TEST_RELEASE_FD=str(release_read),
                     LD_PRELOAD=":".join(filter(None, (str(shim), env.get("LD_PRELOAD")))))
    server = subprocess.Popen([executable], env=gated_env, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True,
                              pass_fds=(entered_write, release_read))
    sockets = []

    def event(expected):
        assert select.select([entered_read], [], [], 5)[0], f"missing gate event {expected!r}"
        assert os.read(entered_read, 1) == expected

    try:
        wait_ready(server)
        (wow / "_retail_/Interface/AddOns").mkdir(parents=True, exist_ok=True)
        archive = make_zip()
        operation = f"{0x40:032x}"
        digest = store(operation, archive)
        event(b"L")
        queued, result = connect()
        sockets.append(queued)
        assert result == 0
        request(queued, EXTRACT, operation, expected=len(archive), digest=digest)
        event(b"Q")
        os.write(release_write, b"1")
        values, _ = reply(queued)
        assert values[2] == READY and values[3] == 0, (
            "STORE COMPLETE was delivered while its lock rejected the next EXTRACT", values[3])
        assert complete(queued)[0][3] == 0
        event(b"L")
        activate_sock, result = connect()
        sockets.append(activate_sock)
        assert result == 0
        request(activate_sock, ACTIVATE, operation)
        # Valid queued payload bytes are NOT a cancellation signal.
        activate_sock.sendall(activation_payload("_retail_", [("Good", "Good")]))
        event(b"Q")
        os.write(release_write, b"1")
        assert complete(activate_sock)[0][3] == 0
        event(b"L")
        clean, result = connect()
        sockets.append(clean)
        assert result == 0
        request(clean, CLEAN, operation)
        event(b"Q")
        os.write(release_write, b"1")
        assert complete(clean)[0][3] == 0
        wait_absent(root / operation)
        # A genuinely stalled owner still rejects boundedly without client retries.
        operation = f"{0x41:032x}"
        owner, result = connect()
        sockets.append(owner)
        assert result == 0
        request(owner, STORE, operation, len(archive))
        assert reply(owner)[0][2] == READY
        started = time.monotonic()
        assert command(CLEAN_ALL)[3] == -errno.EBUSY
        assert 1.8 <= time.monotonic() - started <= 3.5
        reset(owner)
        wait_absent(root / operation)
        assert command(QUIT)[3] == 0
        assert server.wait(timeout=5) == 0
    finally:
        for sock in sockets:
            sock.close()
        if server.poll() is None:
            server.kill()
            server.wait()
        stderr = server.stderr.read()
        for descriptor in (entered_read, entered_write, release_read, release_write):
            os.close(descriptor)
        source.unlink()
        shim.unlink()
        assert TOKEN.decode("ascii") not in stderr
        if server.returncode not in (0, -9):
            raise RuntimeError(stderr)


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--handoff-only"):
        raise SystemExit("usage: test-addon-native-worker-protocol.py SERVER [--handoff-only]")
    temporary = Path(tempfile.mkdtemp(prefix="forever-addon-protocol-"))
    root, wow = temporary / "staging", temporary / "wow"
    root.mkdir(mode=0o700)
    wow.mkdir()
    env = dict(os.environ, FOREVER_ADDON_WORKER_ROOT=str(root),
               FOREVER_ADDON_WORKER_WOW_ROOT=str(wow),
               FOREVER_ADDON_WORKER_TOKEN=TOKEN.decode("ascii"))
    if len(sys.argv) == 3:
        try:
            handoff_case(sys.argv[1], env, root, wow, temporary)
        finally:
            shutil.rmtree(temporary, ignore_errors=True)
        return
    # Host startup accepts a private inherited environment, never a token file.
    for invalid in (None, "f" * 63, "g" * 64):
        invalid_env = dict(env)
        if invalid is None:
            invalid_env.pop("FOREVER_ADDON_WORKER_TOKEN")
        else:
            invalid_env["FOREVER_ADDON_WORKER_TOKEN"] = invalid
        refused = subprocess.run([sys.argv[1]], env=invalid_env, capture_output=True, timeout=5)
        assert refused.returncode != 0
        assert TOKEN not in refused.stdout + refused.stderr
    # Refuse writable replacements without chmod-adopting them. Reject links in
    # every ancestor, not merely the final root component.
    root.chmod(0o777)
    refused = subprocess.run([sys.argv[1]], env=env, capture_output=True, timeout=5)
    assert refused.returncode != 0 and root.stat().st_mode & 0o777 == 0o777
    root.chmod(0o700)
    link = temporary / "linked"
    link.symlink_to(root, target_is_directory=True)
    linked_env = dict(env, FOREVER_ADDON_WORKER_ROOT=str(link / "child"))
    refused = subprocess.run([sys.argv[1]], env=linked_env, capture_output=True, timeout=5)
    assert refused.returncode != 0 and not (root / "child").exists()
    link.unlink()
    root.rmdir()
    server = subprocess.Popen([sys.argv[1]], env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
    try:
        wait_ready(server)
        assert not (root / ".token").exists()
        assert root.stat().st_mode & 0o777 == 0o711
        authentication_cases()
        admission_cases()
        archive = make_zip()
        cleanup_cases(root, archive)
        activation_cases(root, wow, archive)
        recovery_cases(root, wow)
        deadline_cases(root, archive)
        assert command(CLEAN_ALL)[3] == 0
        assert command(QUIT)[3] == 0
        assert server.wait(timeout=5) == 0
        # Startup CLEAN_ALL must recover an interrupted activation, not delete
        # the only old copy while leaving the uncommitted new tree installed.
        operation, target = interrupted_activation(root, wow, 0x28, "StartupRecover")
        first_stderr = server.stderr.read()
        assert TOKEN.decode("ascii") not in first_stderr
        server.stdout.close()
        server.stderr.close()
        server = subprocess.Popen([sys.argv[1]], env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        wait_ready(server)
        assert not (root / operation).exists()
        assert (target / "old.lua").read_text() == "old"
        assert not (target / "new.lua").exists()
        assert command(QUIT)[3] == 0
        assert server.wait(timeout=5) == 0
        shutdown_ordering_case(sys.argv[1], env, root, temporary)
        handoff_case(sys.argv[1], env, root, wow, temporary)
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()
        stderr = server.stderr.read() if server.stderr else ""
        shutil.rmtree(temporary, ignore_errors=True)
        assert TOKEN.decode("ascii") not in stderr
        if server.returncode != 0:
            raise RuntimeError(stderr)


if __name__ == "__main__":
    main()
