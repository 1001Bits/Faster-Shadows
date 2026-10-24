"""Generate exact-version hook fingerprints from local, unmodified game files."""
import hashlib
import json
import struct
from pathlib import Path
from inspect_engine import Engine, GAMES

ROOT = Path(__file__).resolve().parents[1]
SLOTS = {
    'sun_update': (97945, 0x0e),
    'point_begin': (845854, 0x1a),
    'point_append': (845854, 0x1b),
    # BSShadowDirectionalLight::Render(uint cascadeMask): DeferredLightsImpl calls it with 7.
    # It loops the cascades and calls BSShadowLight::RenderShadowMap for each set bit;
    # the cascade's world->shadow matrix is computed only there (Ghidra OG 0x28CA710).
    'sun_render': (97945, 0x0a),
}
# BSShaderAccumulator::RegisterObject (vtable 357329, slot 0x2D) dispatches every
# registration through the render-mode table pRegisterObjectFunc. Cascade (mode 16),
# spot (15) and rain-occlusion (14) entries are filled at renderer start. Addresses come
# from the Ghidra Combined project (2026-10-11): OG by PDB name, AE 1.11.240 by its
# 1.11.221 map names, both confirmed by decompiling RegisterObject and the table writer.
REGISTER_OBJECT = (357329, 0x2d)
REGISTER = {
    'og': {'table': 0x6722DB0, 'shadow': 0x282D800, 'occlusion': 0x282DAF0},
    'ae': {'table': 0x3E5F290, 'shadow': 0x21D51A0, 'occlusion': 0x21D54C0},
    # Next-Gen 1.10.984 (Ghidra Combined, 2026-10-23): the AE layout at NG addresses; the table
    # writer 0x2023C50 fills the same slots (shadow 13, 15-17; occlusion 14).
    'ng': {'table': 0x3CA3980, 'shadow': 0x2078000, 'occlusion': 0x2078320},
}
RUNTIMES = ('og', 'ae', 'ng', 'vr')
# Fallout 4 VR 1.2.72 (Ghidra Combined, 2026-10-24; VR has no binary address library). Its sun light has
# no camera-update slot (the plugin publishes the frame from the cascade gather instead) and it staggers
# cascades natively (the Render slot stays unused), so those two hook entries stay empty. The parabolic
# culler's Begin/Append are slots 0x1C/0x1D; RegisterObject is slot 0x2C and passes a fourth argument.
VR = {'camera': 0xD87550, 'player_slot': 0x5B043F0,
      'hooks': {'sun_update': None, 'point_begin': (0x30C6308, 0x1C), 'point_append': (0x30C6308, 0x1D), 'sun_render': None},
      'register_object': (0x30B6AD8, 0x2C), 'table': 0x6879860, 'shadow': 0x281C730, 'occlusion': 0x281CB50}


def vr_profile(engine):
    hooks = {}
    for name, site in VR['hooks'].items():
        if site is None:
            hooks[name] = None
            continue
        table, slot = site
        target = struct.unpack('<Q', engine.read(table + slot*8, 8))[0] - engine.base
        hooks[name] = {'slot': slot, 'table': table, 'target': target, 'prefix': engine.read(target, 32).hex()}
    table, slot = VR['register_object']
    return {'version': list(engine.version), 'runtime': 'vr', 'camera': VR['camera'],
            'camera_prefix': engine.read(VR['camera'], 32).hex(), 'player_slot': VR['player_slot'],
            'executable_sha256': hashlib.sha256(engine.path.read_bytes()).hexdigest(), 'hooks': hooks,
            'register_object': {'slot': slot, 'table': table,
                                'target': struct.unpack('<Q', engine.read(table + slot*8, 8))[0] - engine.base},
            'register_table': VR['table'],
            'handlers': {name: {'target': VR[name], 'prefix': engine.read(VR[name], 32).hex()} for name in ('shadow', 'occlusion')}}


