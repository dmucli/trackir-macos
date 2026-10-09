# TrackIR 5.5.3 — reverse-engineered specification

Everything here was recovered with Ghidra from the binaries that NaturalPoint's TrackIR 5.5.3 installer puts in
`Program Files/TrackIR5/` (`TrackIR5.exe`, `NPClient.dll`, `NPClient64.dll`; `tools/extract-trackir.sh` unpacks them
on a Mac). Addresses are virtual addresses at the default image bases; function names are the names given to those
functions during the analysis.

Confidence markers: **[verified]** = read directly from decompiled/disassembled code;
**[inferred]** = consistent with the code but not observed on real hardware.

---

## 1. Game-facing contract (`NPClient.dll` / `NPClient64.dll`)

### 1.1 Transport

| Item | Value |
|---|---|
| Shared memory | named file mapping `Local\SharedTrackIRData`, size **0x1E6** bytes, created by whichever side opens first (`OpenSharedTrackIRMapping` @ `10001fb0`) **[verified]** |
| Lock | mutex `NPClientMutex`, waited with a 25 ms timeout around every data read/write **[verified]** |
| Commands | `SendMessageA(FindWindowA(NULL,"NaturalPoint"), 0x405 /*WM_USER+5*/, cmd, lParam)` **[verified]** |
| Result of a command | TrackIR writes it into `block.lastResult` (offset 2); the DLL clears it before sending **[verified]** |

### 1.2 Shared block layout (packed, little endian) **[verified]**

| Off | Size | Field | Notes |
|---|---|---|---|
| 0x000 | u16 | version | TrackIR writes `0x0505` (`NPPriv_SetVersion`), DLL initialises `0x0100` |
| 0x002 | u32 | lastResult | `NPPriv_SetLastError` / command result |
| 0x006 | u32 | param[0] | `NP_GetParameter(0)`; TrackIR sets 1 |
| 0x00A | u32 | param[1] | |
| 0x00E | u32 | param[2] | the only one games may write (`NP_SetParameter(2,0)`) |
| 0x012 | char[200] | sigDll | `"precise head tracking\n put your head into the game\n now go look around\n\n Copyright EyeControl Technologies"` |
| 0x0DA | char[200] | sigApp | `"hardware camera\n software processing data\n track user movement\n\n Copyright EyeControl Technologies"` |
| 0x1A2 | 68 | TRACKIRDATA | see below |

`NP_GetSignature` copies the 400 bytes at 0x12. Games compare them against the two strings above.

TRACKIRDATA (68 bytes): `u16 status; u16 frameSignature; u32 ioData; float roll, pitch, yaw, x, y, z,
rawX, rawY, rawZ, deltaX, deltaY, deltaZ, smoothX, smoothY, smoothZ;`

* Units: rotations `deg/180*16383`, translations `t/50*16383`, both clamped to ±16383 (`RunTrackingFrameLoop` @ `00497120`).
* `frameSignature` increments per published frame while transmitting, wraps from 0x7FFF to 1.

### 1.3 Command codes (wParam of message 0x405) **[verified]**

| Code | Export | lParam |
|---|---|---|
| 0x3F2 | NP_RegisterProgramProfileID | profile id (`LockOn.exe`→0x3EA, `nks.exe`+0x3EA→0x2A95) |
| 0x3FC | NP_RegisterWindowHandle | HWND |
| 0x3FD | NP_UnregisterWindowHandle | 0 |
| 0x406 | NP_RequestData | data-field mask |
| 0x7DA | NP_StartDataTransmission | 0 (some exe names auto-register an ID first) |
| 0x7E4 | NP_StopDataTransmission | 0 |
| 0xBC2 | NP_StartCursor | 0 |
| 0xBCC | NP_StopCursor | 0 |
| 0xBD6 | NP_ReCenter | 0 |

Hard-coded exe → profile id in `NP_StartDataTransmission`: SwgClient_r.exe 0x1771, targetware.exe 0x4B2,
tir2joy.exe 0x89A, ww2.exe 0xCE5, TIRF4.exe 0x89B, CRC_DEMO.exe 0x12C1, mf.exe 0x13ED, Trainz.exe 0x125D.

