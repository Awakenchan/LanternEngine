#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import sys

from capstone.x86 import X86_OP_MEM, X86_REG_RIP

from eboot import Eboot

HERE = os.path.dirname(os.path.abspath(__file__))

CONTEXT = {
    'Dispatch': (0x21695c0, 'dcb.dispatch'),
    'DispatchIndirect': (0x2169640, 'dcb.dispatchIndirect'),
    'Draw': (0x21696c0, 'dcb.drawIndexAuto'),
    'DrawIndexed': (0x2169780, 'dcb.drawIndexOffset'),
    'DrawInstanced': (0x2169840, 'dcb.drawIndexAuto'),
    'DrawIndexedInstanced': (0x2169900, 'dcb.drawIndexOffset'),
    'DrawInstancedIndirect': (0x21699d0, 'dcb.drawIndirect'),
    'DrawIndexedInstancedIndirect': (0x2169ab0, 'dcb.drawIndexIndirect'),
    'Commit': (0x2169ba0, None),
}
FLUSH_OUTPUT_MERGER = 0x26d2230
POST_DRAW = 0x26cbd50
COMMAND_FLUSH = 0x26af700
FINISH_COMMAND_LIST = (0x26d5850, 0x216aca0)
EXECUTE_COMMAND_LIST = (0x21679a0, 0x216a7d0)
EXECUTE_COMMAND_LISTS = (0x21679e0, 0x216a980)
SUBMIT_SEGMENT = 0x26b0b00
FLIP_CHAIN = (0x26afb30, 0x26b0ce0, 0x26b0e30)
PRESENT = (0x26d5dd0, 0x26d6e50, 0x33d0)
CREATE_BUFFER = (0x2165c10, 0x58)
UPLOAD_POOL = (0x26a9b50, 0x216b0f0, 0x30, 0xff0)
UPLOAD_POOL_ENTRY = '554889e5415741564155415453 50'.replace(' ', '')
UPLOAD_POOL_ARENA = (0xb8, 0xc0)
RING_ALLOCATOR = (0x26addc0, 0x26a9d50, 0x10000)
OCCLUSION_QUERY = 0x1077660
PIPELINE_DRAIN = 0x26b09e0
TABLE_RECLAIM = 0x26aa860
FLUSH_WAIT = 0x11d7030
CE_WRITERS = (0x11fd190, 0x11fd3e0)
YEBIS_CONTEXT_VTABLE = (0x530a380, 133, {50: 0x2a41320})
YEBIS_GNM = {'draw index': 0x11fbc40, 'draw auto': 0x11fbb50, 'dispatch': 0x11fbd10}
PPFX = {'bind framebuffer': 0xbb68f0, 'apply effects': 0xbb6980}
FLIP_PASS = 0x21d5f80
CUE = (0x1092e80, 0x1093150)
FRAME_TIMER = 0x2034770
REPLAY_TABLE = 0x52b9fb8
STAGE_SLOTS = (('ConstantBuffers', 16), ('ShaderResources', 128), ('Samplers', 14), ('Shader', 1))
STAGES = (0x598, 0xa90, 0xf88, 0x1480, 0x1978, 0x1e70)
STAGE_SETTERS = (
    (0x2168ce0, 0x2168d00, 0x2168d90, 0x2168e00),
    (0x2168e10, 0x2168e30, 0x2168ec0, 0x2168f30),
    (0x2168f40, 0x2168f60, 0x2168ff0, 0x2169060),
    (0x2169070, 0x2169090, 0x2169120, 0x2169190),
    (0x21692c0, 0x21692e0, 0x2169370, 0x21693e0),
    (0x2169440, 0x2169460, 0x21694f0, 0x21695b0),
)
INPUT_ASSEMBLER = (
    ('IASetInputLayout', 0x2168c90, 0x0),
    ('IASetPrimitiveTopology', 0x2168ca0, 0x8),
    ('IASetIndexBuffer', 0x2168cb0, 0x10),
    ('IASetVertexBuffers', 0x2168cc0, 0x20),
)
OUTPUT_MERGER_STATE = {'blend': 0x4d0, 'depth stencil': 0x4d8, 'render targets': 0x4e0,
                       'depth view': 0x520, 'blend factor': 0x568, 'stencil ref': 0x584}
