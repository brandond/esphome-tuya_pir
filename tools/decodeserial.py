#!/usr/bin/env python
from __future__ import print_function

import re
import sys

buffers = {'RX': [], 'TX': []}
net_states = [
    'Config_Smart',
    'Config_AP',
    'Up_WiFi',
    'Up_WiFi_IP',
    'Up_WiFi_IP_MQTT',
    'Up_Sleep',
    ]
funcs = {
    0x00: {
        0x00: lambda *args: globals()['v0_heartbeat'](*args),
        0x01: lambda *args: globals()['v0_productinfo'](*args),
        0x02: lambda *args: globals()['v0_net_report'](*args),
        0x03: lambda *args: globals()['v0_reset_switch'](*args),
        0x05: lambda *args: globals()['v0_dp_report'](*args),
        },
    0x03: {
        0x00: lambda *args: globals()['v3_heartbeat'](*args),
        0x01: lambda *args: globals()['v3_productinfo'](*args),
        0x02: lambda *args: globals()['v3_mode_config'](*args),
        0x03: lambda *args: globals()['v3_net_report'](*args),
        0x04: lambda *args: globals()['v3_reset_switch'](*args),
        0x05: lambda *args: globals()['v3_reset_select'](*args),
        0x06: lambda *args: globals()['v3_dp_set'](*args),
        0x07: lambda *args: globals()['v3_dp_report'](*args),
        0x08: lambda *args: globals()['v3_dp_query'](*args),
        0x0A: lambda *args: globals()['v3_ota_start'](*args),
        0x0B: lambda *args: globals()['v3_ota_package'](*args),
        0x0E: lambda *args: globals()['v3_wifi_test'](*args),
        0x1C: lambda *args: globals()['v3_localtime'](*args),
        },
    }


def v0_heartbeat(p):
    print('\tv0 Heartbeat {}'.format(p))


def v0_productinfo(p):
    print('\tv0 ProductInfo')
    if len(p):
        print('\t\tresponse={}'.format(''.join(chr(int(b, 16)) for b in p)))
    else:
        print('\t\tquery')


def v0_net_report(p):
    print('\tv0 Net Report')
    if p:
        s = int(p[0], 16)
        print('\t\tstate={}'.format(net_states[s]))
    else:
        print('\t\tack')


def v0_reset_switch(p):
    print('\tv0 Reset (Switch)')


def v0_dp_report(p):
    print('\tv0 DP Report')
    handle_dp(p)


def v3_heartbeat(p):
    print('\tv3 Heartbeat {}'.format(p))


def v3_productinfo(p):
    print('\tv3 ProductInfo')
    if len(p):
        print('\t\tresponse={}'.format(''.join(chr(int(b, 16)) for b in p)))
    else:
        print('\t\tquery')


def v3_mode_config(p):
    print('\tv3 Mode Config {}'.format(p))


def v3_net_report(p):
    print('\tv3 Net Report')
    if p:
        s = int(p[0], 16)
        print('\t\tstate={}'.format(net_states[s]))
    else:
        print('\t\tack')


def v3_reset_switch(p):
    print('\tv3 Reset (Switch)')


def v3_reset_select(p):
    print('\tv3 Reset (Select)')
    if p:
        s = int(p[0], 16)
        print('\t\tmode={}'.format(net_states[s]))
    else:
        print('\t\tack')


def v3_dp_set(p):
    print('\tv3 DP Set')
    handle_dp(p)


def v3_dp_report(p):
    print('\tv3 DP Report')
    handle_dp(p)


def v3_dp_query(p):
    print('\tv3 DP Query')


def v3_ota_start(p):
    print('\tv3 OTA Start')


def v3_ota_package(p):
    print('\tv3 OTA Package')


def v3_wifi_test(p):
    print('\tv3 Wifi Test')
    if p:
        print('\t\tresponse: {}'.format(p))
    else:
        print('\t\trequest')


def v3_localtime(p):
    print('\tv3 Localtime')


def handle_dp(b):
    if len (b) == 1:
        print('\t\tDataPoint ack')
    elif len(b) >= 4:
        i = int(b[0], 16)
        t = int(b[1], 16)
        l = (int(b[2], 16) << 8) + (int(b[3], 16))
        d = b[4:4+l]

        print('\t\tDataPoint id={}'.format(i))
        if t == 0x00:
            print('\t\tdata=bytearray [{}]'.format(', '.join(d)))
        elif t == 0x01:
            print('\t\tdata=bool {}'.format(bool(int(d[0], 16))))
        elif t == 0x02:
            print('\t\tdata=int {}'.format(int(''.join(d), 16)))
        elif t == 0x03:
            print('\t\tdata=string {}'.format(''.join(chr(int(c, 16)) for c in d)))
        elif t == 0x04:
            print('\t\tdata=enum {}'.format(int(d[0], 16)))
        elif t == 0x05:
            print('\t\tdata=bitmask {}'.format(int(d[0], 16)))
        else:
            print('\t\tError: Unknown DP type')
    else:
        print('\t\tError: Bad DP length')


def handle_buffer(d, b, v=0):
    print('{}: {}'.format(d, ' '.join(b)))
    # Extract version, command, length, data, and sum
    v = int(b[2], 16) or v
    c = int(b[3], 16)
    l = (int(b[4], 16) << 8) + (int(b[5], 16))
    d = b[6:6+l]
    s = int(b[6+l], 16)
    # Calculate real sum
    r = 0
    for i in range(0, 6+l):
        r += int(b[i], 16)
    r %= 256
    print('\tTuyaMessage version={} command={} length={} data="{}" checksum={}[{}]'.format(v, c, l, ' '.join(d), s, r))
    try:
        funcs[v][c](d)
    except KeyError as e:
        print('\tError: No func for this version+code: {}'.format(e))
    return v


def main(n):
    with open(n, 'r') as f:
        v = 0  # start in version 0; upgrade to highest version seen
        for l in f:
            m = re.match(r'(?P<s>\d+)-(?P<e>\d+) UART: (?P<d>..): (?P<b>..)', l)

            if m:
                g = m.groupdict()
                buffers[g['d']].append(g['b'])

            for d, b in buffers.items():
                while len(b) >= 2:
                    if b[0] == '55' and b[1] == 'AA':
                        if len(b) >= 6:
                            l = (int(b[4], 16) << 8) + (int(b[5], 16)) + (7)
                            if len(b) >= l:
                                p = b[:l]
                                v = handle_buffer(d, p, v)
                                b[:l] = []
                            else:
                                break
                        else:
                            break
                    else:
                        b[:1] = []


if __name__ == '__main__':
    if len(sys.argv) > 1:
        main(sys.argv[1])
    else:
        print('Usage: {} <filename>'.format(sys.argv[0]))