Return codes used: 0 OK, 1 TrackIR not running (no window), 3 bad parameter, 5 no mutex, 6 no mapping / lock timeout,
9 read-only parameter, 0x65 `NP_GetDataEX` called without key, 100 checksum mismatch.

### 1.4 "Enhanced" game encryption **[verified]**

For a registered game with an 8-byte key `k` (from `sgl.dat`), TrackIR (`PublishTrackIRDataToNPClient` @ `0048b7c0`) does:

1. fill rawX..smoothZ (bytes 0x20..0x43) with `(float)rand()`;
2. `ioData = 0; ioData = hash(data)`;
3. encrypt in place, from the last byte down:
   ```
   s = 0x88; j = 0
   for i = 0x43 .. 0:  p = d[i]; d[i] = k[j] ^ p ^ s; s = s + p + i; j = (j+1) & 7
   ```

`NP_GetDataEX(data, keyLo, keyHi)` reverses step 3, zeroes bytes 0x20..0x43 and checks the hash (returns 0 / 100).

`hash` is Paul Hsieh's SuperFastHash over the 68 bytes with seed 0x44, **but** with sign-extended 16-bit reads
and arithmetic (signed) right shifts:
```
h = 0x44
for 17 words (s0,s1 = signed int16 pairs):  h += s0; h ^= ((s1 ^ (h<<5)) << 11)   // == (h<<16) ^ (s1<<11) ^ h
                                            h += (int32)h >> 11
h ^= h<<3; h += (int32)h>>5; h ^= h<<4; h += (int32)h>>17; h ^= h<<25; h += (int32)h>>6
```

---

## 2. Camera discovery (TrackIR5.exe, NaturalPoint CameraLibrary)

The app does **not** use HID for the camera (HID is only an "Armory" U2F/licence path). Cameras are opened through the
Thesycon USBIO driver (`\\.\USBIO_Device%d` / device-interface GUIDs). The device path is matched against a table
(`InitUsbDevicePathRevisionMap` @ `00401f10`) and `CreateCameraForRevision` @ `005921c0` instantiates the class:

| USB id | Revision enum | Class | Notes |
|---|---|---|---|
| 131d:0156 | 2 | CameraRev5 | "TrackIR4", classic protocol, FPGA image #5 (Xilinx 3S100E, `trackir5.ncd`) |
| 131d:0157 | 6 | CameraRev9 | "TrackIR5", classic protocol, FPGA image #9 |
| 131d:0158 | 0xF | CameraRev18 | Rev9 derivative, FPGA image #0x12 |
| **131d:0159** | **0x1E** | **CameraRev35** | **current TrackIR 5**, secure protocol, no host FPGA upload |
| 131d:0106/0115/0116/0126/0127/0128/0129/012A/012C/0130/0131, 25be:0001/0002 | other | OptiTrack / SmartNav products | out of scope |

### 2.1 USB transport **[verified]** / endpoints **[inferred]**

* Configuration 1, interface 0. The pipe list of the interface is read in order: pipe[0] = command writer,
  pipe[1] = reader (4 outstanding 16 KiB reads). Some products have extra pipes, TrackIR does not use them.
  On a TrackIR these are expected to be bulk OUT 0x01 and bulk IN 0x82; the port picks the first bulk OUT and first
  bulk IN endpoint from the descriptors, mirroring the original's order-based selection.
* Writes: one USB transfer per command, 1 s timeout.

### 2.2 Inbound packet routing (`DispatchCameraPipePacket` @ `00583e30`) **[verified]**

For a packet `P` of length `n >= 2`:

* `P[1] & 0xF0 == 0x10` → frame data (§2.4), the full packet is passed on.
* otherwise the pending command whose expected type equals `P[1] & 0xF0` gets `(P+1, n-1)`:
  `0x20` status (`ReadCameraStatus`), `0x40` config (`ReadConfigData`), `0x50` firmware version.

