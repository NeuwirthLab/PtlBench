#!/usr/bin/env python3
"""Collect sequential PtlBench sweeps with statistics, logs, and run metadata.

Uses Python 3.8+ standard library and a configured Open MPI environment.
Message sizes are powers of two from 1 byte through 4 MiB, with a 4 KiB cap
for the memory benchmark. Unsupported sizes and NI features are recorded.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import csv
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

BENCHMARKS = ('ptl_bench', 'ptl_me_none_persistent', 'ptl_ping_pong',
              'ptl_memory_bench')
SIZES = tuple(1 << exponent for exponent in range(23))
MEMORY_SIZES = tuple(size for size in SIZES if size <= 4 * 1024)
STATS_HEADER = ('msg_size_bytes', 'metric', 'unit', 'sample_count', 'mean', 'median', 'min', 'max')
PROGRESS_INTERVAL = 10
REPO = Path(__file__).resolve().parents[1]


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def integer(value):
    if not value.isascii() or not value.isdecimal() or int(value) >= 2**31 - 1:
        raise argparse.ArgumentTypeError('expected a decimal integer below INT_MAX')
    return int(value)


def positive(value):
    number = integer(value)
    if number == 0:
        raise argparse.ArgumentTypeError('must be positive')
    return number


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, default=REPO / 'build',
                        help='directory containing the network benchmarks (default: repo/build)')
    parser.add_argument('--output', type=Path, default=Path('results'),
                        help='parent directory for a new timestamped collection (default: results)')
    parser.add_argument('--label', default='', help='optional letters/digits/dashes/underscores')
    parser.add_argument('--benchmarks', nargs='+', choices=BENCHMARKS, default=list(BENCHMARKS))
    parser.add_argument('--iterations', type=positive, default=10,
                        help='measured samples per size (default: 10)')
    parser.add_argument('--warmup', type=integer, default=10,
                        help='warm-up samples; memory benchmark always uses zero (default: 10)')
    parser.add_argument('--windows', nargs='+', type=positive, default=[64],
                        help='bandwidth and per-message window sizes (default: 64)')
    parser.add_argument('--cache-pids', nargs='+', type=positive, default=[1, 128],
                        help='BXI cache_pids values for plain PUT/GET (default: 1 128)')
    parser.add_argument('--cache-mib', type=positive, default=16,
                        help='cache sweep size for network benchmarks (default: 16 MiB)')
    parser.add_argument('--seed', type=integer, default=1,
                        help='memory/page-fault offset seed (default: 1)')
    parser.add_argument('--repeats', type=positive, default=10,
                        help='independent launches per experiment (default: 10)')
    parser.add_argument('--mpirun', default='mpirun', help='Open MPI launcher executable')
    parser.add_argument('--hosts', help='Open MPI host list; otherwise use the current allocation')
    parser.add_argument('--map-by', default='ppr:1:node',
                        help='Open MPI placement (default: one rank per node)')
    parser.add_argument('--mpi-args', default='',
                        help='extra launcher arguments, e.g. --mpi-args="-x LD_LIBRARY_PATH"')
    parser.add_argument('--timeout', type=float, default=300,
                        help='seconds per launch; timeout stops the collection (default: 300)')
    parser.add_argument('--fail-fast', action='store_true', help='also stop on ordinary failures')
    parser.add_argument('--dry-run', action='store_true',
                        help='save commands and metadata without executing any benchmarks')
    args = parser.parse_args(argv)
    if args.iterations + args.warmup >= 2**31 - 1:
        parser.error('iterations + warmup must be below INT_MAX')
    if max(args.windows) > 4096:
        parser.error('windows must not exceed the 4096-entry full-event queue')
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('timeout must be finite and positive')
    if args.label and not re.fullmatch(r'[A-Za-z0-9_-]+', args.label):
        parser.error('label may contain only letters, digits, dashes, and underscores')
    try:
        args.mpi_args = shlex.split(args.mpi_args)
    except ValueError as error:
        parser.error(str(error))
    if any(re.match(r'^(?:-n|-np|-c)(?:[0-9=]|$)|^--(?:np|n|nprocs|num-procs)(?:=|$)', arg) or arg == ':'
           for arg in args.mpi_args):
        parser.error('the runner supplies exactly two ranks; omit rank counts/MPMD commands')
    args.windows = sorted(set(args.windows))
    args.cache_pids = list(dict.fromkeys(args.cache_pids))
    args.benchmarks = list(dict.fromkeys(args.benchmarks))
    args.build = args.build.expanduser().resolve()
    args.output = args.output.expanduser().resolve()
    return args


def launcher(args):
    command = [shutil.which(args.mpirun) or args.mpirun, *args.mpi_args, '-np', '2',
               '--map-by', args.map_by]
    if args.hosts:
        command += ['--host', args.hosts]
    return command


def make_plan(args):
    jobs = []

    def add(benchmark, variant, options, config, units, sizes=SIZES, cache_pids=1):
        base = {'iterations': args.iterations, 'warmup': args.warmup,
                'message_sizes_bytes': list(sizes), 'cache_pids': cache_pids, **config}
        variant += f'/cache-pids-{cache_pids}'
        command = launcher(args) + ['-x', f'PTLBENCH_CACHE_PIDS={cache_pids}']
        jobs.append({'benchmark': benchmark, 'variant': variant,
                     'command': command + [str(args.build / benchmark), '--stats', *map(str, options)],
                     'config': base, 'csv_header': list(STATS_HEADER), 'units': units})

    for benchmark in args.benchmarks:
        if benchmark == 'ptl_bench':
            for cache_pids, op, ni, events, cache, measure in itertools.product(
                    args.cache_pids, ('put', 'get'), ('nonmatching', 'matching'), ('counting', 'full'),
                    ('hot', 'cold'), ('latency', 'bandwidth')):
                for window in ([1] if measure == 'latency' else args.windows):
                    options = ['-i', args.iterations, '-x', args.warmup, '-w', window,
                               '--min_msg_size', 1, '--max_msg_size', SIZES[-1],
                               '-c', args.cache_mib]
                    for enabled, flag in ((op == 'get', '-g'), (ni == 'matching', '-m'),
                                          (events == 'full', '-f'), (cache == 'cold', '--cold_cache'),
                                          (measure == 'bandwidth', '-b')):
                        if enabled:
                            options.append(flag)
                    add(benchmark, f'{op}-{ni}-{events}-{measure}-cache-{cache}-window-{window}',
                        options, dict(operation=op, ni_mode=ni, events=events, cache=cache,
                                      cache_mib=args.cache_mib, measurement=measure, window=window),
                        {measure: 'us (operation completion)' if measure == 'latency' else 'MB/s (decimal)'},
                        cache_pids=cache_pids)
        elif benchmark == 'ptl_me_none_persistent':
            for op, window in itertools.product(('put', 'get'), args.windows):
                # This executable's native sweep is 1..4194304; only a single-size override exists.
                options = ['-i', args.iterations, '-u', args.warmup, '-w', window]
                if op == 'get':
                    options.append('--get')
                add(benchmark, f'{op}-matching-full-window-{window}', options,
                    dict(operation=op, ni_mode='matching', events='full', window=window),
                    {'bandwidth': 'MB/s (decimal)', 'latency': 'us/operation (window time / window)'})
        elif benchmark == 'ptl_ping_pong':
            for triggered, size in itertools.product((False, True), SIZES):
                mode = 'triggered' if triggered else 'standard'
                add(benchmark, f'{mode}/bytes-{size:08d}',
                    ['-i', args.iterations, '-w', args.warmup, '-m', size]
                    + (['--triggered'] if triggered else []),
                    dict(operation='put', ni_mode='nonmatching', events='counting',
                         window=1, triggered=triggered),
                    {'rtt': 'us (round trip)', **({'setup_time': 'us (trigger registration)'} if triggered else {})},
                    [size])
        elif benchmark == 'ptl_memory_bench':
            # Size first: observe actual rank-side limits before trying larger sizes.
            for size, pattern, local, remote in itertools.product(
                    MEMORY_SIZES, ('put', 'get', 'ping_pong'), ('cold', 'hot'), ('cold', 'hot')):
                options = ['-i', args.iterations, '-m', size, '-c', args.cache_mib, '--seed', args.seed]
                if pattern != 'put':
                    options.append('--get' if pattern == 'get' else '--ping_pong')
                if local == 'hot':
                    options.append('--local-hot')
                if remote == 'hot':
                    options.append('--remote-hot')
                add(benchmark, f'{pattern}-local-{local}-remote-{remote}/bytes-{size:08d}',
                    options, dict(warmup=0, operation='get' if pattern == 'get' else 'put',
                                  pattern='ping_pong' if pattern == 'ping_pong' else 'one_sided',
                                  local_pages=local, remote_pages=remote, cache_mib=args.cache_mib,
                                  seed=args.seed, ni_mode='nonmatching', events='full', window=1),
                    {'latency': 'us (round trip)' if pattern == 'ping_pong' else 'us (operation completion)'},
                    [size])
    return [dict(job, repeat=repeat, id=f"{job['benchmark']}/{job['variant']}/repeat-{repeat:03d}",
                 status='planned', returncode=None, rows=None, elapsed_seconds=None, reason='')
            for repeat in range(1, args.repeats + 1) for job in jobs]


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    temporary.replace(path)


def metadata(args):
    def git(*options):
        try:
            result = subprocess.run(['git', '-C', str(REPO), *options], capture_output=True,
                                    text=True, timeout=5)
            return result.stdout.strip() if result.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired):
            return None

    binaries = {}
    for name in args.benchmarks:
        path = args.build / name
        if path.is_file():
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            binaries[name] = {'path': str(path), 'sha256': digest.hexdigest(),
                              'mtime_ns': path.stat().st_mtime_ns}
    cache = {}
    cache_path = args.build / 'CMakeCache.txt'
    if cache_path.is_file():
        for line in cache_path.read_text(errors='replace').splitlines():
            if re.match(r'^(CMAKE_BUILD_TYPE|CMAKE_C_COMPILER|CMAKE_C_FLAGS\w*|MPI_C_COMPILER|'
                        r'MPI_mpi_LIBRARY|PORTALS_INCLUDE_DIR|PORTALS_LIBRARY):', line):
                key, value = line.split('=', 1)
                cache[key.split(':')[0]] = value
    return {'host': platform.node(), 'platform': platform.platform(), 'python': sys.version,
            'cwd': str(Path.cwd()), 'local_page_bytes': os.sysconf('SC_PAGESIZE'),
            'git_revision': git('rev-parse', 'HEAD'), 'git_status': git('status', '--short'),
            'cmake_cache': cache, 'binaries': binaries,
            'environment': {key: os.environ[key] for key in
                            ('PATH', 'LD_LIBRARY_PATH', 'LOADEDMODULES', 'MPI_HOME',
                             'OMP_NUM_THREADS', 'OMP_PROC_BIND', 'OMP_PLACES') if key in os.environ}}


def stop_process(process):
    """Give mpirun time to terminate remote ranks before killing the local group."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    # Also reap local descendants if the launcher exited first.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def wait_for_launch(process, job, stdout, timeout):
    start = time.monotonic()
    rows_expected = len(job.get('config', {}).get('message_sizes_bytes', ())) * len(job.get('units', {}))
    offset = lines = 0
    while True:
        remaining = timeout - (time.monotonic() - start)
        try:
            return process.wait(timeout=max(0, min(PROGRESS_INTERVAL, remaining)))
        except subprocess.TimeoutExpired:
            elapsed = time.monotonic() - start
            if elapsed >= timeout:
                raise
            # pread leaves the child's output offset untouched. Count only
            # complete lines, excluding the CSV header.
            chunk = os.pread(stdout.fileno(), 65536, offset)
            offset += len(chunk)
            lines += chunk.count(b'\n')
            progress = f', {max(0, lines - 1)}/{rows_expected} statistics rows received' if rows_expected else ''
            print(f'  still running after {elapsed:.0f} s{progress}', flush=True)


