"""The Mod Configuration Menu page: its ids, MCM's defaults, the shipped INI and the plugin's keys agree."""
import configparser
import json
import re
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
MCM=ROOT/'package/MCM/Config/FasterShadows'
# Menu key -> the FasterShadows.ini key whose default it shows (General bEnabled is the runtime switch, on).
INI_KEYS={('General','bEnabled'):None,('SunShadows','bCascadeCadence'):'CascadeCadence',
          ('SunShadows','bCadenceSkipCull'):'CadenceSkipCull',('SunShadows','bCullOnWorkerThread'):'CullOnWorkerThread',
          ('SunShadows','bEnabled'):'Enabled',('ActorShadows','bEnabled'):'Enabled',('PointShadows','bEnabled'):'Enabled',
          ('RainOcclusion','bEnabled'):'Enabled',('VR','bCascadeRedraw'):'CascadeRedraw'}


def ini(path):
    parser=configparser.ConfigParser(inline_comment_prefixes=(';',));parser.optionxform=str
    parser.read_string(path.read_text(encoding='utf-8'))
    return parser


class McmConfigTests(unittest.TestCase):
    def setUp(self):
        self.config=json.loads((MCM/'config.json').read_text(encoding='utf-8'))
        self.ids=[tuple(reversed(item['id'].split(':'))) for item in self.config['content'] if 'id' in item]

    def test_page_names_the_plugin(self):
        self.assertEqual(self.config['modName'],'FasterShadows')
        self.assertTrue(all(item['valueOptions']['sourceType']=='ModSettingBool' for item in self.config['content'] if 'id' in item))

    def test_every_switch_has_a_default_matching_the_shipped_ini(self):
        defaults,shipped=ini(MCM/'settings.ini'),ini(ROOT/'package/FasterShadows.ini')
        self.assertEqual(set(self.ids),set(INI_KEYS))
        for section,key in self.ids:
            expected=1 if INI_KEYS[section,key] is None else shipped.getint(section,INI_KEYS[section,key])
            self.assertEqual(defaults.getint(section,key),expected,f'{key}:{section}')

    def test_the_plugin_reads_every_switch(self):
        source=(ROOT/'src/Settings.cpp').read_text()+(ROOT/'src/ShadowHooks.cpp').read_text()
        read=set(re.findall(r'L"(\w+)", L"(b\w+)"',source))
        self.assertEqual(set(self.ids)-read,set())


if __name__=='__main__':
    unittest.main()