### 2.3 Frame packet header/trailer (`ValidateFramePacketChecksum` @ `005a4280`) **[verified]**

```
P[0] = frame counter   P[1] = 0x1?   P[2] = frame type   P[3] = P[0]^P[1]^P[2]^0xAA
last 4 bytes = big-endian (n - 8)
```

### 2.4 Frame types **[verified]**

* **Type 5** (default for Rev9/Rev35, `ParseFrameType5Segments` @ `005a3720`): 8-byte records from offset 4:
  ```
  x   = b0<<2 | b1>>6                     (10 bits)
  y   = (b1&0x3F)<<3 | b2>>5              (9 bits)
  len = (b2&0x1F)<<5 | b3>>3              (10 bits)   → run covers x .. x+len-1 on row y
  m1  = (((b3&7)<<8 | b4)<<8 | b5)<<1 | b6>>7   (19 bits, segment +8:   sum of i*I, i counted from x)
  m0  = (b6<<8 | b7) & 0x7FFF                   (15 bits, segment +0xC: sum of I, the run's intensity)
  ```
  Records with y == 0 or x+len > width are dropped. The blob centre (`FUN_00588600`, which accumulates 64-bit
  sums at object +0x18/+0x20/+0x28) is sub-pixel:
  `x = sum(m1 + x*m0) / sum(m0)`, `y = sum(y*m0) / sum(m0)`. **[verified on hardware]** one-pixel runs carry m1 = 0;
  m0 grows linearly with the run length (~140 per pixel) and m1 roughly quadratically.
* **Type 0** (`ParseFrameType0Segments` @ `005a40d0`): 4-byte records, half-pixel x:
  `f=b3; y = b0 + ((f&0x20) + (f&4)*8)*8;  x0 = (b1 | (f&0x80)<<1 | (f&0x10)<<5 | (f&2)<<9)/2;
   x1 = (b2 | (f&0x40)<<2 | (f&8)<<6 | (f&1)<<10)/2`.
  (Bits 5 and 2 of `f` both add 0x100 to `y`; that is what the disassembly at `005a416c` does.)
* **Type 4**: grayscale runs `[xlo, ylo, hi]` + pixel bytes terminated by 0 (`005a3a20`).
* **Type 3**: raw grayscale stripes, 64-column interleave (`005a38f0`).

Sensor for TrackIR 5: 640×480.

---

## 3. Classic command protocol (Rev5 / Rev9 / Rev18) **[verified]**

Plain byte commands on the OUT pipe:

| Purpose | Bytes |
|---|---|
| FPGA load begin | `1B` |
| FPGA data chunk | `1C n data[n]` (n ≤ 60) |
| Read firmware version | `16` |
| Flush pair (before status/start) | `12`, `13` |
| Read status | `12`, `13`, `1D` (status reply type 0x20; reply bytes [3..4] BE = FPGA checksum) |
| After upload | `19 05 10 10 00` (reg 5 = 0x1000), status, `20` |
| Write register | `19 reg sub hi lo` (sub is 0x10 in practice, 0x0F for reg 0x0C) |
| Write imager register | `23 a b c 00 00` |
| Start streaming | `14 00` |
| Stop (Rev9) | 5×`14 01`, 10 ms, `12 01`, `13 01`, 5×`14 01`, 50 ms |
| IR LEDs off | `10 00 80` |
| Threshold | `15 thr 01 00` |
| Shutdown | IR off, `1B` |

Registers: 3 = video type (0,3,4,5), 4 = LED word `(statusIntensity<<8)|ledMask`, 5 = 0x1000 after FPGA load,
9 = IR intensity `((v&~1)<<7)|1` (0 = off), 0x20 = frame rate {25,50,100}.
Exposure (Rev9): imager `42 08 v>>8`, `42 10 v&0xFF`, v ∈ [0,479].
Status-LED intensity: `v>>6` → 0:3, 1:2, 2:1, else 0.

