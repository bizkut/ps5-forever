#!/usr/bin/env python3
"""Build-only by default; --execute explicitly permits console contact.

Examples (run from the repository root):
  python3 ps5/writebroker/benchmark-writebroker.py --output /tmp/wb-native
  python3 ps5/writebroker/benchmark-writebroker.py --execute --mode native \
      --host CONSOLE_HOST --output /tmp/wb-native-run
  python3 ps5/writebroker/benchmark-writebroker.py --execute --mode broker-ab \
      --host CONSOLE_HOST --output /tmp/wb-ab \
      --baseline-broker /path/to/original.elf --candidate-broker /path/to/tuned.elf \
      --restore-broker /path/to/exact-installed-broker.elf
  # On a trusted LAN host, not through the slower host-console upload link:
  python3 ps5/writebroker/benchmark-writebroker.py --execute --mode ftp-network \
      --host CONSOLE_HOST --output /tmp/wb-lan-ftp

Native buffers are generated ON-console. Native io_us times only write/pwrite
syscalls; preparation, verification, fsync and close are reported separately.
The native matrix is authid ABBA x 32K/256K/1M/4M x write/pwrite x growing
and materialized-overwrite files x 1/4 writers.
The broker matrix is 32K/256K/1M/4M x 1/4/8 writers x default/4M client windows.
Its latency is an on-console synchronous loopback request, not isolated disk I/O
and not host-console RTT. Server A/B uses exact caller-supplied ELFs (same wire
version); restoration is mandatory and performed even after a failed run.

FTP runs from the invoking host, generating its data in memory: one 1 GiB STOR,
then four parallel 256 MiB STORs (also 1 GiB total). Run ftp-network on the
trusted LAN host. This is a NETWORK reference, never an isolated disk
ceiling; native write/authid results determine the disk-path cost.
No SSH, host-key changes, title launch/kill, title installation or live tuning.
Execution refuses any running title, creates one fresh scratch subdirectory,
and retains failed-run files/logs. All uploaded disk data is synthetic.
"""
import argparse
import datetime
import concurrent.futures
import ftplib
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import threading
import time
import uuid

SCRATCH = '/data/forever/prefix/drive_c/scratch-bench'
GIB = 1 << 30


def digest(data):
    return hashlib.sha256(data).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def control(host, command):
    with socket.create_connection((host, 9111), timeout=10) as connection:
        connection.sendall((command + '\n').encode())
        response = bytearray()
        while b'\n' not in response:
            data = connection.recv(65536)
            if not data:
                break
            response.extend(data)
            if len(response) > 65536:
                raise RuntimeError('Oversized title-control response')
    return response.decode(errors='replace')


def idle(host):
    response = control(host, 'procs')
    if not response.startswith('ok procs ') or not re.search(r'\bcount=0\b', response):
        raise RuntimeError('Refusing console mutation while title state is not idle: ' + response)
    return response


def space_guard(args, label):
    with ftp(args.host) as client:
        response = client.sendcmd('AVBL /data')
    match = re.fullmatch(r'213 ([0-9]+)', response)
    if not match:
        raise RuntimeError('Unparsed free-space response: ' + response)
    available = int(match[1])
    save(args.output / (label + '-space.json'), {'available_bytes': available,
                                               'maximum_scratch_bytes': GIB})
    if available < 2 * GIB:
        raise RuntimeError('Refusing benchmark with less than two GiB free')


class FTP(ftplib.FTP):
    def ntransfercmd(self, command, rest=None):
        connection, size = super().ntransfercmd(command, rest)
        for option in (socket.SO_SNDBUF, socket.SO_RCVBUF):
            try:
                connection.setsockopt(socket.SOL_SOCKET, option, 4 << 20)
            except OSError:
                pass  # Keep the usable OS default, as on the FTP server.
        self.last_buffers = {
            'send': connection.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF),
            'receive': connection.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
        }
        return connection, size


def ftp(host):
    client = FTP()
    client.connect(host, 2121, timeout=120)
    client.login()
    client.voidcmd('TYPE I')
    return client


class Capture:
    def __init__(self, host, needle):
        self.host, self.needle = host, needle
        self.ready, self.done, self.stop = threading.Event(), threading.Event(), threading.Event()
        self.rows, self.errors = [], []
        self.thread = threading.Thread(target=self.collect, daemon=True)

    def collect(self):
        try:
            with socket.create_connection((self.host, 3232), timeout=10) as connection:
                connection.settimeout(0.3)
                # Drain only old klog bytes before declaring the capture ready.
                while True:
                    try:
                        if not connection.recv(262144):
                            raise RuntimeError('Klog closed while draining')
                    except socket.timeout:
                        break
                self.ready.set()
                connection.settimeout(0.5)
                recent = b''
                while not self.stop.is_set():
                    try:
                        data = connection.recv(262144)
                    except socket.timeout:
                        continue
                    if not data:
                        raise RuntimeError('Klog closed during capture')
                    self.rows.append(data)
                    recent = (recent + data)[-8192:]
                    if self.needle in recent:
                        self.done.set()
        except Exception as error:
            self.errors.append(str(error))
        finally:
            self.ready.set()

    def start(self):
        self.thread.start()
        if not self.ready.wait(15) or self.errors:
            raise RuntimeError('Klog capture unavailable: ' + repr(self.errors))

    def finish(self):
        self.stop.set()
        self.thread.join(15)
        if self.thread.is_alive():
            self.errors.append('Klog reader did not stop')
        return b''.join(self.rows)