CREATORS = {
    'CreateTexture2D': (0x2165f90, None),
    'CreateTexture3D': (0x2166300, None),
    'shader 0x128': (0x2166d00, 0x128),
    'shader 0x120 a': (0x2166e10, 0x120),
    'shader 0x120 b': (0x2166f20, 0x120),
    'CreateGeometryShader': (0x2167030, 0x150),
    'CreateGeometryShaderWithStreamOutput': (0x2167140, 0x150),
    'shader 0x120 c': (0x21672d0, 0x120),
    'shader 0x120 d': (0x21673e0, 0x120),
    'CreateDeferredContext': (0x21676c0, 0xdae0),
    'CreateBlendState': (0x21658e0, 0x128),
    'CreateDepthStencilState': (0x21659a0, 0x38),
    'CreateRasterizerState': (0x2165a80, 0x24),
    'CreateSamplerState': (0x2165b40, 0x40),
}
EMPTY_SETTERS = {
    'RSSetState': 0x2169290,
    'OMSetBlendState': 0x21693f0,
    'OMSetDepthStencilState': 0x2169400,
}
COUNT_SETTERS = {
    'RSSetViewports': (0x21692a0, 0x26d3350),
    'RSSetScissorRects': (0x21692b0, 0x26d3360),
}
SWAP_CHAIN = {'wrapper': 0x26d5a80, 'creator': 0x26d5e90, 'destroyer': 0x26d6a50, 'present': 0x26d6e50}
SWAP_CHAIN_ENTRY = '554889e54157415641554154534881ec98010000'
SWAP_CHAIN_LAYOUT = {'target stride': 0x2c, 'description': 0x300, 'buffer count': 0x328}
SWAP_CHAIN_HOLDER = 0x348
UPLOAD_RESOURCE = 0x26b35e0
UPDATE_SUBRESOURCE = 0x2168a20
CONSTANT_UPDATES = {'dynamic': (0x26adb90, '554889e5415741564155415453 4883ec28'.replace(' ', '')),
                    'default': (0x26b3560, '554889e5415741564155415453 4883ec18'.replace(' ', ''))}
GAMMA_RAMP = (0x26d6b20, 0x380, 0x401)
TEXTURE_HOLDERS = {'CreateTexture2D': (0x2171d60, 3), 'CreateTexture3D': (0x21723a0, 4)}
RECORD_DRAW_ENTRIES = {0x21696c0: '554889e5415741564155415453 50', 0x2169780: '554889e5415741564155415453 50',
                       0x2169840: '554889e5415741564155415453 50', 0x2169900: '554889e54157415641554154534883ec18'}
PENDING_ON_CONTEXT = 0xb730
DRAW_WRAPPER = 0x21a23d0
VIEWS = {
    'CreateDepthStencilView': (0x2166610, 0x40, 0x60, 0x535f420),
    'CreateRenderTargetView': (0x21667a0, 0x20, 0x50, 0x535f440),
    'CreateShaderResourceView': (0x21669a0, 0x8, 0x50, 0x535f460),
    'CreateUnorderedAccessView': (0x2166b60, 0x80, 0x50, 0x535f480),
}
SHADER_MAGIC = (0x72646853, 0x24)
GEOMETRY_SHADER_CHECK = (0x2173220, 0x2c, 3)


def gnm_names():
    names = {}
    for line in open(os.path.join(HERE, 'gnm_names.txt')):
        part = line.split()
        if len(part) >= 2 and part[0].startswith('0x'):
            names[int(part[0], 16)] = part[1]
    return names


def immediates(e, f):
    out = set()
    for i in e.disasm(f):
        for tok in i.op_str.replace(',', ' ').replace('[', ' ').replace(']', ' ').split():
            if tok.startswith('0x'):
                try:
                    out.add(int(tok, 16))
                except ValueError:
                    pass
            elif tok.isdigit():
                out.add(int(tok))
    return out


def rip_targets(e, f):
    out = set()
    for i in e.disasm(f):
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                out.add(i.address + i.size + op.mem.disp)
    return out