FPGA checksum: `s = 0; for b in image: s = (s + b) ^ (b << 4)`; compared as 16 bits.
The camera re-uploads only when the reported checksum differs.

Initialisation (`Rev9 InitializeCamera` @ `005a2b60`): threshold 150, exposure 479, intensity 10, LEDs 0x33 off,
status intensity 255, video type 5.

FPGA images live raw in `.rdata` (`GetEmbeddedResourceBlob` @ `0059d670`):

| Resource | VA | Size | Used by |
|---|---|---|---|
| 5 | 0x008EB9A0 | 0x11C29 | Rev5 |
| 9 | 0x00938750 | 0x08CEC | Rev9 |
| 0x12 | 0x00D51AB8 | 0x08D31 | Rev18 |

They are extracted at runtime from the user's own `TrackIR5.exe` (`trackir-mac extract-fpga`); nothing proprietary
ships with the port.

---

## 4. Secure protocol (Rev35, 131d:0159) **[verified on hardware 2026-10-09]**

A real TrackIR 5 (131d:0159, full speed, interface 0 with bulk OUT 0x01 / IN 0x82, no string descriptors)
completed the handshake and XTEA authentication against the port and streamed type-5 frames at 123 frames/s.

### 4.1 Packet construction

Every command is a **24-byte** packet that starts as 24 random bytes:

* **Simple** (`Rev35 +0x4b8`): `buf[0] = cmd`.
* **Field** (`Rev35_BuildFieldPacket` @ `005bca20`): `idx = rand(6..14)`; `buf[0] = cmd`;
  idx bits are hidden as `buf[1].bit3=idx.bit3, buf[2].bit2=idx.bit2, buf[3].bit1=idx.bit1, buf[4].bit0=idx.bit0`;
  `buf[idx] = buf[0x10]^a`, `buf[idx-1] = buf[0x13]^b`, `buf[idx+1] = buf[0x12]^c`.
* **Handshake** (`Rev35_BuildHandshakePacket` @ `005bcaa0`): `idx = rand(2..14)`, `r = rand(0..3)`, `buf[0] = 0x1A`,
  same idx bit hiding, `buf[idx] = ((buf[idx]&0x0F) | step<<4) ^ (buf[0x10]&0xF0)`, `buf[idx+1] = (buf[idx+1]&0x3F) | r<<6`.

Then every packet is obfuscated (`Rev35_ObfuscateAndSendPacket` @ `005bd7a0`):
```
hi = rand(1..15); lo = rand(1..15); buf[0x11] = hi<<4 | lo
buf[0] ^= buf[lo];  buf[0] ^= buf[hi] ^ 0x69
```

Commands: `0x13` (end of reset), `0x17` (read config), `0x19 reg hi lo` (write register),
`0x1A step` (handshake), `0x1F` (random nonce), `0x23 a b c` (imager register).

### 4.2 Handshake / camera authentication

Handshake step 7 is the status poll. When building it the host records:
`expectedLen = 14 + r`, `expectByte = buf[idx+2] ^ buf[0x12] ^ idx`, `stateXor = buf[0x13] ^ buf[0x0D]`,
`nonce = buf[idx>>1 .. +8]`. Step 0 selects the key: `key = TABLE[buf[0x15] & 7]` (8 × 128-bit keys at VA 0x01850D80).

The reply `R` (after stripping the first byte, see §2.2) must satisfy (`Rev35_VerifyHandshakeResponse` @ `005bd6c0`):
`R[0]==0x20, R[1]==0x01, len(R)==expectedLen, R[4]==expectByte, R[6..13] == out`,
where `out = XTEA_key(nonce) ^ nonce` (XTEA, 32 cycles, standard delta; `XteaEncipherBlock` @ `005a4ce0`),
after which `key = {out.lo, out.hi, 0, 0}`. The camera state acknowledged is `R[5] ^ stateXor`.
The key only lets the *host* authenticate the *camera*; the host needs no secret to drive it.

### 4.3 State machine (`Rev35_HandshakeStateMachine` @ `005bd300`)