def deploy(host, elf):
    with socket.create_connection((host, 9021), timeout=20) as connection:
        connection.sendall(elf)
        connection.shutdown(socket.SHUT_WR)


def run_payload(args, label, elf, broker=False):
    guard = idle(args.host)
    capture = Capture(args.host, b'writebroker: listening on 127.0.0.1:' if broker
                      else b'writebroker-bench: done ')
    started = time.monotonic()
    failure = None
    completed = False
    try:
        capture.start()
        deploy(args.host, elf)
        completed = capture.done.wait(180 if broker else args.timeout)
        if not completed:
            raise RuntimeError('Payload completion marker timed out: ' + label)
    except Exception as error:
        failure = error
    finally:
        log = capture.finish()
        (args.output / (label + '.klog')).write_bytes(log)
        records = []
        for match in re.finditer(r'writebroker-bench: result ([^\r\n]+)', log.decode(errors='replace')):
            row = {}
            for key, value in re.findall(r'(\w+)=([^ ]+)', match[1]):
                try:
                    row[key] = float(value) if '.' in value else int(value)
                except ValueError:
                    row[key] = value
            if row.get('kind') == 'native' and row.get('threads') == 1 and row.get('io_us'):
                row['syscall_MiBs'] = row['bytes'] * 1000000 / row['io_us'] / (1 << 20)
            records.append(row)
        receipt = {'guard': guard, 'completed': completed, 'host': args.host,
                   'payload_sha256': digest(elf), 'klog_sha256': digest(log),
                   'host_elapsed_seconds': time.monotonic() - started,
                   'capture_errors': capture.errors, 'records': records,
                   'error': str(failure) if failure else None}
        save(args.output / (label + '.json'), receipt)
    if failure:
        raise failure
    if capture.errors:
        raise RuntimeError('Incomplete klog evidence: ' + repr(capture.errors))
    if not broker:
        expected = 24 if records and records[0]['kind'] == 'broker' else 128
        if (len(records) != expected or any(row['errors'] or row['bytes'] != 128 << 20 for row in records)
                or not re.search(rb'writebroker-bench: done errors=0\b', log)):
            raise RuntimeError('Probe failed verification or has incomplete results: ' + label)
    receipt['final_procs'] = idle(args.host)
    save(args.output / (label + '.json'), receipt)
    return receipt


class SyntheticFile:
    def __init__(self, length):
        self.remaining = length
        self.block = bytes(range(256)) * 4096

    def read(self, count):
        count = min(count, self.remaining, len(self.block))
        self.remaining -= count
        return self.block[:count]


