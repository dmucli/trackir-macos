"""Replays the port's Rev35 packet stream through TrackIR5.exe's own code (CameraRev35 methods) under Unicorn."""
import struct, subprocess, sys
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import *

exe, harness, keys = sys.argv[1:4]
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(base, (len(image) + 0xFFF) & ~0xFFF)
uc.mem_write(base, image)

OBJ, BUF, PKT, STACK, STUB, RET = 0x60000000, 0x61000000, 0x62000000, 0x63000000, 0x64000000, 0x65000000
for a in (OBJ, BUF, PKT, STUB, RET):
    uc.mem_map(a, 0x10000)
uc.mem_map(STACK, 0x100000)
uc.mem_write(OBJ, struct.pack('<I', 0x01850E70))  # CameraRev35 vftable

RAND_S_IAT, SEND = 0x0080DF78, 0x00587DF0
BUILD_FIELD, BUILD_HS, BUILD_SIMPLE, OBFUSCATE, VERIFY, CHECK_STATE = 0x5BCA20, 0x5BCAA0, 0x5BC9D0, 0x5BD7A0, 0x5BD6C0, 0x5BC930
uc.mem_write(RAND_S_IAT, struct.pack('<I', STUB))
uc.mem_write(STUB, b'\xc3')

rand_queue, sent = [], []
def hook(uc, addr, size, _):
    esp = uc.reg_read(UC_X86_REG_ESP)
    if addr == STUB:  # errno_t rand_s(unsigned int*), cdecl
        ra, ptr = struct.unpack('<II', uc.mem_read(esp, 8))
        uc.mem_write(ptr, struct.pack('<I', rand_queue.pop(0)))
        uc.reg_write(UC_X86_REG_EAX, 0)
        uc.reg_write(UC_X86_REG_ESP, esp + 4)
        uc.reg_write(UC_X86_REG_EIP, ra)
    elif addr == SEND:  # thiscall Send(buf, len): capture and return
        ra, buf, n = struct.unpack('<III', uc.mem_read(esp, 12))
        sent.append(bytes(uc.mem_read(buf, n)))
        uc.reg_write(UC_X86_REG_ESP, esp + 12)
        uc.reg_write(UC_X86_REG_EIP, ra)
uc.hook_add(UC_HOOK_CODE, hook, begin=SEND, end=SEND)
uc.hook_add(UC_HOOK_CODE, hook, begin=STUB, end=STUB)

def thiscall(fn, *args):
    esp = STACK + 0x80000 - 4 * (len(args) + 1)
    uc.mem_write(esp, struct.pack('<%dI' % (len(args) + 1), RET, *args))
    uc.reg_write(UC_X86_REG_ESP, esp)
    uc.reg_write(UC_X86_REG_ECX, OBJ)
    uc.emu_start(fn, RET)
    return uc.reg_read(UC_X86_REG_EAX)

def u32(addr):
    return struct.unpack('<I', uc.mem_read(addr, 4))[0]

lines = subprocess.check_output([harness, keys]).decode().splitlines()
checked = mismatches = verified = 0
for line in lines:
    parts = line.split()
    kind, a, b, c = parts[0], int(parts[1]), int(parts[2]), int(parts[3])
    rand_queue[:] = [int(x, 16) for x in parts[4].split(',')]
    expected = bytes.fromhex(parts[5])
    sent.clear()
    uc.mem_write(BUF, b'\0' * 32)
    if kind == 'field':
        thiscall(BUILD_FIELD, BUF, 0x19, a, b, c)
    elif kind == 'simple':
        thiscall(BUILD_SIMPLE, BUF, a)
    else:
        thiscall(BUILD_HS, BUF, a)
    thiscall(OBFUSCATE, BUF, 24)
    checked += 1
    if sent != [expected] or rand_queue:
        mismatches += 1
        print('packet mismatch', kind, a, sent[0].hex() if sent else None, expected.hex())
        continue
    if kind == 'handshake' and a == 7:
        length, check, xor, nonce, reply = int(parts[6]), int(parts[7]), int(parts[8]), parts[9], bytes.fromhex(parts[10])
        got = (u32(OBJ + 0x1698), u32(OBJ + 0x169C), u32(OBJ + 0x16A0), bytes(uc.mem_read(OBJ + 0x16BC, 8)).hex())
        if got != (length, check, xor, nonce):
            mismatches += 1
            print('expectation mismatch', got, (length, check, xor, nonce))
        uc.mem_write(PKT, reply)
        ok = thiscall(VERIFY, 0xF, PKT, len(reply)) & 0xFF
        state_ok = thiscall(CHECK_STATE, 3, PKT) & 0xFF
        if ok and state_ok:
            verified += 1
        else:
            mismatches += 1
            print('TrackIR5.exe rejected the reply the port accepted', ok, state_ok)

print('%d packets compared, %d status replies verified by TrackIR5.exe, %d mismatches' % (checked, verified, mismatches))
sys.exit(1 if mismatches else 0)
