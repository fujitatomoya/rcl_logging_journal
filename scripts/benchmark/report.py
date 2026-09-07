#!/usr/bin/env python3
# Copyright 2026 Tomoya Fujita <tomoya.fujita825@gmail.com>.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Turn the JSON lines written by run_matrix.sh into markdown tables.

Usage: report.py <results directory>
"""

import json
import pathlib
import sys


def load(path):
    if not path.exists():
        return []
    rows = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            rows.append(json.loads(line))
    return rows


def us(ns):
    return f'{ns / 1000:.1f}'


def human(num):
    num = float(num)
    for unit in ('B', 'KiB', 'MiB', 'GiB'):
        if abs(num) < 1024 or unit == 'GiB':
            return f'{num:.1f} {unit}'
        num /= 1024
    return f'{num:.1f} GiB'


def table(headers, rows):
    align = ['|' + '|'.join(' :--- ' if i == 0 else ' ---: ' for i in range(len(headers))) + '|']
    out = ['| ' + ' | '.join(headers) + ' |'] + align
    for row in rows:
        out.append('| ' + ' | '.join(str(c) for c in row) + ' |')
    return '\n'.join(out)


def report_b1(rows):
    print('## B1 client call latency\n')
    print('Latency of one `rcl_logging_external_log()` call as seen by the caller, '
          'in microseconds.\n')
    body = []
    for r in rows:
        body.append([
            r['backend'], r['size'], r['severity'],
            us(r['p50_ns']), us(r['p95_ns']), us(r['p99_ns']), us(r.get('p999_ns', 0)),
            us(r['max_ns']), f"{r['calls_per_sec']:,.0f}",
        ])
    print(table(
        ['backend', 'size', 'severity', 'p50 us', 'p95 us', 'p99 us', 'p99.9 us', 'max us',
         'calls/s'],
        body))
    print()


def report_b2(rows):
    print('## B2 system CPU per record\n')
    print('CPU time of the application plus the log daemons (journald, and rsyslogd for the '
          'syslog backend), in CPU milliseconds per 1000 records.\n')
    body = []
    for r in rows:
        app = r['cpu_user_sec'] + r['cpu_sys_sec']
        daemon = r.get('daemon_cpu_sec', 0.0)
        per_k = 1000.0 / r['count']
        body.append([
            r['backend'], r['count'],
            f'{app * 1000 * per_k:.2f}', f'{daemon * 1000 * per_k:.2f}',
            f'{(app + daemon) * 1000 * per_k:.2f}', r.get('daemon_pids', ''),
        ])
    print(table(
        ['backend', 'records', 'app CPU ms/1k', 'daemon CPU ms/1k', 'total CPU ms/1k',
         'daemon pids'],
        body))
    print()


def report_b3(rows):
    print('## B3 sustained throughput and loss\n')
    print('5 s bursts at a fixed rate. "delivered" counts the records found in the sink '
          'afterwards, "suppressed" is what journald reported dropping because of its '
          'rate limit.\n')
    body = []
    for r in rows:
        delivered = r.get('delivered', 'n/a')
        lost = (r['count'] - delivered) if isinstance(delivered, int) else 'n/a'
        body.append([
            r['backend'], f"{r['rate']:,.0f}", f"{r['calls_per_sec']:,.0f}", r['count'],
            delivered, lost, r.get('suppressed', 'n/a'), us(r['p99_ns']),
        ])
    print(table(
        ['backend', 'target msg/s', 'achieved msg/s', 'sent', 'delivered', 'lost',
         'journald suppressed', 'p99 us'],
        body))
    print()


def report_b4(rows):
    print('## B4 storage footprint\n')
    print('Bytes added on disk by an identical workload '
          '(70% 64 B INFO, 25% 256 B INFO, 5% 4 KiB WARN).\n')
    body = []
    for r in rows:
        per_record = f"{r['bytes_delta'] / r['records']:.1f}" if r['records'] else 'n/a'
        body.append([r['backend'], r['records'], human(r['bytes_delta']), per_record])
    print(table(['backend', 'records', 'bytes on disk', 'bytes/record'], body))
    print()


def report_b5(rows):
    print('## B5 query performance\n')
    print('Wall time to list all WARN and above records of one logger: '
          '`journalctl` field match versus `grep` over text logs.\n')
    body = []
    for r in rows:
        body.append([
            r['backend'], r.get('query', r.get('skipped', '')),
            r.get('first_ms', 'n/a'), r.get('second_ms', 'n/a'),
        ])
    print(table(['backend', 'query', 'first run ms', 'second run ms'], body))
    print()


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    results = pathlib.Path(argv[1])
    print(f'# Logging backend benchmark ({results.name})\n')

    env = results / 'environment.txt'
    if env.exists():
        print('## Environment\n')
        print('```')
        print(env.read_text().rstrip())
        print('```\n')

    for name, renderer in (
        ('b1', report_b1), ('b2', report_b2), ('b3', report_b3),
        ('b4', report_b4), ('b5', report_b5),
    ):
        rows = load(results / f'{name}.jsonl')
        if rows:
            renderer(rows)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
