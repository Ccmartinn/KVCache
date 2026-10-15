#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
# Author: COCO Team
import re
import subprocess
import logging
import getopt
import sys
import os

SUCCESS = 0
FAILED = 1

DEVICE_DIR = '/sys/devices/'
L3C_DEV_NAME = 'l3c'
DDR_DEV_NAME = 'ddrc'
HHA_DEV_NAME = 'hha'
UC_DEV_NAME = 'l3c'

PA_DEV_NAME = '_pa0'

SCCL_DEVS = [L3C_DEV_NAME, DDR_DEV_NAME, HHA_DEV_NAME, UC_DEV_NAME]
SICL_DEVS = [PA_DEV_NAME]

SUPPORT_CPUS = ["920B", "920C", "920E", "920F"]
L3C_CPUS = ["920B", "920C", "920E"]
UC_CPUS = ["920F"]
PA_CPUS = ["920B"]

# DDR带宽统计：支持920B/920C/920F/920E DDR_EVENTS = ['0x84', '0x83']  # [DDR读，DDR写]
DDR_EVENTS = ['0x84', '0x83']

# HHA请求带宽统计：支持920B/920C/920F/920E
HHA_EVENTS = ['0x00', '0x01', '0x02', '0x1c', '0x1d', '0x1e', '0x1f']

# L3T带宽统计：支持920B/920C/920E
L3C_EVENTS = [
    '0x00', '0x01', '0x02', '0x03', '0x0f',
    '0x20', '0x21', '0x22', '0x23', '0x48', '0xB8'
]

# UC带宽统计：支持920F 【修复：只保留最终生效事件】
UC_EVENTS = ['0x19', '0x1a', '0x74', '0x7d']

# PA带宽统计：支持920B
PA_EVENTS = [
    '0x5C', '0x5D', '0x5E', '0x5F', '0x60', '0x61', '0x62', '0x63',
    '0x64', '0x65', '0x66', '0x67', '0x68', '0x69', '0x6A', '0x6B',
    '0x6C', '0x6D', '0x6E', '0x6F', '0x0B', '0x0C',
    '0x40', '0x41', '0x42', '0x43', '0x44', '0x45', '0x46', '0x47',
    '0x48', '0x49', '0x4A', '0x4B', '0x4C', '0x4D', '0x4E', '0x4F',
    '0x58', '0x59', '0x5A', '0x5B', '0x0D', '0x0E', '0x78'
]

REPORT_INTERVAL_MS = 1000  # 采样间隔，单位毫秒


def sccl_scil_to_numa(name):
    # [5:3] chipId
    # [2:0] dieId
    # TotemDie [0]=1
    # IODie    [1]=0
    totemDie_numas = {3: 'numa0', 1: 'numa1', 11: 'numa2', 9: 'numa3', 19: 'numa4', 17: 'numa5', 27: 'numa6', 25: 'numa7'}
    ioDie_numas = {2: 'numa0', 0: 'numa1', 10: 'numa2', 8: 'numa6', 18: 'numa4', 16: 'numa5', 26: 'numa6', 24: 'numa7'}
    idx = int(name.replace('sccl', '').replace('sicl', ''))
    if idx in totemDie_numas.keys():
        return totemDie_numas[idx] + '-' + name
    else:
        return ioDie_numas[idx] + '-' + name


class CmdExecError(Exception):
    """Cmd Execute Error"""
    pass


STANDARDERROR = (ArithmeticError, AssertionError, AttributeError, BufferError,
                 EOFError, SyntaxError, LookupError, MemoryError, NameError, RuntimeError,
                 ReferenceError, StopAsyncIteration, StopIteration, SystemError,
                 TypeError, ValueError, PermissionError, OSError)


def cmd_exec(cmd, stdout=subprocess.PIPE, shell=False):
    """shell命令封装"""
    if isinstance(cmd, str) and shell is False:
        cmd = cmd.split()
    child = subprocess.Popen(cmd, stdout=stdout, stderr=subprocess.STDOUT, shell=shell)
    out, err = child.communicate()
    if child.returncode != SUCCESS:
        msg = "%s:%s" % (str(err).strip().replace('\n', ''), child.returncode)
        raise CmdExecError(msg)
    return str(out, encoding='utf-8') if isinstance(out, bytes) else out