def generate():
    profiles = []
    for runtime in RUNTIMES:
        engine = Engine(GAMES[runtime])
        if runtime == 'vr':
            profiles.append(vr_profile(engine))
            continue
        hooks = {}
        for name, (identifier, slot) in SLOTS.items():
            table = engine.mapping[identifier]
            target = struct.unpack('<Q', engine.read(table + slot*8, 8))[0] - engine.base
            hooks[name] = {'id': identifier, 'slot': slot, 'table': table,
                           'target': target, 'prefix': engine.read(target, 32).hex()}
        identifier, slot = REGISTER_OBJECT
        vtable = engine.mapping[identifier]
        register_object = {'id': identifier, 'slot': slot, 'table': vtable,
                           'target': struct.unpack('<Q', engine.read(vtable + slot*8, 8))[0] - engine.base}
        handlers = {name: {'target': REGISTER[runtime][name], 'prefix': engine.read(REGISTER[runtime][name], 32).hex()}
                    for name in ('shadow', 'occlusion')}
        camera_id = 384264 if runtime == 'og' else 2228956
        # AE's official F4SE g_player = 0x32DD370; the older CommonLib NG
        # singleton ID 2690919 is absent in this database. Never use a nearest ID.
        player_id = 303410 if runtime == 'og' else 2698073
        camera = engine.mapping[camera_id]
        profiles.append({'version': list(engine.version), 'runtime': runtime,
                         'camera': camera, 'camera_prefix': engine.read(camera, 32).hex(),
                         'player_slot': engine.mapping[player_id],
                         'executable_sha256': hashlib.sha256(engine.path.read_bytes()).hexdigest(),
                         'hooks': hooks, 'register_object': register_object,
                         'register_table': REGISTER[runtime]['table'], 'handlers': handlers})
    (ROOT / 'evidence').mkdir(exist_ok=True)
    (ROOT / 'evidence/engine-profiles.json').write_text(json.dumps(profiles, indent=2)+'\n')

    def array(hexes):
        return ', '.join('0x'+hexes[i:i+2] for i in range(0, 64, 2))

    lines = ['// Generated by tools/make_profiles.py. See evidence/engine-profiles.json.',
             '#pragma once', '#include <array>', '#include <cstdint>',
             'namespace fps {',
             'struct HookSite { std::uint32_t table, target; std::uint8_t slot; std::array<std::uint8_t, 32> prefix; };',
             'struct HandlerSite { std::uint32_t target; std::array<std::uint8_t, 32> prefix; };',
             'struct Profile { std::array<std::uint16_t, 4> version; std::uint32_t camera, playerSlot; std::array<std::uint8_t, 32> cameraPrefix; '
             'std::array<HookSite, 4> hooks; std::uint32_t registerTable; HandlerSite shadowHandler, occlusionHandler; };',
             f'inline constexpr std::array<Profile, {len(profiles)}> profiles{{{{']
    for profile in profiles:
        lines.append('  {{'+', '.join(map(str, profile['version']))+f'}}, 0x{profile["camera"]:X}, 0x{profile["player_slot"]:X}, '
                     f'{{{array(profile["camera_prefix"])}}}, {{{{')
        for h in profile['hooks'].values():
            if h is None:  # no such site on this runtime; never selected there
                lines.append(f'    {{0x0, 0x0, 0x0, {{{array("00"*32)}}}}},')
                continue
            lines.append(f'    {{0x{h["table"]:X}, 0x{h["target"]:X}, 0x{h["slot"]:X}, {{{array(h["prefix"])}}}}},')
        shadow, occlusion = profile['handlers']['shadow'], profile['handlers']['occlusion']
        lines.append(f'  }}}}, 0x{profile["register_table"]:X},')
        lines.append(f'    {{0x{shadow["target"]:X}, {{{array(shadow["prefix"])}}}}},')
        lines.append(f'    {{0x{occlusion["target"]:X}, {{{array(occlusion["prefix"])}}}}}}},')
    lines.extend(['}};', '}'])
    (ROOT / 'src').mkdir(exist_ok=True)
    (ROOT / 'src/EngineProfiles.h').write_text('\n'.join(lines)+'\n')
    print(json.dumps(profiles, indent=2))


if __name__ == '__main__':
    generate()