```
0xE --reset--> sleep 30 ms, state 0, send HS(0) ×2, send 0x13
0   --reply ack 0--> state 1, send HS(1)        (nack → resend HS(0))
1   --reply ack 1--> sleep 5 ms, state 2, send HS(2)
2   --reply ack 2--> send HS(3);  --reply ack 3--> state 3 (ready, host then sends 0x17)
3   --start--> state 4, send HS(4)   --event 6--> send HS(6)   --reset--> state 0, HS(0)
4   --stop --> state 3, send HS(5)
any --bad reply--> state 0xF (error)
```
After every transition the host sends a status poll (HS(7)) and feeds the reply back in.

### 4.4 Rev35 settings

| Setting | Wire |
|---|---|
| Threshold t (0..255) | reg 5 = 2t |
| IR intensity | reg 9 = (v != 0) |
| Video type | reg 3 |
| LEDs | reg 4 = `((statusIntensity<<4 | mask&3)<<4) | (mask>>4 & 3)` |
| Exposure v (1..479) | imager `35 02 v<<4`, `35 01 v>>4`, `35 00 v>>12`, `3B 8F v&FF`, `3B 8E v>>8` |
| Stop | event 5, reg 9 = 0, then HS(6) and `0x13` |

Init (`Rev35_InitializeCamera` @ `005bcd70`): threshold 150, exposure 120, intensity 1, video type 5.

---

## 5. Optics and pose

Lens model (`+0xC8`, principal point = image centre) **[verified]**:

| Class | f (px) | k1 | k2 | k3 |
|---|---|---|---|---|
| Rev9/Rev18 | 630 | 0.0614822 | −0.6844258 | 0.0143133 |
| Rev35, serial < 400891 | 583 | −0.0862667 | 0.230944 | 0 |
| Rev35, serial ≥ 400891 | 594 | −0.0960102 | 0.424944 | 0 |

Marker model (`cModuleVector` defaults, solver object `005cc480`) **[verified]**: `|P0P1| = |P0P2| = 116.052 mm`,
`|P1P2| = 69.621 mm`, so P0 is the apex.

Marker selection (`SelectVectorMarkers` @ `005cbd20`) keeps blob ids from frame to frame; when it has to re-sort,
clip types 0 and 1 order the markers by image y, top first (P0 = top). Type 2 picks the marker lying between the
other two in both x and y.

Solver (`005cc4f0`) **[verified]**: not closed-form P3P. Image points become rays `(u·pitch, −f, v·pitch)` (optical
axis along −Y), and Newton–Raphson solves the three depths from the law-of-cosines equations for the pairs
(P0,P1), (P0,P2), (P1,P2), starting each frame from fixed depths:

| Clip | P0 | P1 | P2 |
|---|---|---|---|
| type 0 (TrackClip) | 700 | 500 | 500 |
| type 1 (TrackClip PRO) | 780 | 830 | 780 |

The fixed start is what selects the physical branch of the 3-point mirror ambiguity (for type 0: apex farther from the
camera than the base). At most 19 iterations, stopping at 1e-15 relative error. The 3-D points are depth × ray
(`005cd1a0`). The port keeps this solver and falls back to Grunert's closed form only when Newton fails.

Camera geometry reported to the solver (`+0xF0/+0xF4/+0xF8`): Rev35 1.92 × 1.44 mm imager, 1.67 mm lens;
Rev9 3.84 × 2.88 mm, 3.99 mm. **[inferred]** The port uses the calibrated focal lengths from the lens model above
instead, since the two disagree by about 6%.

## 6. Output pipeline (`RunTrackingFrameLoop` @ `00497120`) **[verified]**

Per axis (profile `Axis` 0..5 = yaw, pitch, roll, x, y, z):
```
out = sign(in) * ∫_0^|in| f(t) dt      // f: piecewise-linear through the profile's 11 (x, slope) points
if Inverted: out = -out
out += recenterOffset; clamp to axis limit
```
(`IntegrateLinearSlopeCurve` @ `0045a490`, trapezoid rule). Pitch and X are negated before publishing.
