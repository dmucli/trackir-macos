"""Runs NaturalPoint's real NP_GetDataEX (NPClient.dll 5.5.3, x86) under Unicorn on data encrypted by the port."""
import struct, subprocess, sys
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UcError
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX

dll_path, gen_path = sys.argv[1], sys.argv[2]
pe = pefile.PE(dll_path)
base = pe.OPTIONAL_HEADER.ImageBase
image = pe.get_memory_mapped_image()
size = (len(image) + 0xFFF) & ~0xFFF

STUBS, BLOCK, STACK, RET = 0x30000000, 0x20000000, 0x40000000, 0x50000000
MAPPING_PTR, MUTEX = 0x10055B0C, 0x10055B10   # DAT_10055b0c / DAT_10055b10
GETDATAEX = 0x10001410

uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(base, size)
uc.mem_write(base, image)
for addr in (STUBS, BLOCK, RET):
    uc.mem_map(addr, 0x10000)
uc.mem_map(STACK, 0x100000)

# Import stubs: name -> (return value, bytes of stdcall arguments)
stub_behaviour = {b'FindWindowA': (0x1234, 8), b'WaitForSingleObject': (0, 8), b'ReleaseMutex': (1, 4)}
stubs = {}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in entry.imports:
        if imp.name in stub_behaviour:
            stub = STUBS + 16 * len(stubs)
            stubs[stub] = stub_behaviour[imp.name]
            uc.mem_write(imp.address, struct.pack('<I', stub))
            uc.mem_write(stub, b'\xc3')  # placeholder, the hook does the work
assert len(stubs) == 3, stubs

def hook(uc, address, size, _):
    if address in stubs:
        ret, argbytes = stubs[address]
        esp = uc.reg_read(UC_X86_REG_ESP)
        ra = struct.unpack('<I', uc.mem_read(esp, 4))[0]
        uc.reg_write(UC_X86_REG_EAX, ret)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + argbytes)
        uc.reg_write(UC_X86_REG_EIP, ra)
uc.hook_add(UC_HOOK_CODE, hook)

uc.mem_write(MAPPING_PTR, struct.pack('<I', BLOCK))
uc.mem_write(MUTEX, struct.pack('<I', 0x5678))

def get_data_ex(encrypted, key):
    uc.mem_write(BLOCK, b'\0' * 0x1E6)
    uc.mem_write(BLOCK + 0x1A2, encrypted)
    out = STACK + 0x80000
    uc.mem_write(out, b'\0' * 68)
    lo, hi = struct.unpack('<II', key)
    esp = STACK + 0x40000
    uc.mem_write(esp, struct.pack('<IIII', RET, out, lo, hi))
    uc.reg_write(UC_X86_REG_ESP, esp)
    uc.emu_start(GETDATAEX, RET)
    return uc.reg_read(UC_X86_REG_EAX), bytes(uc.mem_read(out, 68))

lines = subprocess.check_output([gen_path, '200']).decode().split()
ok = bad = 0
for i in range(0, len(lines), 3):
    key, plain, enc = (bytes.fromhex(x) for x in lines[i:i + 3])
    rc, dec = get_data_ex(enc, key)
    # NP_GetDataEX zeroes io_data and the raw/delta/smooth fields after checking the hash.
    want = plain[:4] + b'\0' * 4 + plain[8:32] + b'\0' * 36
    if rc == 0 and dec == want:
        ok += 1
    else:
        bad += 1
        print('mismatch rc=%d' % rc, dec.hex(), want.hex())
    wrong = bytes([key[0] ^ 1]) + key[1:]
    rc2, _ = get_data_ex(enc, wrong)
    if rc2 != 100:
        bad += 1
        print('wrong key accepted, rc=%d' % rc2)
print('real NP_GetDataEX accepted %d/%d port-encrypted frames; failures: %d' % (ok, len(lines) // 3, bad))
sys.exit(1 if bad else 0)
