#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import sys

from eboot import Eboot

failed = 0

def fact(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what)
    failed += not ok

def text(e, fn):
    return [f'{i.mnemonic} {i.op_str}' for i in e.disasm(fn)]

def has(lines, *needles):
    return all(any(n in l for l in lines) for n in needles)

def calls(e, fn, target):
    return any(t == target for _, t in e.calls_in(fn))

def rip(i):
    return i.address + i.size + next(op.mem.disp for op in i.operands if op.type == 3)

def main():
    e = Eboot(sys.argv[1] if len(sys.argv) > 1 else 'out/eboot.elf')

    names = [e.cstr(e.ptr(0x53398b0 + 8 * k)) for k in range(6)]
    fact(names == ['Top', 'LanguageSetting', 'ControllSetting', 'EnvironmentSetting', 'BrightnessSetting',
                   'NetworkSetting'], f'53398b0: the six section names {names}')
    fact(e.u64(0x53398e0) == 0 and e.u64(0x53398e8) == 0 and 0x53398e0 not in e.relative and
         0x53398e8 not in e.relative, '53398e0, 53398e8: slots 6 and 7 zero, no relocation')
    fact(e.is_function(0x1b20900), '1b20900 is a function')
    lea = [i for i in e.disasm(0x1b20900) if i.mnemonic == 'lea' and 'rip' in i.op_str and rip(i) == 0x53398b0]
    fact(bool(lea), '1b20900 reads the name table (lea at %s)' % [hex(i.address) for i in lea])
    loop = e.disasm(0x1b20e1e, 0xa)
    fact(e.read(0x1b20e1e, 4) == bytes([0x48, 0x83, 0xfa, 0x06]) and loop[1].mnemonic == 'jne' and
         int(loop[1].op_str, 16) < 0x1b20e1e, '1b20e1e: cmp rdx, 6; jne back: the loop over the six names')
    t = text(e, 0x1b20900)
    fact(has(t, 'mov esi, 0xf20', 'call qword ptr [rax + 0x58]'), '1b20900 allocates the 0xf20-byte dialog')
    fact(has(t, 'lea rbx, [r12 + 0xeb0]', 'mov rsi, rbx', 'call rax'), '1b20900 calls handler(dialog, dialog + 0xeb0)')
    fact(has(t, 'mov edx, 0x9d8a', 'mov edx, 0x9d8b'), '1b20900 adds Defaults: 0x9d8a (0x9d8b with r9 set)')
    fact(has(t, 'and rcx, 1', 'mov rax, qword ptr [rax + rcx - 1]'), '1b20900 reads an odd handler as a virtual slot')
    end = [i for i in e.disasm(0x1b20dc6, 0xa)]
    fact(end[0].op_str == 'esi, esi' and end[2].op_str == '0x1b28de0', '1b20dc6: 1b28de0(dialog, 0) lays the rows out')
    for opener, name in ((0x1bdb940, 'LanguageSetting'), (0x1bdb960, 'ControllSetting'), (0x1bdb980, 'EnvironmentSetting'),
                         (0x1bdb9a0, 'BrightnessSetting'), (0x1bdb9c0, 'NetworkSetting')):
        ins = e.disasm(opener, 0x19)
        fact(e.cstr(rip(ins[0])) == name and ins[2].op_str == 'r8d, r8d' and ins[3].op_str == 'r9d, r9d' and
             ins[4].mnemonic == 'jmp' and int(ins[4].op_str, 16) == 0x1b20900,
             f'{opener:x}: 1b20900(root, params, "{name}", handler, 0, 0)')

    t = text(e, 0x1bb3ad0)
    fact(e.function(0x1bb4978)[0] == 0x1bb3ad0 and e.disasm(0x1bb4978, 5)[0].op_str == '0x1bea630',
         '1bb4978: the System list ends with call 1bea630')
    fact(e.callers(0x1bea630).count(0x1bb4978) == 1, '1bea630 is called once from the System list')
    for k, msg in enumerate(range(0x1adb0, 0x1adb6)):
        fact(has(t, f'mov edx, {msg:#x}'), f'1bb3ad0: System row message {msg:#x} (group 200 caption, 201 help)')
    fact(has(t, 'mov esi, 0xc8', 'mov esi, 0xc9'), '1bb3ad0: captions from groups 200 and 201, 0x40 apart')
    vt = [rip(i) for i in e.disasm(0x1bb3ad0) if i.mnemonic == 'lea' and 'rip' in i.op_str]
    fact(0x533d120 in vt, '1bb3ad0: its rows hold functors with vtable 533d120')
    rows = [s for s in e.callers(0x1b4e2a0) if e.function(s)[0] == 0x1bb3ad0]
    exits = [s for s in e.callers(0x1b4c9a0) if e.function(s)[0] == 0x1bb3ad0]
    fact(len(rows) == 5 and len(exits) == 1, f'1bb3ad0: five rows by 1b4e2a0, Exit Game by 1b4c9a0 ({len(rows)}, {len(exits)})')
    fact(has(t, 'lea rcx, [rbp - 0x20c8]', 'mov qword ptr [rbp - 0x20c8], 0'), '1b4e2a0(builder, captions, functor, &flag 0)')
    leas = [rip(i) for i in e.disasm(0x1bb4e00) if i.mnemonic == 'lea' and 'rip' in i.op_str]
    fact(leas == [0x5343880, 0x1bdb960] and calls(e, 0x1bb4e00, 0x1bb4c70),
         '1bb4e00 (Controls): 1bb4c70(out, ctx, function{5343880, 1bdb960})')

    fact(e.read(e.ptr(0x5343880 + 16), 12) == bytes.fromhex('488b47084889f74889d6ffe0'),
         '5343880 invoke: jmp [this+8] with its two arguments')
    t = text(e, e.ptr(0x533d120 + 16))
    fact(has(t, 'mov rax, rsi', 'lea rsi, [rdx + 0x48]', 'call qword ptr [rax + 8]'),
         '533d120 invoke(out, this, ctx): [this+8](out, ctx + 0x48)')
    t = text(e, 0x1bb3ad0)
    fact(has(t, 'mov qword ptr [rbp - 0x1d00], rdi', 'lea rdi, [rbp - 0x1d20]'), 'a function keeps its own address at +0x20')

    for call, target, fn, pad in ((0x1bb4978, 0x1bea630, 0x1bb3ad0, 0x1bb4c61), (0x1b4adb3, 0x1bea820, 0x1b4a3d0, 0x1b49ac1)):
        ins = e.disasm(pad - 6, 6)
        fact(e.function(call)[0] == fn and e.disasm(call, 5)[0].op_str == hex(target), f'{call:x}: call {target:x} in {fn:x}')
        fact(ins[0].op_str == '0x2bbe4f8' and ins[1].mnemonic == 'nop' and
             e.read(pad, 15) == bytes.fromhex('6666666666662e0f1f840000000000') and e.is_function(pad + 15),
             f'{pad:x}: 15 bytes of padding after call 2bbe4f8; nop, before the function at {pad + 15:x}')
        into = [x for x in range(pad, pad + 15) if e.callers(x) or e.jumps_to(x)]
        fact(not into, f'{pad:x}: nothing calls or jumps into the padding')
        fact(abs(pad - (call + 5)) < 1 << 31, f'{call:x}: the padding is in rel32 reach')
    t = text(e, 0x1b4a3d0)
    fact(sum(1 for l in t if l == 'call 0x1b4c9a0') >= 4, '1b4a3d0 (the title list) adds its rows with 1b4c9a0')
    press_start = e.disasm(0x1b3855f, 7)[0]
    fact(e.cstr(rip(press_start)) == 'PressStart' and e.function(press_start.address)[0] == 0x1b384f0,
         '1b384f0 builds the Press Start widget separately from the interactive title list')
    fact(e.callers(0x1b384f0) == [0x1bda58d] and e.function(0x1bda58d)[0] == 0x1bda530,
         '1bda58d: the Press Start constructor is called from its own factory')

    t = text(e, 0x1ae8cc0)
    fact(has(t, 'mov qword ptr [r14], r15', 'mov qword ptr [r14 + 0x30], rbx', 'mov qword ptr [r14 + 0x28], 7',
             'mov qword ptr [r14 + 0x20], 0', 'mov byte ptr [r14 + 0x38], 1'),
         '1ae8cc0(out, group, id): +0 text, +8 wstring (+0x10 chars, +0x20 length, +0x28 capacity, +0x30 allocator)')
    t = text(e, 0x1ae7270)
    fact(has(t, 'mov qword ptr [r14], 0', 'lea rdi, [r14 + 8]'), '1ae7270 (a missing id): text 0, a formatted wstring')
    t = text(e, 0x2967040)
    fact(has(t, 'mov qword ptr [r15 + 0x28], rbx', 'mov qword ptr [r15 + 0x20], 7', 'mov qword ptr [r15 + 0x18], 0'),
         '2967040(string, text): a wstring made in place (+0x08, +0x18, +0x20, +0x28)')
    t = text(e, 0x1b211a0)
    fact(has(t, 'cmp qword ptr [rbx + 0x30], 8', 'mov rsi, qword ptr [rbx + 0x18]', 'mov rdi, qword ptr [rbx + 0x38]',
             'call qword ptr [rax + 0x70]'), "1b211a0 frees a list's entries: capacity >= 8, allocator vtable +0x70")

    t = text(e, 0x1b2ac00)
    fact(has(t, 'mov esi, 0xce0', 'mov dword ptr [rbp - 0x190], 0', 'mov dword ptr [rbp - 0x18c], 0xa',
             'mov dword ptr [rbp - 0x188], 1', 'add r15, 0xe50') and calls(e, 0x1b2ac00, 0x1b2bf60),
         '1b2ac00 slider: 0..10 step 1, a record pushed at dialog + 0xe50')
    t = text(e, 0x1b2a100)
    fact(has(t, 'mov esi, 0x410', 'mov qword ptr [rbx + 0x360], rax', 'mov byte ptr [rbx + 0x368], al', 'lea rax, [rbx + 0xa0]',
             'add rdi, 0xe50') and calls(e, 0x1b2a100, 0x1ace690),
         '1b2a100 choice: 0x410 bytes, value pointer +0x360, default +0x368, its list +0xa0')
    t = text(e, 0x1b29370)
    fact(has(t, 'mov esi, 0xcf0', 'lea r12, [rbx + 0x100]', 'mov dword ptr [rbx + 0x3d4], eax', 'qword ptr [r15 + 0x908]',
             'add r12, 0xe50'), '1b29370 pick: 0xcf0 bytes, choices list +0x100, default +0x3d4, count +0x908')
    t = text(e, 0x1b2b3b0)
    fact(has(t, 'mov qword ptr [rbx + 0x98], 0', 'mov edx, 0x9d6c', 'mov edx, 0x9d6d') and calls(e, 0x1b2b3b0, 0x1b2c200),
         '1b2b3b0: the On/Off list, messages 0x9d6c and 0x9d6d')
    t = text(e, 0x1b2c200)
    fact(has(t, 'qword ptr [r15 + 0x98]', 'cmp rcx, 3', 'lea r15, [rax + rax*8]', 'mov byte ptr [rbx + r15*8 + 0x40], al'),
         '1b2c200: a choice list of two 0x48-byte entries, count +0x98')
    t = text(e, 0x1b1c0c0)
    fact(has(t, 'qword ptr [r15 + 0x908]', 'cmp rcx, 0x21', 'mov dword ptr [rbx + r15*8], eax'),
         '1b1c0c0: a pick list of 32 entries (int32 values), count +0x908')
    t = text(e, 0x1b2bf60)
    fact(has(t, 'mov rbx, qword ptr [r14 + 0x10]', 'movabs rax, 0x8e38e38e38e38e39', 'mov rax, qword ptr [r15 + 0x80]',
             'lock inc dword ptr [rax + 8]', 'mov rax, qword ptr [r15 + 0x40]'),
         '1b2bf60: 0x90-byte records (+0 caption, +0x40 help, +0x80 the counted widget), end at vector + 0x10')

    ins = e.disasm(0x1ace690, 0x60)
    store = [i for i in ins if i.mnemonic == 'lea' and 'rip' in i.op_str]
    fact(len(store) >= 2 and rip(store[1]) + 0x10 == 0x5399fb0, '1ace690: a list component, vtable pointer 5399fb0')
    t = text(e, 0x1b28a50)
    fact(has(t, 'lea rdi, [rbx + 0xa70]') and calls(e, 0x1b28a50, 0x1ace690), "1b28a50: the dialog's rows list at +0xa70")
    t = text(e, 0x1b28de0)
    fact(has(t, 'lea r12, [r15 + 0xa70]', 'mov rax, qword ptr [r15 + 0xe60]', 'sub rax, qword ptr [r15 + 0xe58]') and
         calls(e, 0x1b28de0, 0x1ad06e0), '1b28de0: the rows list from the records (+0xe58..+0xe60), then 1ad06e0')
    t = text(e, 0x1b1a2d0)
    fact(has(t, 'mov rsi, qword ptr [r12 + 0xe58]', 'call qword ptr [rax + 0x20]'),
         "1b1a2d0 (Defaults): each record's widget, vtable +0x20")

    t = text(e, 0x1c0c9f0)
    fact(has(t, 'mov esi, 0xcc', 'mov edx, 2', 'lock xadd dword ptr [rbx + 8], eax', 'call qword ptr [rax]') and
         [x for _, x in e.calls_in(0x1c0c9f0)][:4] == [0x1ae8cc0, 0x1c1ed10, 0x195db60, 0x1c1a070],
         '1c0c9f0: question (group 204), 1c1ed10 box, 195db60 command (2), 1c1a070; counted at +8, slot 0 deletes')
    fact(e.disasm(0x1bb436d, 5)[0].op_str == '0x1b4c9a0', '1bb436d: Exit Game added by 1b4c9a0(builder, captions, functor)')

    print(f'\n{failed} of the facts above do not hold' if failed else '\nall facts hold')
    return 1 if failed else 0

if __name__ == '__main__':
    sys.exit(main())