def multi_cmd_exec(*args):
    """带管道符的shell命令封装:param args:Execute multiple shell commands with '|':return: Execution results."""
    process_list = []
    try:
        for index, value in enumerate(args):
            if index == 0:
                _p = subprocess.Popen(value, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            else:
                _p = subprocess.Popen(value, stdin=process_list[index - 1].stdout,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            process_list.append(_p)
        output, err = process_list[-1].communicate()
        if process_list[-1].returncode != SUCCESS:
            msg = "%s:%s" % (str(err).strip().replace('\n', ''), process_list[-1].returncode)
            raise CmdExecError(msg)
        return str(output, encoding='utf-8') if isinstance(output, bytes) else output
    except STANDARDERROR as err:
        logging.error(str(err).strip())
        raise CmdExecError("%s" % str(err).strip())


class Topology:
    def __init__(self):
        self._sccls = self._get_sccls()
        self._sicls = self._get_sicls()
        self._l3c_topo = self._get_topo_by_device_name(L3C_DEV_NAME)
        self._ddr_topo = self._get_topo_by_device_name(DDR_DEV_NAME)
        self._hha_topo = self._get_topo_by_device_name(HHA_DEV_NAME)
        self._uc_topo = self._get_topo_by_device_name(UC_DEV_NAME)
        self._pa_topo = self._get_topo_by_device_name(PA_DEV_NAME)
        print("sccl list:", self._sccls)
        print("sicl list:", self._sicls)

        self._ddr = DDRRecord(self._ddr_topo)
        self._hha = HHARecord(self._hha_topo)
        self._l3c = L3CRecord(self._l3c_topo)
        self._uc = UCRecord(self._uc_topo)
        self._pa = PARecord(self._pa_topo)
        self._time = 0.0
        self._interval = REPORT_INTERVAL_MS / 1000.0

    def _get_sccls(self):
        cmd = 'ls {}'.format(DEVICE_DIR).split()
        grep = 'grep sccl'.split()
        awk = ['awk', '-F', '_', '{print $2}']
        sort = ['sort']
        uniq = ['uniq']
        out = multi_cmd_exec(cmd, grep, awk, sort, uniq)
        return out.split()

    def _get_sicls(self):
        cmd = 'ls {}'.format(DEVICE_DIR).split()
        grep = 'grep sicl'.split()
        awk = ['awk', '-F', '_', '{print $2}']
        sort = ['sort']
        uniq = ['uniq']
        out = multi_cmd_exec(cmd, grep, awk, sort, uniq)
        out = out.replace('pmu', '')
        return out.split()

    def _get_topo_by_device_name(self, name):
        if name in SCCL_DEVS:
            sccls_sicls = self._sccls
        elif name in SICL_DEVS:
            sccls_sicls = self._sicls
        else:
            sccls_sicls = []
        topo = {}
        for sccls_sicl in sccls_sicls:
            cmd = 'ls {}'.format(DEVICE_DIR).split()
            grep1 = 'grep {}'.format(sccls_sicl).split()
            grep2 = 'grep {}'.format(name).split()
            sort = ['sort']
            uniq = ['uniq']
            out = multi_cmd_exec(cmd, grep1, grep2, sort, uniq)
            for device in out.split():
                topo[device] = sccls_sicl
        return topo

    def gen_ddrc_pmu_events(self):
        base_events = DDR_EVENTS
        events = []
        for device in self._ddr_topo:
            for event in base_events:
                events.append('{}/config={}/'.format(device, event))
        return events

    def gen_hha_pmu_events(self):
        base_events = HHA_EVENTS
        events = []
        for device in self._hha_topo:
            for event in base_events:
                events.append('{}/config={}/'.format(device, event))
        return events

    def gen_l3c_pmu_events(self):
        base_events = L3C_EVENTS
        events = []
        for device in self._l3c_topo:
            for event in base_events:
                events.append('{}/config={}/'.format(device, event))
        return events

    def gen_uc_pmu_events(self):
        base_events = UC_EVENTS
        events = []
        for device in self._uc_topo:
            for event in base_events:
                events.append('{}/config={}/'.format(device, event))
        return events

    def gen_pa_pmu_events(self):
        base_events = PA_EVENTS
        events = []
        for device in self._pa_topo:
            for event in base_events:
                events.append('{}/config={}/'.format(device, event))
        return events

    def gen_perf_stat_cmd(self, cpu_type=''):
        ddr_cmd = ','.join(self.gen_ddrc_pmu_events())
        hha_cmd = ','.join(self.gen_hha_pmu_events())
        l3_cmd = ''
        pa_cmd = ''

        if cpu_type in L3C_CPUS:
            l3_cmd = ','.join(self.gen_l3c_pmu_events())
        if cpu_type in UC_CPUS:
            l3_cmd = ','.join(self.gen_uc_pmu_events())
        if cpu_type in PA_CPUS:
            pa_cmd = ','.join(self.gen_pa_pmu_events())

        event_list = []
        if ddr_cmd:
            event_list.append(ddr_cmd)
        if hha_cmd:
            event_list.append(hha_cmd)
        if l3_cmd:
            event_list.append(l3_cmd)
        if pa_cmd:
            event_list.append(pa_cmd)
        e_arg = ','.join(event_list)

        if cpu_type in SUPPORT_CPUS:
            perf_cmd = f'perf stat -x , -a -I {REPORT_INTERVAL_MS} -e {e_arg}'
        else:
            print(f"cpu_type is invalid,please select input cpu_type from {SUPPORT_CPUS}")
            sys.exit(1)
        print(f"generate perf cmd: {perf_cmd}")
        return perf_cmd

    def run_perf(self, cmd=''):
        proc = None
        try:
            proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            while proc.poll() is None:
                line = proc.stdout.readline()
                if not line:
                    continue
                line = str(line, encoding='utf-8', errors='ignore').strip('\n')
                if 'failed' in line.lower():
                    continue
                self._parse(line)
        except KeyboardInterrupt:
            print("\nGot Keyboard interrupt, stop collecting")
        finally:
            if proc and proc.poll() is None:
                proc.terminate()

    def _parse(self, line: str):
        """
        perf stat -x,输出格式
        [timestamp],[cputag],[agg_cpus],[value],[unit],[event],[run_time],[pct]
        """
        record = line.split(',')
        if len(record) < 6:
            return
        try:
            time = float(record[0])
            value = int(record[1])
            event = record[5]
        except (ValueError, IndexError):
            return

        if self._time == 0.0:
            self._time = time
        if time < 2:
            return

        if time - self._time >= self._interval:
            ddr_data = self._ddr.report()
            hha_data = self._hha.report()
            l3c_data = self._l3c.report()
            uc_data = self._uc.report()
            pa_data = self._pa.report()

            print('\n' + "=" * 60)
            print(f"Sample Timestamp: {self._time:.2f}s")

            print('******************DDR*******************************')
            _header = ['TIME(s)', 'SCCL', 'DDR_READ(GiB/s)', 'DDR_WRITE(GiB/s)', 'SUM(GiB/s)']
            print('\t'.join(_header))
            for sccl, vals in ddr_data.items():
                print(f"{self._time:.2f}\t{sccl_scil_to_numa(sccl)}\t" + '\t'.join([f"{e:.3f}" for e in vals]))

            print('******************HHA*******************************')
            hha_header = ['TIME(s)', 'SCCL', 'TOTAL(gops)', 'CROSS_CHIP(gops)', 'CROSS_DIE(gops)', 'INNER_DIE(gops)',
                          'HHA_READ_DDR(GiB/s)', 'HHA_WRITE_DDR(GiB/s)', 'ACCESS_ALL(GiB/s)']
            print('\t'.join(hha_header))
            for sccl, vals in hha_data.items():
                print(f"{self._time:.2f}\t{sccl_scil_to_numa(sccl)}\t" + '\t'.join([f"{e:.3f}" for e in vals]))

            print('********************L3C/UC*****************************')
            l3c_header = ['TIME(s)', 'SCCL', 'READ_CPIPE', 'WRITE_CPIPE', 'READ_CPIPE_hit', 'WRITE_CPIPE_hit',
                          'READ_SPIPE', 'WRITE_SPIPE', 'READ_SPIPE_hit', 'WRITE_SPIPE_hit', 'L3_HIT_ALL', 'RET_TO_CPU_DATA']
            print('\t'.join(l3c_header))
            for sccl, vals in l3c_data.items():
                print(f"{self._time:.2f}\t{sccl_scil_to_numa(sccl)}\t" + '\t'.join([f"{e:.3f}" for e in vals]))

            print('*********************PA*****************************')
            pa_header = ['TIME(s)', 'SICL', 'RING2PA(G/s)', 'PA2RING(G/s)',
                         'RING2PA_LK0', 'RING2PA_LK1', 'RING2PA_LK2', 'RING2PA_LK3',
                         'PA2RING_LK0', 'PA2RING_LK1', 'PA2RING_LK2', 'PA2RING_LK3']
            print('\t'.join(pa_header))
            for sicl, vals in pa_data.items():
                print(f"{self._time:.2f}\t{sccl_scil_to_numa(sicl)}\t" + '\t'.join([f"{e:.3f}" for e in vals]))

            self._ddr.clear()
            self._hha.clear()
            self._l3c.clear()
            self._uc.clear()
            self._pa.clear()
            self._time = time

        if L3C_DEV_NAME in event:
            self._l3c.push(event, value)
        if DDR_DEV_NAME in event:
            self._ddr.push(event, value)
        if HHA_DEV_NAME in event:
            self._hha.push(event, value)
        if PA_DEV_NAME in event:
            self._pa.push(event, value)
        if UC_DEV_NAME in event:
            self._uc.push(event, value)


class DDRRecord:
    def __init__(self, ddr_topo):
        self._topo = ddr_topo
        self._records = {ddrc: {pmu: 0 for pmu in DDR_EVENTS} for ddrc in self._topo}
        self._device_regex = re.compile(r'(hisi.*)/config=(.*)/')

    def push(self, event: str, value: int):
        name, pmu = self._get_device_name(event)
        self._records[name][pmu] = value

    def _get_device_name(self, event: str):
        grps = re.match(self._device_regex, event)
        if not grps:
            raise ValueError(f'unexpected event pattern {event}')
        name = grps.groups()[0]
        pmu = grps.groups()[1]
        return name, pmu

    def report(self):
        sccls = list(set([self._topo[dev] for dev in self._topo]))
        sum_d = {sccl: {pmu: 0 for pmu in DDR_EVENTS} for sccl in sccls}
        for ddrc in self._records:
            sccl = self._topo[ddrc]
            for pmu in self._records[ddrc]:
                sum_d[sccl][pmu] += self._records[ddrc][pmu]
        ret = {}
        for sccl in sum_d:
            rdbw = sum_d[sccl]['0x84'] * 32 / 1024 / 1024 / 1024
            wrbw = sum_d[sccl]['0x83'] * 32 / 1024 / 1024 / 1024
            bwsum = rdbw + wrbw
            ret[sccl] = [rdbw, wrbw, bwsum]
        return ret

    def clear(self):
        self._records = {ddrc: {pmu: 0 for pmu in DDR_EVENTS} for ddrc in self._topo}


class L3CRecord(DDRRecord):
    def __init__(self, l3c_topo):
        super().__init__(l3c_topo)
        self._records = {l3c: {pmu: 0 for pmu in L3C_EVENTS} for l3c in self._topo}

    def report(self):
        sccls = list(set([self._topo[dev] for dev in self._topo]))
        sum_d = {sccl: {pmu: 0 for pmu in L3C_EVENTS} for sccl in sccls}
        for l3c in self._records:
            sccl = self._topo[l3c]
            for pmu in self._records[l3c]:
                sum_d[sccl][pmu] += self._records[l3c][pmu]
        ret = {}
        for sccl in sum_d:
            rCPIPE = (sum_d[sccl]['0x00'] + sum_d[sccl]['0x0f']) * 64 / 1024 / 1024 / 1024
            wCPIPE = sum_d[sccl]['0x01'] * 64 / 1024 / 1024 / 1024
            rCPIPE_hit = (sum_d[sccl]['0x02'] + sum_d[sccl]['0x0f']) * 64 / 1024 / 1024 / 1024
            wCPIPE_hit = sum_d[sccl]['0x03'] * 64 / 1024 / 1024 / 1024
            rSPIPE = (sum_d[sccl]['0x20'] + sum_d[sccl]['0x0f']) * 64 / 1024 / 1024 / 1024
            wSPIPE = sum_d[sccl]['0x21'] * 64 / 1024 / 1024 / 1024
            rSPIPE_hit = sum_d[sccl]['0x22'] * 64 / 1024 / 1024 / 1024
            wSPIPE_hit = sum_d[sccl]['0x23'] * 64 / 1024 / 1024 / 1024
            L3_hit = sum_d[sccl]['0x48'] * 64 / 1024 / 1024 / 1024
            RET_TO_CPU_DATA = sum_d[sccl]['0xB8'] * 64 / 1024 / 1024 / 1024
            ret[sccl] = [rCPIPE, wCPIPE, rCPIPE_hit, wCPIPE_hit, rSPIPE, wSPIPE, rSPIPE_hit, wSPIPE_hit, L3_hit, RET_TO_CPU_DATA]
        return ret

    def clear(self):
        self._records = {l3c: {pmu: 0 for pmu in L3C_EVENTS} for l3c in self._topo}


class HHARecord(DDRRecord):
    def __init__(self, hha_topo):
        super().__init__(hha_topo)
        self._records = {hha: {pmu: 0 for pmu in HHA_EVENTS} for hha in self._topo}

    def report(self):
        sccls = list(set([self._topo[dev] for dev in self._topo]))
        sum_d = {sccl: {pmu: 0 for pmu in HHA_EVENTS} for sccl in sccls}
        ret = {}
        for hha in self._records:
            sccl = self._topo[hha]
            for pmu in self._records[hha]:
                sum_d[sccl][pmu] += self._records[hha][pmu]
        for sccl in sum_d:
            total_cnt = sum_d[sccl]['0x00']
            cross_socket_cnt = sum_d[sccl]['0x01']
            cross_die_cnt = sum_d[sccl]['0x02']
            inner_die_cnt = total_cnt - cross_die_cnt - cross_socket_cnt

            total_bw = total_cnt / 1024 / 1024 / 1024
            cross_socket_bw = cross_socket_cnt / 1024 / 1024 / 1024
            cross_die_bw = cross_die_cnt / 1024 / 1024 / 1024
            inner_die_bw = inner_die_cnt / 1024 / 1024 / 1024

            read_ddr_bw = sum_d[sccl]['0x1c'] * 64 / 1024 / 1024 / 1024 + sum_d[sccl]['0x1e'] * 128 / 1024 / 1024 / 1024
            write_ddr_bw = sum_d[sccl]['0x1d'] * 64 / 1024 / 1024 / 1024 + sum_d[sccl]['0x1f'] * 128 / 1024 / 1024 / 1024
            access_ddr_all = read_ddr_bw + write_ddr_bw
            ret[sccl] = [total_bw, cross_socket_bw, cross_die_bw, inner_die_bw, read_ddr_bw, write_ddr_bw, access_ddr_all]
        return ret

    def clear(self):
        self._records = {hha: {pmu: 0 for pmu in HHA_EVENTS} for hha in self._topo}


class PARecord(DDRRecord):
    def __init__(self, pa_topo):
        super().__init__(pa_topo)
        self._records = {pa: {pmu: 0 for pmu in PA_EVENTS} for pa in self._topo}

    def report(self):
        sicls = list(set([self._topo[dev] for dev in self._topo]))
        sum_d = {sicl: {pmu: 0 for pmu in PA_EVENTS} for sicl in sicls}
        ret = {}
        for pa in self._records:
            sicl = self._topo[pa]
            for pmu in self._records[pa]:
                sum_d[sicl][pmu] += self._records[pa][pmu]

        pa2ring_bw_pmus = ['0x5C', '0x5D', '0x5E', '0x5F']
        pa2ring_link0_pmus = ['0x60', '0x64', '0x68', '0x6C']
        pa2ring_link1_pmus = ['0x61', '0x65', '0x69', '0x6D']
        pa2ring_link2_pmus = ['0x62', '0x66', '0x6A', '0x6E']
        pa2ring_link3_pmus = ['0x63', '0x67', '0x6B', '0x6F']
        ring2pa_bw_pmus = ['0x40', '0x41', '0x42', '0x43', '0x44', '0x45', '0x46', '0x47',
                           '0x48', '0x49', '0x4A', '0x4B', '0x4C', '0x4D', '0x4E', '0x4F',
                           '0x58', '0x59', '0x5A', '0x5B', '0x0D', '0x0E']
        ring2pa_link0_pmus = ['0x40', '0x44', '0x48', '0x4C', '0x58']
        ring2pa_link1_pmus = ['0x41', '0x45', '0x49', '0x4D', '0x59']
        ring2pa_link2_pmus = ['0x42', '0x46', '0x4A', '0x4E', '0x5A']
        ring2pa_link3_pmus = ['0x43', '0x47', '0x4B', '0x4F', '0x5B']

        for sicl in sum_d:
            cycle_cnt = sum_d[sicl]['0x78']
            if cycle_cnt == 0:
                ret[sicl] = [0.0] * 10
                continue
            ring2pa_bw = 0
            pa2ring_bw = 0
            ring2pa_link0 = 0
            ring2pa_link1 = 0
            ring2pa_link2 = 0
            ring2pa_link3 = 0
            pa2ring_link0 = 0
            pa2ring_link1 = 0
            pa2ring_link2 = 0
            pa2ring_link3 = 0

            for p in ring2pa_bw_pmus:
                ring2pa_bw += sum_d[sicl][p]
            ring2pa_bw = ring2pa_bw / cycle_cnt * 30

            for p in pa2ring_bw_pmus:
                pa2ring_bw += sum_d[sicl][p]
            pa2ring_bw = pa2ring_bw / cycle_cnt * 30

            for p in ring2pa_link0_pmus:
                ring2pa_link0 += sum_d[sicl][p]
            ring2pa_link0 = ring2pa_link0 / cycle_cnt * 30

            for p in ring2pa_link1_pmus:
                ring2pa_link1 += sum_d[sicl][p]
            ring2pa_link1 = ring2pa_link1 / cycle_cnt * 30

            for p in ring2pa_link2_pmus:
                ring2pa_link2 += sum_d[sicl][p]
            ring2pa_link2 = ring2pa_link2 / cycle_cnt * 30

            for p in ring2pa_link3_pmus:
                ring2pa_link3 += sum_d[sicl][p]
            ring2pa_link3 = ring2pa_link3 / cycle_cnt * 30

            for p in pa2ring_link0_pmus:
                pa2ring_link0 += sum_d[sicl][p]
            pa2ring_link0 = pa2ring_link0 / cycle_cnt * 30

            for p in pa2ring_link1_pmus:
                pa2ring_link1 += sum_d[sicl][p]
            pa2ring_link1 = pa2ring_link1 / cycle_cnt * 30

            for p in pa2ring_link2_pmus:
                pa2ring_link2 += sum_d[sicl][p]
            pa2ring_link2 = pa2ring_link2 / cycle_cnt * 30

            for p in pa2ring_link3_pmus:
                pa2ring_link3 += sum_d[sicl][p]
            pa2ring_link3 = pa2ring_link3 / cycle_cnt * 30

            ret[sicl] = [ring2pa_bw, pa2ring_bw,
                         ring2pa_link0, ring2pa_link1, ring2pa_link2, ring2pa_link3,
                         pa2ring_link0, pa2ring_link1, pa2ring_link2, pa2ring_link3]
        return ret

    def clear(self):
        self._records = {pa: {pmu: 0 for pmu in PA_EVENTS} for pa in self._topo}


class UCRecord(DDRRecord):
    def __init__(self, uc_topo):
        super().__init__(uc_topo)
        self._records = {uc: {pmu: 0 for pmu in UC_EVENTS} for uc in self._topo}

    def report(self):
        sccls = list(set([self._topo[dev] for dev in self._topo]))
        sum_d = {sccl: {pmu: 0 for pmu in UC_EVENTS} for sccl in sccls}
        ret = {}
        for uc in self._records:
            sccl = self._topo[uc]
            for pmu in self._records[uc]:
                sum_d[sccl][pmu] += self._records[uc][pmu]
        for sccl in sum_d:
            rd64 = sum_d[sccl]['0x19'] * 64 / 1024 / 1024 / 1024
            rd128 = sum_d[sccl]['0x1a'] * 128 / 1024 / 1024 / 1024
            cring_snp = sum_d[sccl]['0x74']
            uring_snp = sum_d[sccl]['0x7d']
            ret[sccl] = [rd64, rd128, cring_snp, uring_snp]
        return ret

    def clear(self):
        self._records = {uc: {pmu: 0 for pmu in UC_EVENTS} for uc in self._topo}


def main(argv):
    cpu_type = ''
    try:
        opts, args = getopt.getopt(argv, "hc:", ["cpu_type="])
    except getopt.GetoptError:
        print("usage: python3 pmu_mon.py -c <920B|920C|920E|920F>")
        sys.exit(2)
    for opt, arg in opts:
        if opt == '-h':
            print('python3 pmu_mon.py -c <cpu_type>')
            print('cpu_type: 920B / 920C /920E /920F')
            sys.exit()
        elif opt in ("-c", "--cpu_type"):
            cpu_type = arg

    if os.geteuid() != 0:
        print("ERROR: must run with root!")
        sys.exit(1)

    topology = Topology()
    cmd = topology.gen_perf_stat_cmd(cpu_type)
    topology.run_perf(cmd)


if __name__ == '__main__':
    main(sys.argv[1:])