BIND = 0x26bc320
BIND_CALLS = {'the vertex buffer flush': 0x26cfcf0, 'the ring allocator': 0x26addc0, 'the rename lookup': 0x26ae840,
              'setPrimitiveSetup': 0x1075960, 'setTessellation': 0x10756b0, 'the output flush': 0x26d2790,
              'the shader table': 0x26d04e0, 'setShaderStages': 0x1076030, 'the fetch pointer': 0x1076390,
              'setVsShader': 0x1075480, 'setPsShader': 0x10753c0, 'the prefetch': 0x1073f50,
              'the pixel semantics': 0x10873a0, 'setPsInputs': 0x1075000}
RENDER_WRITERS = {0x1072fd0: (0x8e, None), 0x1073090: (0x1e0, 0xffff1fff), 0x1073100: (0x105, None),
                  0x10733a0: (0x200, 0xc07007ff), 0x1073400: (0x10b, 0xffffff), 0x1073190: (0x10c, None),
                  0x1076b30: (0x204, 0xf7fe03f), 0x1076e00: (0x205, 0x393fff), 0x10765e0: (0x10f, None),
                  0x10766d0: (0x94, 0x7fff)}
RENDER_FLUSHES = {FLUSH_OUTPUT_MERGER: ((0x1072fd0, 0x1073090, 0x1073100, 0x10733a0, 0x1073400, 0x1073190),
                                       (0x4d0, 0x568, 0x4d8, 0x584), (0x546c8f4, 0x546ca1c)),
                  0x26d3370: ((0x1076b30, 0x1076e00, 0x10765e0, 0x10766d0), (0x240, 0x248, 0x3c8), (0x546ca58,))}
TARGET_WRITERS = {0x10738b0: ((0x318, 0x31c, 0xf), (0x1086ca0,)), 0x10739b0: ((0x10, 0x11, 0x2, 0x5, 0x2af, 0xf), (0x1072770, 0x1072790))}
TARGET_FLUSH = (0x26d2790, (0x4e0, 0x520))
VERTEX_FLUSH = (0x26cfcf0, (0xa260, 0x28cc, 0x28cd, 0x20, 0x120, 0x1a0))
USER_DATA_BASES = 0x48d15c0
USER_DATA_VALUES = (0x2e40, 0x2c0c, 0x2c4c, 0x2c8c, 0x2ccc, 0x2d0c, 0x2d4c)