def ftp_network(args, root):
    for writers in (1, 4):
        idle(args.host)
        space_guard(args, 'ftp-' + str(writers))
        barrier = threading.Barrier(writers + 1)
        rows = [{} for _ in range(writers)]

        def upload(index):
            path = root + '/ftp-' + str(writers) + '-' + str(index) + '.bin'
            row = rows[index]
            attempted = False
            try:
                with ftp(args.host) as client:
                    try:
                        stream = SyntheticFile(GIB // writers)
                        barrier.wait(timeout=30)
                        attempted = True
                        start = time.monotonic()
                        response = client.storbinary('STOR ' + path, stream, blocksize=1 << 20)
                        finished = time.monotonic()
                        elapsed = finished - start
                        size = client.size(path)
                        row.update({'bytes': size, 'seconds': elapsed, 'MiBs': size / elapsed / (1 << 20),
                                    'started_monotonic': start, 'finished_monotonic': finished,
                                    'reply': response, 'host_data_socket_buffers': client.last_buffers})
                        if size != GIB // writers:
                            raise RuntimeError('FTP scratch size mismatch: ' + path)
                    finally:
                        if attempted:
                            client.delete(path)
                            row['deleted'] = True
            except Exception as error:
                barrier.abort()
                row['error'] = str(error)
                raise

        start = None
        failure = None
        with concurrent.futures.ThreadPoolExecutor(max_workers=writers) as pool:
            futures = [pool.submit(upload, index) for index in range(writers)]
            try:
                start = time.monotonic()
                barrier.wait(timeout=30)
                for future in futures:
                    future.result()
            except Exception as error:
                failure = error
                barrier.abort()
        # End-to-end wall includes SIZE and deleting each owned file. The data
        # transfer interval below excludes setup, SIZE and deletion.
        wall = time.monotonic() - start
        complete = [row for row in rows if 'finished_monotonic' in row]
        transfer_seconds = (max(row['finished_monotonic'] for row in complete) -
                            min(row['started_monotonic'] for row in complete)) if complete else 0
        size = sum(row.get('bytes', 0) for row in rows)
        save(args.output / ('ftp-network-' + str(writers) + '.json'), {
            'writers': writers, 'bytes': size, 'transfer_seconds': transfer_seconds,
            'MiBs': size / transfer_seconds / (1 << 20) if transfer_seconds else 0,
            'end_to_end_seconds': wall, 'files': rows,
            'interpretation': 'LAN host-to-console throughput; not an isolated disk ceiling',
        })
        if failure:
            raise failure


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--execute', action='store_true', help='Explicit permission to contact the console')
    parser.add_argument('--host', help='Console host (required with --execute)')
    parser.add_argument('--mode', choices=('native', 'broker-ab', 'ftp-network', 'all'), default='native')
    parser.add_argument('--output', type=Path, required=True, help='New local evidence directory')
    parser.add_argument('--sdk', type=Path, default=Path.home() / 'ps5-payload-sdk-platform')
    parser.add_argument('--wine', type=Path, help='Wine source tree (default: sibling ps5-wine in repository)')
    parser.add_argument('--baseline-broker', type=Path)
    parser.add_argument('--candidate-broker', type=Path)
    parser.add_argument('--restore-broker', type=Path)
    parser.add_argument('--timeout', type=int, default=1800)
    args = parser.parse_args()
    if args.execute and not args.host:
        parser.error('--host is required with --execute')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    root = SCRATCH + '/run-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ-') + uuid.uuid4().hex[:8]
    source = Path(__file__).with_suffix('.c')
    payloads, brokers = {}, {}
    manifest = {'scratch_root': root, 'host': args.host, 'mode': args.mode,
                'runner_sha256': digest(Path(__file__).read_bytes()), 'execute': args.execute,
                'payloads': {}, 'brokers': {}}
    if args.mode in ('native', 'broker-ab', 'all'):
        if args.wine is None:
            args.wine = Path(__file__).resolve().parents[3] / 'ps5-wine'
        manifest['source_sha256'] = digest(source.read_bytes())
        for kind in ('native', 'broker'):
            target = args.output / (kind + '.elf')
            command = [str(args.sdk / 'bin/prospero-clang'), '-Wall', '-Wextra', '-Werror', '-O2', '-pthread',
                       '-I' + str(args.wine / 'include'), '-DWB_BENCH_ROOT="' + root + '"',
                       '-DWB_BENCH_BROKER=' + str(int(kind == 'broker')), str(source), '-o', str(target)]
            subprocess.run(command, check=True)
            payloads[kind] = target.read_bytes()
            manifest['payloads'][kind] = {'sha256': digest(payloads[kind]), 'command': command}
    if args.mode in ('broker-ab', 'all'):
        for kind in ('baseline', 'candidate', 'restore'):
            path = getattr(args, kind + '_broker')
            if path is None:
                parser.error('--' + kind + '-broker is required for broker A/B and exact restoration')
            brokers[kind] = path.read_bytes()
            manifest['brokers'][kind] = {'path': str(path.resolve()), 'sha256': digest(brokers[kind])}
    save(args.output / 'manifest.json', manifest)
    if not args.execute:
        print('Prepared only; no console contact. Evidence/build directory:', args.output)
        return
    save(args.output / 'guard.json', {'procs': idle(args.host)})
    space_guard(args, 'initial')
    with ftp(args.host) as client:
        try:
            client.cwd(SCRATCH)
        except ftplib.error_perm:
            client.mkd(SCRATCH)
        client.mkd(root)
    success = False
    try:
        if args.mode in ('ftp-network', 'all'):
            ftp_network(args, root)
        if args.mode in ('native', 'all'):
            run_payload(args, 'native-authid-abba', payloads['native'])
        if args.mode in ('broker-ab', 'all'):
            try:
                for label, kind in (('A1', 'baseline'), ('B1', 'candidate'), ('B2', 'candidate'), ('A2', 'baseline')):
                    run_payload(args, label + '-start-broker', brokers[kind], broker=True)
                    run_payload(args, label + '-broker-matrix', payloads['broker'])
            finally:
                run_payload(args, 'restore-broker', brokers['restore'], broker=True)
        success = True
    finally:
        removed = False
        try:
            if success:
                idle(args.host)
                with ftp(args.host) as client:
                    client.rmd(root)  # Never recursively delete unknown or failed-run files.
                removed = True
        finally:
            save(args.output / 'cleanup.json', {'scratch_removed': removed, 'scratch_root': root,
                                               'failed_run_root_retained': not removed})
    print('Completed console evidence:', args.output)


if __name__ == '__main__':
    main()