def execute(job, directory, timeout, stdout):
    job['started_at'] = utc_now()
    start = time.monotonic()
    process = None
    job.update(status='running', returncode=None)
    write_json(directory / 'run.json', job)
    try:
        with (directory / 'stderr.log').open('wb') as err:
            process = subprocess.Popen(job['command'], stdout=stdout, stderr=err,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            job['returncode'] = wait_for_launch(process, job, stdout, timeout)
        job['status'] = 'ok' if job['returncode'] == 0 else 'failed'
        if job['returncode']:
            job['reason'] = f"process exited with status {job['returncode']}"
    except subprocess.TimeoutExpired:
        stop_process(process)
        job.update(status='timed_out', reason=f'launch exceeded {timeout:g} seconds',
                   returncode=process.returncode)
    except KeyboardInterrupt:
        if process:
            stop_process(process)
        job.update(status='interrupted', reason='collection interrupted',
                   returncode=process.returncode if process else None)
    except OSError as error:
        job.update(status='failed', reason=str(error))
    finally:
        job['finished_at'] = utc_now()
        job['elapsed_seconds'] = time.monotonic() - start


def collect_statistics(job, stream, path):
    """Validate and save the statistics emitted by the executable."""
    config = job['config']
    sizes = config['message_sizes_bytes']
    pending = {(size, metric): unit for size in sizes for metric, unit in job['units'].items()}
    stream.seek(0)
    reader = csv.DictReader(stream)
    if reader.fieldnames != list(STATS_HEADER):
        raise ValueError(f'CSV header differs from {list(STATS_HEADER)}')
    count = 0
    sample_sizes = set()
    for row in reader:
        if None in row or any(value is None for value in row.values()):
            raise ValueError('malformed statistics row')
        size = int(row['msg_size_bytes'])
        key = (size, row['metric'])
        if key not in pending:
            raise ValueError(f'unexpected or duplicate statistics for {key}')
        if row['unit'] != pending.pop(key):
            raise ValueError(f'wrong statistics unit for {key}')
        samples = int(row['sample_count'])
        if samples != config['iterations']:
            raise ValueError(f'wrong sample count for {key}')
        mean, median, minimum, maximum = (float(row[k]) for k in ('mean', 'median', 'min', 'max'))
        if (not all(math.isfinite(value) for value in (mean, median, minimum, maximum))
                or not (0 <= minimum <= mean <= maximum and minimum <= median <= maximum)):
            raise ValueError(f'invalid statistics for {key}')
        # Each metric describes the same iterations at a given size.
        if size not in sample_sizes:
            sample_sizes.add(size)
            count += samples
    if pending:
        raise ValueError(f'missing statistics for {list(pending)}')

    temporary = path.with_suffix('.csv.tmp')
    try:
        stream.seek(0)
        with temporary.open('w', newline='', encoding='utf-8') as out:
            shutil.copyfileobj(stream, out)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    return count


class Limits:
    """Only infer skips from explicit benchmark diagnostics, never launcher errors."""
    def __init__(self):
        self.rules = []

    def reason(self, job):
        for rule in self.rules:
            if rule['benchmark'] != job['benchmark']:
                continue
            if rule['kind'] == 'triggered' and not job['config'].get('triggered'):
                continue
            if rule['kind'] != 'triggered' and max(job['config']['message_sizes_bytes']) <= rule['maximum']:
                continue
            return f"{rule['reason']} (observed in {rule['source']})"
        return ''

    def observe(self, job, diagnostic):
        if job['status'] != 'failed':
            return
        rule = None
        if (job['benchmark'] == 'ptl_ping_pong' and job['config'].get('triggered')
                and 'Iterations plus warmup exceed NI max_triggered_ops' in diagnostic):
            values = re.findall(r'NI max_triggered_ops=(\d+)', diagnostic)
            if values:
                limit = min(map(int, values))
                rule = dict(kind='triggered', maximum=limit,
                            reason=f'NI trigger capacity {limit} is below iterations + warmup')
        if (job['benchmark'] == 'ptl_memory_bench'
                and 'Message must fit within one system page' in diagnostic):
            # A rejected size bounds the page size on at least one MPI rank.
            rule = dict(kind='page', maximum=job['config']['message_sizes_bytes'][0] - 1,
                        reason='message rejected by a rank-side page-size check')
        if rule:
            rule.update(benchmark=job['benchmark'], source=job['id'])
            self.rules.append(rule)
            job.update(status='unsupported', reason=rule['reason'])


def save_collection(root, manifest):
    write_json(root / 'manifest.json', manifest)
    temporary = root / 'summary.csv.tmp'
    fields = ('id', 'benchmark', 'status', 'returncode', 'rows', 'elapsed_seconds',
              'reason', 'statistics', 'stderr', 'cache_pids')
    with temporary.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for job in manifest['runs']:
            writer.writerow({**{key: job.get(key, '') for key in fields},
                             'cache_pids': job['config'].get('cache_pids', ''),
                             'statistics': job['id'] + '/statistics.csv' if job['status'] == 'ok' else '',
                             'stderr': job['id'] + '/stderr.log' if job.get('started_at') else ''})
    temporary.replace(root / 'summary.csv')


def main(argv=None):
    args = arguments(argv)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    root = args.output / (stamp + ('-' + args.label if args.label else ''))
    # Exclusive creation: existing data is never overwritten or reused.
    root.mkdir(parents=True, exist_ok=False)
    manifest = {'schema_version': 2, 'created_at': utc_now(), 'status': 'planned',
                'settings': {key: str(value) if isinstance(value, Path) else value
                             for key, value in vars(args).items()},
                'system': metadata(args), 'runs': make_plan(args)}
    save_collection(root, manifest)
    print(f"Collection: {root}\nPlanned launches: {len(manifest['runs'])}", flush=True)
    if args.dry_run:
        for job in manifest['runs']:
            print(f"{job['id']}: {shlex.join(job['command'])}")
        return 0
    limits = Limits()
    manifest['status'] = 'running'
    job = None
    try:
        for index, job in enumerate(manifest['runs'], 1):
            directory = root / job['id']
            directory.mkdir(parents=True)
            reason = limits.reason(job)
            executable = args.build / job['benchmark']
            if reason:
                job.update(status='skipped', reason=reason)
            elif not executable.is_file() or not os.access(executable, os.X_OK):
                job.update(status='unavailable', reason=f'missing executable: {executable}')
            else:
                print(f"[{index}/{len(manifest['runs'])}] {job['id']}", flush=True)
                job['status'] = 'running'
                save_collection(root, manifest)
                # Validate the captured statistics before publishing the file.
                with tempfile.TemporaryFile(mode='w+', encoding='utf-8', newline='') as stdout:
                    execute(job, directory, args.timeout, stdout)
                    if job['status'] == 'failed':
                        log = directory / 'stderr.log'
                        limits.observe(job, log.read_text(errors='replace') if log.exists() else '')
                    if job['status'] == 'ok':
                        try:
                            cache_pids = job['config'].get('cache_pids')
                            if cache_pids is not None:
                                diagnostic = (directory / 'stderr.log').read_text(errors='replace')
                                confirmed = re.findall(r'rank ([01]): BXI cache_pids=(\d+)\b', diagnostic)
                                if set(confirmed) != {('0', str(cache_pids)), ('1', str(cache_pids))}:
                                    raise ValueError(f'both MPI ranks must confirm BXI cache_pids={cache_pids}')
                            job['rows'] = collect_statistics(job, stdout, directory / 'statistics.csv')
                        except (ValueError, OSError, csv.Error) as error:
                            job.update(status='invalid_data', reason=str(error))
            write_json(directory / 'run.json', job)
            manifest['observed_limits'] = limits.rules
            save_collection(root, manifest)
            print(f"  {job['status']}: {job['reason'] or str(job['rows']) + ' samples'}", flush=True)
            if (job['status'] in ('timed_out', 'interrupted') or
                    args.fail_fast and job['status'] in ('failed', 'invalid_data', 'unavailable')):
                break
    except KeyboardInterrupt:
        manifest['status'] = 'interrupted'
        if job is not None and (job['status'] == 'running' or
                                job['status'] == 'ok' and job['rows'] is None):
            job.update(status='interrupted', reason='interrupted before result validation completed')
            write_json(root / job['id'] / 'run.json', job)
    for job in manifest['runs']:
        if job['status'] == 'planned':
            job.update(status='not_run', reason='collection stopped before this run')
    counts = Counter(job['status'] for job in manifest['runs'])
    interrupted = manifest['status'] == 'interrupted' or counts['interrupted'] > 0
    failed = any(counts[key] for key in ('failed', 'invalid_data', 'timed_out', 'unavailable', 'not_run'))
    manifest.update(status='interrupted' if interrupted else 'failed' if failed else 'complete',
                    finished_at=utc_now(), counts=dict(counts))
    save_collection(root, manifest)
    print(f"Finished: {dict(counts)}\nResults: {root}")
    return 130 if interrupted else 1 if failed else 0


def interrupt(signum, frame):
    raise KeyboardInterrupt


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, interrupt)
    try:
        sys.exit(main())
    except OSError as error:
        print(f'Collection error: {error}', file=sys.stderr)
        sys.exit(1)