def main(path):
    e = Eboot(path)
    names = gnm_names()
    fails = []

    def check(ok, what):
        print(f"{'OK  ' if ok else 'FAIL'}  {what}")
        if not ok:
            fails.append(what)

    def calls(f):
        return [t for _, t in e.calls_in(f)]

    for name, (f, writer) in CONTEXT.items():
        check(e.is_function(f), f'context {name} at 0x{f:x}')
        targets = calls(f)
        if writer:
            check(any(names.get(t) == writer for t in targets), f'  {name} writes {writer}')
        check(COMMAND_FLUSH in targets, f'  {name} ends with the command flush')
    for name in ('Draw', 'DrawIndexed', 'DrawInstanced', 'DrawIndexedInstanced', 'Commit'):
        check(FLUSH_OUTPUT_MERGER in calls(CONTEXT[name][0]), f'  {name} flushes the output merger')
    for name in ('DrawInstancedIndirect', 'DrawIndexedInstancedIndirect', 'Commit'):
        check(POST_DRAW in calls(CONTEXT[name][0]), f'  {name} runs the post-draw')

    check(FINISH_COMMAND_LIST[1] in calls(FINISH_COMMAND_LIST[0]), 'FinishCommandList calls its body')
    check(EXECUTE_COMMAND_LIST[1] in calls(EXECUTE_COMMAND_LIST[0]), 'ExecuteCommandList calls its body')
    check(EXECUTE_COMMAND_LISTS[1] in calls(EXECUTE_COMMAND_LISTS[0]), 'ExecuteCommandLists calls its body')
    check(e.is_function(SUBMIT_SEGMENT), f'segment submit at 0x{SUBMIT_SEGMENT:x}')
    check(FLIP_CHAIN[1] in calls(FLIP_CHAIN[0]) and FLIP_CHAIN[2] in calls(FLIP_CHAIN[1]), 'flip: close, submit and flip chained')
    present = e.disasm(PRESENT[0])
    check(any(i.mnemonic == 'jmp' and i.op_str == hex(PRESENT[1]) for i in present) and
          any(hex(PRESENT[2]) in i.op_str for i in present), 'Present tail-jumps to the swap chain at context+0x33d0')
    check(CREATE_BUFFER[1] in immediates(e, CREATE_BUFFER[0]), 'CreateBuffer makes a 0x58-byte holder')
    pool, device, node, block = UPLOAD_POOL
    imm = immediates(e, pool)
    check(node in imm and block in imm, 'upload pool: 0x30-byte nodes in 0xff0-byte blocks')
    check(any(e.function(c)[0] == device for c in e.callers(pool)), f'upload pool made by the device at 0x{device:x}')
    check(e.read(pool, len(UPLOAD_POOL_ENTRY) // 2).hex() == UPLOAD_POOL_ENTRY, 'upload pool has the entry the hook expects')
    check(all(a in imm for a in UPLOAD_POOL_ARENA), '  keeps its arena, the base and size it is given, at +0xb8 and +0xc0')
    check(len(e.callers(pool)) == 1, '  made once')
    check(RING_ALLOCATOR[1] in calls(RING_ALLOCATOR[0]) and RING_ALLOCATOR[2] in immediates(e, RING_ALLOCATOR[0]),
          f'ring allocator 0x{RING_ALLOCATOR[0]:x} takes 64 KiB blocks from the pool 0x{RING_ALLOCATOR[1]:x}')
    for name, f in (('occlusion query writer', OCCLUSION_QUERY), ('pipeline drain', PIPELINE_DRAIN),
                    ('table reclaim', TABLE_RECLAIM), ('flush wait', FLUSH_WAIT), ('flip pass', FLIP_PASS),
                    ('frame timer', FRAME_TIMER), ('CUE pre-draw', CUE[0]), ('CUE post-draw', CUE[1]),
                    ('CE writers start', CE_WRITERS[0]), ('CE writers end', CE_WRITERS[1])):
        check(e.is_function(f), f'{name} at 0x{f:x}')
    table, count, slots = YEBIS_CONTEXT_VTABLE
    entries = [e.ptr(table + 8 * i) for i in range(count)]
    check(all(e.is_function(x) for x in entries), f'YEBIS context vtable 0x{table:x}: {count} methods')
    for slot, f in slots.items():
        check(entries[slot] == f, f'  slot {slot} (ClearRenderTargetView) is 0x{f:x}')
    for name, f in {**YEBIS_GNM, **PPFX}.items():
        check(e.is_function(f), f'{name} at 0x{f:x}')

    replay = [e.ptr(REPLAY_TABLE + 8 * i) for i in range(36)]
    check(all(e.is_function(x) for x in replay), f'GX command-list replay table 0x{REPLAY_TABLE:x}: 36 handlers')

    def handler_calls(h, f):
        return f in calls(h) or any(i.mnemonic == 'jmp' and i.op_str == hex(f) for i in e.disasm(h))

    for k, (name, f, offset) in enumerate(INPUT_ASSEMBLER):
        check(handler_calls(replay[k], f), f'  replay {k} calls {name} 0x{f:x}')
        if offset:
            check(offset in immediates(e, replay[k]), f'    and keeps it at state+0x{offset:x}')
    for s, (base, setters) in enumerate(zip(STAGES, STAGE_SETTERS)):
        offset = base
        for j, ((kind, slots), f) in enumerate(zip(STAGE_SLOTS, setters)):
            k = 4 + 4 * s + j
            check(handler_calls(replay[k], f), f'  replay {k} calls stage {s} Set{kind} 0x{f:x}')
            check(offset in immediates(e, replay[k]), f'    stage {s} {kind} at state+0x{offset:x}')
            offset += 8 * slots
    for k, name in zip(range(28, 36), ('Dispatch', 'Draw', 'DrawIndexed', 'DrawInstanced', 'DrawIndexedInstanced',
                                       'DrawInstancedIndirect', 'DrawIndexedInstancedIndirect', 'Commit')):
        check(handler_calls(replay[k], CONTEXT[name][0]), f'  replay {k} calls {name}')
    om = immediates(e, FLUSH_OUTPUT_MERGER)
    for name, offset in OUTPUT_MERGER_STATE.items():
        check(offset in om, f'output-merger flush reads {name} at state+0x{offset:x}')
    for name, (f, size) in CREATORS.items():
        check(e.is_function(f), f'{name} at 0x{f:x}')
        if size:
            check(size in immediates(e, f), f'  allocates 0x{size:x} bytes')
    for name, (f, bind, size, vtable) in VIEWS.items():
        imm = immediates(e, f)
        check(e.is_function(f) and bind in imm, f'{name} at 0x{f:x} needs bind flag 0x{bind:x}')
        check(size in imm, f'  allocates 0x{size:x} bytes')
        check(vtable in rip_targets(e, f), f'  vtable 0x{vtable:x}')
    magic, at = SHADER_MAGIC
    f, type_at, gs = GEOMETRY_SHADER_CHECK
    imm = immediates(e, f)
    check(magic in imm and at in imm and type_at in imm and gs in imm,
          f'geometry shader check 0x{f:x}: Shdr magic at +0x{at:x}, type {gs} at +0x{type_at:x}')
    check(f in calls(CREATORS['CreateGeometryShaderWithStreamOutput'][0]) or
          any(f in calls(c) for c in calls(CREATORS['CreateGeometryShaderWithStreamOutput'][0])),
          '  stream-output creator goes through it')

    for name, f in EMPTY_SETTERS.items():
        check(e.u8(f) == 0xc3, f'{name} at 0x{f:x} only returns: the caller writes the pending state')
    for name, (f, inner) in COUNT_SETTERS.items():
        check(any(f <= j < f + 0x10 for j in e.jumps_to(inner)), f'{name} at 0x{f:x} passes its count to 0x{inner:x}')
    check(PENDING_ON_CONTEXT in immediates(e, DRAW_WRAPPER), f'0x{DRAW_WRAPPER:x} passes context+0x{PENDING_ON_CONTEXT:x} as the pending state')

    bind = calls(BIND)
    for name, f in BIND_CALLS.items():
        check(f in bind, f'bind 0x{BIND:x} calls {name} 0x{f:x}')
    check(all(e.u8(USER_DATA_BASES + 2 * i) == (v & 0xff) for i, v in enumerate(USER_DATA_VALUES)),
          f'user-data registers by stage at 0x{USER_DATA_BASES:x}')

    for f, entry in RECORD_DRAW_ENTRIES.items():
        entry = entry.replace(' ', '')
        check(e.read(f, len(entry) // 2).hex() == entry, f'draw 0x{f:x} has the entry the record hook expects')
    for writer, (register, mask) in RENDER_WRITERS.items():
        written = immediates(e, writer)
        check(register in written and (mask is None or mask in written),
              f'{names.get(writer, hex(writer))} 0x{writer:x} writes context register 0x{register:x}' + (f' masked by 0x{mask:x}' if mask else ''))
    for flush, (writers, offsets, defaults) in RENDER_FLUSHES.items():
        called, read = calls(flush), immediates(e, flush)
        check(all(w in called for w in writers), f'flush 0x{flush:x} writes its state through ' + ', '.join(hex(w) for w in writers))
        check(all(o in read for o in offsets), '  from pending state ' + ', '.join(f'+0x{o:x}' for o in offsets))
        check(all(d in rip_targets(e, flush) for d in defaults), '  with the default state at ' + ', '.join(hex(d) for d in defaults))
    for writer, (registers, checks) in TARGET_WRITERS.items():
        check(all(r in immediates(e, writer) for r in registers) and all(c in calls(writer) for c in checks),
              f'{names.get(writer, hex(writer))} writes ' + ', '.join(hex(r) for r in registers) + ' after ' + ', '.join(hex(c) for c in checks))
    flush, offsets = TARGET_FLUSH
    check(all(w in calls(flush) for w in TARGET_WRITERS) and all(o in immediates(e, flush) for o in offsets),
          f'output flush 0x{flush:x} sets the targets at +0x4e0 and the depth view at +0x520 through both writers')
    flush, offsets = VERTEX_FLUSH
    check(all(o in immediates(e, flush) for o in offsets), f'vertex flush 0x{flush:x} builds the V# table at +0xa260 from the vertex buffers, strides and offsets, copying the entries at +0x28cc and +0x28cd')
    creator = SWAP_CHAIN['creator']
    check(e.read(creator, len(SWAP_CHAIN_ENTRY) // 2).hex() == SWAP_CHAIN_ENTRY, f'swap chain creator 0x{creator:x} has the entry the hook expects')
    check([e.function(c)[0] for c in e.callers(creator)] == [SWAP_CHAIN['wrapper']], f'  called only by 0x{SWAP_CHAIN["wrapper"]:x}')
    created = immediates(e, creator)
    check(SWAP_CHAIN_LAYOUT['target stride'] in created, '  one render target every 0x2c bytes at the start of the chain')
    check(SWAP_CHAIN_LAYOUT['description'] in created, '  keeps its description at +0x300')
    check(0x57 in created, '  takes only format 0x57')
    check(CREATORS['CreateTexture2D'][0] in calls(creator), '  also makes the texture the game draws into')
    presented = immediates(e, SWAP_CHAIN['present'])
    check(SWAP_CHAIN_LAYOUT['buffer count'] in presented, f'present 0x{SWAP_CHAIN["present"]:x} reads the buffer count at +0x328')
    check(SWAP_CHAIN_HOLDER in presented and SWAP_CHAIN_LAYOUT['target stride'] in presented,
          '  and points the holder at +0x348 at each buffer through its target')
    for kind, (update, entry) in CONSTANT_UPDATES.items():
        check(e.read(update, len(entry) // 2).hex() == entry, f'{kind} buffer update 0x{update:x} has the entry the hook expects')
        check({e.function(c)[0] for c in e.callers(update)} == {UPDATE_SUBRESOURCE}, f'  called only by UpdateSubresource 0x{UPDATE_SUBRESOURCE:x}')
    dynamic = CONSTANT_UPDATES['dynamic'][0]
    check(RING_ALLOCATOR[0] in calls(dynamic) and 0x26ae700 in calls(dynamic), f'  the dynamic update takes new ring memory (0x{RING_ALLOCATOR[0]:x}) and a rename entry (0x26ae700)')
    check(0x26b1c40 in calls(CONSTANT_UPDATES['default'][0]), '  the default update writes through the command stream (0x26b1c40)')
    check(not any(e.function(c)[0] == UPDATE_SUBRESOURCE for c in e.callers(RING_ALLOCATOR[0])), '  UpdateSubresource takes ring memory only through the dynamic update')
    uploads = {e.function(c)[0] for c in e.callers(UPLOAD_RESOURCE)}
    check(uploads == {UPDATE_SUBRESOURCE, GAMMA_RAMP[0]}, f'0x{UPLOAD_RESOURCE:x} is an upload: called by UpdateSubresource 0x{UPDATE_SUBRESOURCE:x} and the gamma ramp 0x{GAMMA_RAMP[0]:x} only')
    check(calls(GAMMA_RAMP[0]).count(UPLOAD_RESOURCE) == 3 and GAMMA_RAMP[1] in immediates(e, GAMMA_RAMP[0]) and GAMMA_RAMP[2] in immediates(e, GAMMA_RAMP[0]),
          '  the gamma ramp uploads three channels of 0x401 values into the texture at +0x380, not a display buffer')
    for name, (holder, dimension) in TEXTURE_HOLDERS.items():
        check(holder in calls(CREATORS[name][0]), f'{name} fills its holder with 0x{holder:x}')
        check(dimension in immediates(e, holder), f'  dimension {dimension}')

    print(f"\n{len(fails)} failed")
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'E:/BloodborneDev/engine/out/eboot.elf'))
