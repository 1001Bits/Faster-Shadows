"""Check shipped profiles and compiled ABI against the original game PEs."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

import capstone
import pefile
from inspect_engine import Engine, GAMES

ROOT = Path(__file__).resolve().parents[1]
# BSShadowDirectionalLight::AccumulateFromLists (Ghidra Combined, 2026-10-11): registers each
# cascade's casters into descriptor+0x48. OG by PDB name; AE 1.11.240 by its map name.
ACCUMULATE_FROM_LISTS = {'og': 0x28CA7D0, 'ae': 0x2250400, 'ng': 0x20F32F0}
# Cascade cull skipping (src/ShadowHooks.cpp cullskip): E8 call sites inside AccumulateFromLists.
# BSShaderAccumulator::ClearDebugStats, BSCullingGroup::Process and ::AccumulatePasses (Ghidra Combined).
CASCADE_CULL_SITES = {'og': [(0x28CA9B6, 0x282E790), (0x28CAA5D, 0x1CCCE70), (0x28CAA73, 0x1CCD520), (0x28CAC73, 0x1CCD520)],
                      'ae': [(0x2250696, 0x21D64B0), (0x225073E, 0x17E0270), (0x2250754, 0x17E08C0), (0x22509B1, 0x17E08C0)],
                      # Next-Gen 1.10.984 has AE's code at its own addresses (Ghidra Combined, 2026-10-23).
                      'ng': [(0x20F3586, 0x2079310), (0x20F362E, 0x16C5010), (0x20F3644, 0x16C5660), (0x20F38A1, 0x16C5660)]}
# Sun cull worker (src/ShadowHooks.cpp sunworker): the two CullingJob::QBatchEnabled queries of the
# batched sun fit/gather (OG AccumulateSunShadowLight, AE inlined in MainRenderSetup), the one in
# DeferredLightsImpl, and BSCullingGroup::AccumulatePasses' call to BSMTAManager::BeginAccumulation.
# The last three are engine calls that prove the functions the worker calls directly:
# BSPreCulledObjects::QEnabled, BSGraphics::Renderer::SetContext, BSSystemFileStreamer::SetThreadEventFlag.
SUN_WORKER_SITES = {'og': [(0x2850384, 0x1CCB850), (0x28503B1, 0x1CCB850), (0x28537D6, 0x1CCB850), (0x1CCD54C, 0x28608F0),
                           (0x28CA88D, 0x2809E30), (0xCC0751, 0x1D14CB0), (0x1B53F31, 0x1B48680)],
                    'ae': [(0x21F3218, 0x17DE920), (0x21F3245, 0x17DE920), (0x21F5CE5, 0x17DE920), (0x17E08EA, 0x2202500),
                           (0x22504B9, 0x21AE520), (0xBD7461, 0x1821EB0), (0x1687D11, 0x1681A90)],
                    'ng': [(0x2096078, 0x16C36C0), (0x20960A5, 0x16C36C0), (0x2098B45, 0x16C36C0), (0x16C568A, 0x20A5360),
                           (0x20F33A9, 0x2051380), (0xB57B11, 0x1706BE0), (0x156D5E1, 0x1567360)]}
# The accumulate query is followed by bShadows, St and kCullingGroup operands (src/SunWorkerSite.h).
SUN_ACCUMULATE_PATTERN = [(0x00, b'\xE8'), (0x05, b'\x84\xC0\x74'), (0x09, b'\x80\x3D'), (0x0F, b'\x00'), (0x10, b'\x74'),
                          (0x12, b'\x48\x8B\x0D'), (0x19, b'\x48\x8D\x15'), (0x20, b'\x48\x8B\x89\x08\x02\x00\x00')]
# Sun-glare poll (sunglare): BSShaderAccumulator::EvaluateSunOcclusionResults' GetOcclusionQueryResults call.
SUN_GLARE_SITES = {'og': (0x282F27A, 0x1D11CD0), 'ae': (0x21D70CA, 0x181EE70), 'ng': (0x2079F2A, 0x1703BA0)}
# Next to CBRO (OG only, src/ShadowHooks.cpp cbropublish and sunworker::ChooseCull): AccumulateFromLists' first call,
# BSGeometryListCullingProcess::ClearCollectedGeometry, after `mov rdi, rcx; mov rcx, [rcx+0x2C8]; mov r14, rdx` (the
# light stays in rdi), and DeferredLightsImpl's own cascade cull call, which CBRO wraps. DrawWorld::pVisCamera is
# the camera the sun's fit receives (AccumulateSunShadowLight passes it).
CBRO_SITES = {'og': {'publish': (0x28CA800, 0x2977E80), 'prologue': (0x28CA7F3, '488bf9488b89c80200004c8bf2'),
                     'inline_cull': (0x28537F9, 0x28CA7D0), 'vis_camera': 0x6723240, 'fit_caller': 0x2850380}}
# Shadow slice guard (slotguard, every runtime): RenderShadowMap's SetCurrentDepthStencilTarget call,
# which every shadow map's slice bind passes, and the `xor r8d, r8d` (clear mode 0) before it.
# Static shadow cache (shadowcache): BSBatchRenderer::Draw from the three RenderPassImmediately
# leaves (Ghidra Combined, 2026-10-23).
SLICE_GUARD_SITES = {'og': (0x28C98CA, 0x1D31940, 0x28C98BF), 'ae': (0x224F5E2, 0x1839C80, 0x224F5D4),
                     'ng': (0x20F24D2, 0x171E960, 0x20F24C4)}
SHADOW_CACHE_SITES = {'og': [(0x287FCE5, 0x287EDE0), (0x287FD86, 0x287EDE0), (0x287FDF7, 0x287EDE0)],
                      'ae': [(0x22200B5, 0x221F080), (0x2220156, 0x221F080), (0x22201C7, 0x221F080)],
                      'ng': [(0x20C2F15, 0x20C1EE0), (0x20C2FB6, 0x20C1EE0), (0x20C3027, 0x20C1EE0)]}


def function_body(engine, rva):
    # Reading raw runtime-function records avoids pefile recursively decoding
    # tens of thousands of unrelated unwind records.
    pdata = next(s for s in engine.pe.sections if s.Name.rstrip(b'\0')==b'.pdata')
    data = pdata.get_data()[:pdata.Misc_VirtualSize]
    records = list(struct.iter_unpack('<III',data))

    def root(record):
        begin, _, unwind = record
        seen = set()
        while unwind not in seen:
            seen.add(unwind)
            header = engine.read(unwind,4)
            if not (header[0] >> 3) & 4:  # UNW_FLAG_CHAININFO
                return begin
            offset = (4+header[2]*2+3) & ~3
            begin, _, unwind = struct.unpack('<III',engine.read(unwind+offset,12))
        raise AssertionError('cyclic unwind chain')

    # MSVC splits these functions into multiple chained unwind regions. A
    # first-region-only check would miss most of the native camera updater.
    regions = [(begin,end) for begin,end,unwind in records
               if rva <= begin < rva+0x10000 and root((begin,end,unwind))==rva]
    if not regions:
        # Small leaf functions need no unwind record. Accept only a bounded,
        # return-bearing body terminated by native INT3 alignment padding.
        leaf = []
        for instruction in engine.instructions(rva,256):
            if instruction.mnemonic == 'int3':
                assert any(i.mnemonic=='ret' for i in leaf), ('no leaf return',hex(rva))
                return leaf
            leaf.append(instruction)
        raise AssertionError(('no bounded native function',hex(rva)))
    return [i for begin,end in regions for i in engine.instructions(begin,end-begin)]


def rip_target(engine, instruction, operand):
    return instruction.address+instruction.size+operand.mem.disp-engine.base


def table_writes(engine, handler, table):
    """Slots of pRegisterObjectFunc that native code fills with `handler` (lea reg,[handler]; mov [slot],reg)."""
    code, start, slots = engine.code, engine.text.VirtualAddress, set()
    for match in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
        offset = match.start()
        if offset+7 > len(code) or start+offset+7+struct.unpack_from('<i', code, offset+3)[0] != handler:
            continue
        instructions = engine.instructions(start+offset, 0xC0)
        if not instructions or instructions[0].mnemonic != 'lea':
            continue
        register = instructions[0].operands[0].reg
        for i in instructions[1:]:
            if (i.mnemonic == 'mov' and len(i.operands) == 2 and i.operands[0].type == capstone.CS_OP_MEM and
                    i.operands[0].mem.base == capstone.x86.X86_REG_RIP and
                    i.operands[1].type == capstone.CS_OP_REG and i.operands[1].reg == register):
                target = rip_target(engine, i, i.operands[0])
                if table <= target < table+0x200 and (target-table) % 8 == 0:
                    slots.add((target-table)//8)
            elif i.operands and i.operands[0].type == capstone.CS_OP_REG and i.operands[0].reg == register:
                break  # the handler register is reused
    return slots


def verify_registration(engine, profile):
    """RegisterObject dispatches through the profiled table, and the renderer fills the replaced slots."""
    entry = profile['register_object']
    assert engine.mapping[entry['id']] == entry['table']
    target = struct.unpack('<Q', engine.read(entry['table']+entry['slot']*8, 8))[0]-engine.base
    assert target == entry['target'], 'BSShaderAccumulator::RegisterObject slot'
    body = engine.instructions(target, 0x100)
    table = profile['register_table']
    assert any(i.mnemonic == 'lea' and i.operands[1].type == capstone.CS_OP_MEM and
               i.operands[1].mem.base == capstone.x86.X86_REG_RIP and rip_target(engine, i, i.operands[1]) == table
               for i in body), 'RegisterObject does not load the profiled handler table'
    assert any(i.mnemonic == 'movsxd' and '0x560]' in i.op_str for i in body), 'accumulator render mode'
    assert any(i.mnemonic == 'jmp' and '*8]' in i.op_str for i in body), 'indexed handler dispatch'
    writes = {}
    for name, required in (('shadow', {15, 16}), ('occlusion', {14})):
        handler = profile['handlers'][name]
        assert engine.read(handler['target'], 32).hex() == handler['prefix'], (name, 'handler prefix')
        slots = table_writes(engine, handler['target'], table)
        assert required <= slots, (name, sorted(slots))
        writes[name] = sorted(slots)
    return writes


def verify_lamp_layouts(engine):
    """Parabolic lamp sphere (+0x1C0/+0x1CC) and spot reach/render mode used by the filters."""
    def calls(body):
        return {i.operands[0].imm-engine.base for i in body
                if i.mnemonic == 'call' and i.operands and i.operands[0].type == capstone.CS_OP_IMM}
    process = struct.unpack('<Q', engine.read(engine.mapping[845854]+0x19*8, 8))[0]-engine.base
    seen, frontier, operands = set(), {process}, []
    for _ in range(3):
        following = set()
        for function in frontier-seen:
            seen.add(function)
            try:
                body = function_body(engine, function)
            except AssertionError:
                continue
            operands.extend(i.op_str for i in body)
            following |= calls(body)
        frontier = following
    text = '\n'.join(operands)
    assert '0x1c0]' in text and '0x1cc]' in text, 'parabolic lamp sphere'
    update = struct.unpack('<Q', engine.read(engine.mapping[67506]+0x0e*8, 8))[0]-engine.base
    body = function_body(engine, update)
    spot = '\n'.join(i.op_str for i in body)
    assert '0x204]' in spot and '0x138]' in spot, 'spot reach'
    assert any(i.mnemonic == 'mov' and '0x560]' in i.op_str and i.op_str.endswith(', 0xf') for i in body), 'spot render mode'
    return {'parabolic_process': hex(process), 'spot_update': hex(update)}


# Fallout 4 VR 1.2.72 (Ghidra Combined, 2026-10-24): the cascade gather call sites the plugin hooks for the
# per-frame sun state, the gather, the fit, and the cascade-mask call site it hooks to add stale cascades.
VR_GATHER = 0x290D640
VR_GATHER_CALLS = (0x2847921, 0x2844154)
VR_FIT = 0x290DBD0
VR_MASK_CHECKED = (0x284EA63, '8b44846089055772030' + '4e88efdf5ff')
VR_MASK_CALL = (0x284EA6D, 0x27AE800)
VR_PARABOLIC_SET_CAMERA = 0x299C500
VR_PLAYER_USER = 0x3F0890  # FindReferenceFor3D compares against the player global


def verify_vr(engine, profile):
    findings = []
    for name, hook in profile['hooks'].items():
        if hook is None:
            continue
        assert struct.unpack('<Q', engine.read(hook['table']+hook['slot']*8, 8))[0] == engine.base+hook['target'], name
        assert engine.read(hook['target'], 32).hex() == hook['prefix'], name
        findings.append({'hook': name, 'vtable_rva': hex(hook['table']), 'slot': hex(hook['slot']), 'target_rva': hex(hook['target'])})
    assert profile['hooks']['sun_update'] is None and profile['hooks']['sun_render'] is None
    entry = profile['register_object']
    target = struct.unpack('<Q', engine.read(entry['table']+entry['slot']*8, 8))[0]-engine.base
    assert target == entry['target'], 'VR RegisterObject slot'
    body = engine.instructions(target, 0x180)
    table = profile['register_table']
    assert any(i.mnemonic == 'lea' and i.operands[1].type == capstone.CS_OP_MEM and
               i.operands[1].mem.base == capstone.x86.X86_REG_RIP and rip_target(engine, i, i.operands[1]) == table
               for i in body), 'VR RegisterObject does not load the profiled handler table'
    assert any(i.mnemonic == 'movsxd' and '0xf688]' in i.op_str for i in body), 'VR accumulator render mode'
    assert any(i.mnemonic == 'mov' and i.op_str == 'r9d, r14d' for i in body), 'VR handlers take a fourth argument'
    writes = {}
    for name, required in (('shadow', {15, 16}), ('occlusion', {14})):
        handler = profile['handlers'][name]
        assert engine.read(handler['target'], 32).hex() == handler['prefix'], (name, 'handler prefix')
        slots = table_writes(engine, handler['target'], table)
        assert required <= slots, (name, sorted(slots))
        writes[name] = sorted(slots)
    for call in VR_GATHER_CALLS:
        code = engine.read(call, 5)
        assert code[0] == 0xE8 and call+5+struct.unpack('<i', code[1:])[0] == VR_GATHER, ('VR gather call', hex(call))
    gather = '\n'.join(i.op_str for i in function_body(engine, VR_GATHER))
    assert '0x190]' in gather and '0x198]' in gather and '0x110' in gather, 'VR gather walks 0x110-byte descriptors'
    assert '0x50]' in gather and '0x58]' in gather, 'VR gather uses both cascade accumulators'
    fit = '\n'.join(i.op_str for i in function_body(engine, VR_FIT))
    for offset in ('0x1f0]', '0x200]', '0x210]'):
        assert offset in fit, ('VR sun blend field', offset)
    start, expected = VR_MASK_CHECKED
    assert engine.read(start, len(expected)//2).hex() == expected, 'VR cascade mask site'
    call, callee = VR_MASK_CALL
    code = engine.read(call, 5)
    assert code[0] == 0xE8 and call+5+struct.unpack('<i', code[1:])[0] == callee, 'VR cascade mask call'
    lamp = '\n'.join(i.op_str for i in function_body(engine, VR_PARABOLIC_SET_CAMERA))
    assert '0x1c0]' in lamp and '0x1cc]' in lamp, 'VR parabolic lamp sphere'
    camera = engine.instructions(profile['camera'], 15)
    assert [i.mnemonic for i in camera] == ['mov', 'mov', 'ret'] and '0x180]' in camera[1].op_str, 'VR world camera getter'
    assert engine.read(profile['camera'], 32).hex() == profile['camera_prefix']
    player_refs = [hex(i.address-engine.base) for i in function_body(engine, VR_PLAYER_USER) for o in i.operands
                   if o.type == capstone.CS_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP and
                   i.address+i.size+o.mem.disp-engine.base == profile['player_slot']]
    assert player_refs, 'VR player global not referenced'
    return {'runtime': '.'.join(map(str, engine.version)), 'hooks': findings, 'camera_getter': hex(profile['camera']),
            'register_table': hex(table), 'register_slots': writes, 'player_slot': hex(profile['player_slot']),
            'player_native_refs': player_refs, 'gather_calls': [hex(c) for c in VR_GATHER_CALLS],
            'mask_call': hex(VR_MASK_CALL[0])}


def verify(profile):
    engine = Engine(GAMES[profile['runtime']])
    assert list(engine.version) == profile['version']
    assert hashlib.sha256(engine.path.read_bytes()).hexdigest() == profile['executable_sha256']
    if profile['runtime'] == 'vr':
        return verify_vr(engine, profile)
    findings = []
    for name, hook in profile['hooks'].items():
        assert engine.mapping[hook['id']] == hook['table']
        assert struct.unpack('<Q', engine.read(hook['table']+hook['slot']*8, 8))[0] == engine.base+hook['target']
        assert engine.read(hook['target'],32).hex() == hook['prefix']
        findings.append({'hook': name, 'vtable_rva': hex(hook['table']),
                         'slot': hex(hook['slot']), 'target_rva': hex(hook['target'])})
    update = function_body(engine, profile['hooks']['sun_update']['target'])
    operands = '\n'.join(i.op_str for i in update)
    # The native camera updater actually reads the shadow descriptor array,
    # walks F0-byte descriptors, and uses the native target/previous/progress
    # state to interpolate the direction used by its cameras.
    for marker in ('0x200', '0xb8'):
        assert marker in operands, ('sun layout', marker)
    light_register = 'r15' if profile['runtime'] == 'og' else 'rcx'
    for offset in (0x200, 0x210, 0x220):
        assert any(f'[{light_register} + {offset:#x}]' in i.op_str for i in update), ('native sun blend field', hex(offset))
    table = profile['hooks']['sun_update']['table']
    transform_getter = struct.unpack('<Q',engine.read(table+4*8,8))[0]-engine.base
    count_getter = struct.unpack('<Q',engine.read(table+15*8,8))[0]-engine.base
    transform = '\n'.join(x.op_str for x in engine.instructions(transform_getter,60))
    count = '\n'.join(x.op_str for x in engine.instructions(count_getter,64))
    assert '0xf0' in transform and '0x198' in transform, 'descriptor stride/array'
    assert '0x190' in count, 'descriptor count'
    assert any('0x40]' in i.op_str and i.mnemonic=='mov' for i in update), 'descriptor camera'
    assert any('0xe0]' in i.op_str and i.mnemonic=='mov' for i in update), 'descriptor culler'
    accumulate = function_body(engine, ACCUMULATE_FROM_LISTS[profile['runtime']])
    accumulate_operands = '\n'.join(i.op_str for i in accumulate)
    assert '0x198]' in accumulate_operands and '0x190]' in accumulate_operands, 'cascade accumulation reads the descriptors'
    assert any('0x48]' in i.op_str and i.mnemonic=='mov' for i in accumulate), 'descriptor accumulator'
    render = function_body(engine, profile['hooks']['sun_render']['target'])
    render_operands = '\n'.join(i.op_str for i in render)
    # Render(mask) walks the F0-byte descriptors and renders only masked cascades, which is
    # what lets a staggered cascade keep its map and matrix (RenderShadowMap computes it).
    assert '0x190]' in render_operands and '0x198]' in render_operands and '0xf0' in render_operands, 'sun Render loops descriptors'
    assert any(i.mnemonic in ('bt', 'shr', 'test') for i in render), 'sun Render tests the cascade mask'
    cull_sites = []
    accumulate_starts = {i.address-engine.base for i in accumulate}
    for call, target in CASCADE_CULL_SITES.get(profile['runtime'], []):
        code = engine.read(call, 5)
        assert code[0] == 0xE8 and call+5+struct.unpack('<i', code[1:])[0] == target, ('cascade cull site', hex(call))
        assert call in accumulate_starts, ('cascade cull site outside AccumulateFromLists', hex(call))
        cull_sites.append(hex(call))
    for call, target in SUN_WORKER_SITES.get(profile['runtime'], []) + [SUN_GLARE_SITES[profile['runtime']]]:
        code = engine.read(call, 5)
        assert code[0] == 0xE8 and call+5+struct.unpack('<i', code[1:])[0] == target, ('sun worker/glare site', hex(call))
        cull_sites.append(hex(call))
    if cbro := CBRO_SITES.get(profile['runtime']):
        for call, target in (cbro['publish'], cbro['inline_cull']):
            code = engine.read(call, 5)
            assert code[0] == 0xE8 and call+5+struct.unpack('<i', code[1:])[0] == target, ('CBRO-mode site', hex(call))
            cull_sites.append(hex(call))
        start, expected = cbro['prologue']
        assert engine.read(start, len(expected)//2).hex() == expected, 'CBRO publish: the light in rdi'
        assert cbro['publish'][0] in accumulate_starts, 'CBRO publish site outside AccumulateFromLists'
        fit = engine.instructions(cbro['fit_caller'], 0x60)
        assert any(op.type == capstone.CS_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP and
                   rip_target(engine, i, op) == cbro['vis_camera'] for i in fit for op in i.operands), 'DrawWorld::pVisCamera'
    guard_call, guard_target, guard_mode = SLICE_GUARD_SITES[profile['runtime']]
    for call, target in SHADOW_CACHE_SITES.get(profile['runtime'], []) + [(guard_call, guard_target)]:
        code = engine.read(call, 5)
        assert code[0] == 0xE8 and call+5+struct.unpack('<i', code[1:])[0] == target, ('shadow cache/slice guard site', hex(call))
        cull_sites.append(hex(call))
    assert engine.read(guard_mode, 3) == bytes.fromhex('4533c0'), 'slice guard clear mode setup'
    accumulate_query = engine.read(SUN_WORKER_SITES[profile['runtime']][1][0], 0x27)
    assert all(accumulate_query[offset:offset+len(expected)] == expected for offset, expected in SUN_ACCUMULATE_PATTERN), \
        'sun accumulate query operands'
    registration = verify_registration(engine, profile)
    lamps = verify_lamp_layouts(engine)
    camera = engine.instructions(profile['camera'], 15)
    assert [i.mnemonic for i in camera] == ['mov','mov','ret']
    assert '0x140]' in camera[1].op_str
    assert engine.read(profile['camera'],32).hex() == profile['camera_prefix']
    player_refs = []
    # Prove g_player is a real native global reference. The modern RVA also
    # agrees with ianpatt/f4se's CURRENT_RELEASE_RUNTIME_1_11_240 GameReferences.cpp.
    if profile['runtime'] == 'og':
        godmode = engine.mapping[1032309]
        for i in function_body(engine, godmode):
            for operand in i.operands:
                if operand.type == capstone.CS_OP_MEM and operand.mem.base == capstone.x86.X86_REG_RIP:
                    target = i.address+i.size+operand.mem.disp-engine.base
                    if target==profile['player_slot']:
                        player_refs.append(hex(i.address-engine.base))
        assert player_refs, 'OG player global not referenced by native god-mode query'
    else:
        if profile['runtime'] == 'ae':
            assert profile['player_slot'] == 0x32DD370
        assert engine.mapping[2698073] == profile['player_slot']
    return {'runtime': '.'.join(map(str, engine.version)), 'hooks': findings,
            'sun_layout_checks': 13, 'camera_getter': hex(profile['camera']),
            'register_table': hex(profile['register_table']), 'register_slots': registration,
            'lamp_layouts': lamps, 'player_slot': hex(profile['player_slot']), 'player_native_refs': player_refs,
            'cascade_cull_sites': cull_sites}


def verify_dll(path):
    pe = pefile.PE(str(path))
    exports = {symbol.name:symbol for symbol in pe.DIRECTORY_ENTRY_EXPORT.symbols}
    for name in (b'F4SEPlugin_Query',b'F4SEPlugin_Load',b'F4SEPlugin_Version'):
        assert name in exports
    assert not any(name and name.startswith(b'FPSBenchmark') for name in exports), 'private controls in shipping DLL'
    data = pe.get_data(exports[b'F4SEPlugin_Version'].address, 0x250)
    assert struct.unpack_from('<I',data)[0]==1
    assert struct.unpack_from('<I',data,4)[0]==0x01000000, 'incorrect plugin version'  # 1.0.0.0
    assert data[8:].split(b'\0',1)[0] == b'FasterShadows'
    address_flags, structure_flags = struct.unpack_from('<II',data,0x208)
    assert address_flags == 6 and structure_flags == 0, 'incorrect F4SE compatibility flags'
    versions = struct.unpack_from('<16I',data,0x210)
    assert versions == (0x010A0A30,0x010A3D80,0x010B0F00)+(0,)*13, 'incorrect runtime allowlist'  # 16 slots
    dependencies = [item.dll.decode('ascii') for item in pe.DIRECTORY_ENTRY_IMPORT]
    assert not any('skse' in item.lower() for item in dependencies)
    return {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'bytes': path.stat().st_size, 'exports': [x.decode() for x in exports],
            'dependencies': dependencies, 'address_flags': address_flags,
            'structure_flags': structure_flags, 'compatible_versions': [hex(v) for v in versions if v]}


if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path, default=ROOT/'build-release/Release/FasterShadows.dll')
    parser.add_argument('--engine-only', action='store_true',
                        help='verify the executables only; packaging still needs a full run with the built DLL')
    args = parser.parse_args()
    profiles = json.loads((ROOT/'evidence/engine-profiles.json').read_text())
    result = {'static_only': True, 'gameplay_tested': False, 'runtimes': [verify(p) for p in profiles]}
    if args.engine_only:
        (ROOT/'evidence/engine-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    else:
        result['dll'] = verify_dll(args.dll)
        (ROOT/'evidence/verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
