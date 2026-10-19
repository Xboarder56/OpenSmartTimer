"""Release signing/feed staging with disposable synthetic images and keys."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zlib
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[2]
def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f'scripts/release/{name}.py')
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result
feed = module('update_feed')
# stage_published_updates imports its sibling in the normal CLI path.
import sys
sys.modules['update_feed'] = feed
publisher = module('stage_published_updates')

class UpdateFeedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); (self.root/'include').mkdir()
        self.key = ec.derive_private_key(1,ec.SECP256R1())
        public = feed.public_hex(self.key.public_key())
        (self.root/'include/ReleaseUpdateConfig.h').write_text(
            '#define SMART_TIMER_UPDATE_REPOSITORY "example/timer"\n'
            '#define SMART_TIMER_UPDATE_BASE_URL "https://example.github.io/timer/"\n'
            f'#define SMART_TIMER_UPDATE_PUBLIC_KEY "{public}"\n')
        self.private = self.root/'test-only.pem'
        self.private.write_bytes(self.key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
        self.bundle = self.root/'bundle'; self.bundle.mkdir()
        artifacts=[]
        for target,suffix in [('magbound_v1_esp8266','MagBoundV1-ESP8266'),('magbound_v2_esp32c3','MagBoundV2-ESP32C3')]:
            name=f'OpenSmartTimer-1.0.0-rc.1-{suffix}.bin'; data=b'not flashable test fixture '+target.encode()
            (self.bundle/name).write_bytes(data)
            artifacts.append({'target':target,'file':name,'size_bytes':len(data),'crc32':f'{zlib.crc32(data):08x}','sha256':hashlib.sha256(data).hexdigest()})
        self.release={'version':'1.0.0-rc.1','qualification':'software-verified-only','source':{'repository':'example/timer'},'artifacts':artifacts}
        (self.bundle/'release.json').write_text(json.dumps(self.release))
        self.public=self.key.public_key()
    def sign(self):
        with patch.object(feed,'config',lambda:feed.config_original(self.root)):
            feed.sign(self.bundle,self.private)
    def test_signed_stage_and_mutations(self):
        self.sign(); original=json.loads((self.bundle/'updates.json').read_text())
        output=self.root/'pages'; feed.stage(self.bundle,output,self.public,'Notes from the tagged release')
        self.assertEqual(len(list(output.rglob('*.bin'))),2)
        self.assertFalse(list(output.rglob('*.pem')))
        self.assertLess(len(next(output.rglob('*.json')).read_bytes()),768)
        self.assertTrue(all('release_notes' in json.loads(p.read_text()) for p in output.rglob('*.json')))
        self.assertFalse(any('-rc.json' in p.name or '-stable.json' in p.name for p in output.rglob('*.json')))
        self.assertEqual(len(list(output.glob('updates/magbound*.json'))),2)
        for field,value in [('release_version','1.0.0-rc.2'),('release_version','4294967296.0.0'),
                            ('image_size',100),('image_sha256','0'*64),('target','magbound_v2_esp32c3'),
                            ('release_signature',original['artifacts'][0]['release_signature'].upper())]:
            mutated=copy.deepcopy(original);mutated['artifacts'][0][field]=value
            (self.bundle/'updates.json').write_text(json.dumps(mutated))
            with self.assertRaises(Exception):feed.records(self.bundle,self.public)
        (self.bundle/'updates.json').write_text(json.dumps(original))
        image=self.bundle/original['artifacts'][0]['file'];image.write_bytes(image.read_bytes()+b'bad')
        with self.assertRaises(ValueError):feed.records(self.bundle,self.public)
    def test_signing_key_and_image_validation(self):
        self.private.write_bytes(ec.generate_private_key(ec.SECP256R1()).private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
        with self.assertRaises(ValueError):self.sign()
        self.assertFalse((self.bundle/'updates.json').exists())
    def test_repository_name_case_follows_github_not_the_configured_spelling(self):
        # github.repository reports the account's display case (Example/Timer);
        # GitHub treats owner/repository names as case-insensitive.
        self.release['source']['repository']='Example/Timer'
        (self.bundle/'release.json').write_text(json.dumps(self.release))
        self.sign()
        self.assertTrue((self.bundle/'updates.json').exists())
    def test_another_repository_is_never_signed(self):
        self.release['source']['repository']='example/other'
        (self.bundle/'release.json').write_text(json.dumps(self.release))
        with self.assertRaisesRegex(ValueError,'differs from updater configuration'):self.sign()
        self.assertFalse((self.bundle/'updates.json').exists())
    def test_all_published_tagged_versions_share_one_selection(self):
        releases=[{'tagName':tag,'isPrerelease':'-' in tag} for tag in ['v1.0.0','v1.1.0-rc.2','v1.1.0-rc.10','bad']]
        self.assertEqual(publisher.select(releases)['tagName'],'v1.1.0-rc.10')
        self.assertEqual(publisher.select(releases+[{'tagName':'v1.1.0','isPrerelease':False}])['tagName'],'v1.1.0')
    def test_initial_tagged_release_can_be_a_prerelease(self):
        self.assertEqual(publisher.select([{'tagName':'v1.0.0-rc.1','isPrerelease':True}])['tagName'],'v1.0.0-rc.1')
    def test_drafts_and_untagged_entries_are_excluded(self):
        releases=[{'tagName':'v1.0.0','isPrerelease':False},
                  {'tagName':'v9.0.0','isPrerelease':False,'isDraft':True},
                  {'tagName':'','isPrerelease':False}]
        self.assertEqual(publisher.select(releases)['tagName'],'v1.0.0')
        with self.assertRaises(ValueError):publisher.select([])
        self.assertIsNone(publisher.version_key('v4294967296.0.0'))
        self.assertIsNone(publisher.version_key('v1.0.0-'+'a'*48))
    def test_tag_binding_and_bounded_utf8_release_notes(self):
        self.sign();output=self.root/'pages'
        with self.assertRaisesRegex(ValueError,'published tag'):
            feed.stage(self.bundle,output,self.public,expected_version='1.0.0')
        self.assertFalse(output.exists())
        feed.stage(self.bundle,output,self.public,'文'*5000,expected_version='1.0.0-rc.1')
        for path in output.glob('updates/*.json'):
            notes=json.loads(path.read_text())['release_notes']
            self.assertLessEqual(len(notes.encode('utf8')),8192)
            self.assertNotIn('\ufffd',notes)

feed.config_original=feed.config
if __name__=='__main__':unittest.main()
