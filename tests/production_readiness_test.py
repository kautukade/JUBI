from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ProductionReadinessTest(unittest.TestCase):
    def test_versions_are_synchronized(self):
        manifest = json.loads((ROOT / 'BUILD_MANIFEST.json').read_text(encoding='utf-8'))
        production = json.loads((ROOT / 'config/production.json').read_text(encoding='utf-8'))
        app = (ROOT / 'sarus/core/app.py').read_text(encoding='utf-8')
        iss = (ROOT / 'installer/SARUS-Setup.iss').read_text(encoding='utf-8')
        self.assertEqual(manifest['version'], '1.3.1')
        self.assertEqual(production['version'], '1.3.1')
        self.assertIn("'version': '1.3.1'", app)
        self.assertIn('#define MyAppVersion "1.3.1"', iss)
        self.assertIn('VersionInfoVersion=1.3.1.0', iss)

    def test_acceptance_is_manifest_driven(self):
        text = (ROOT / 'sarus/acceptance.py').read_text(encoding='utf-8')
        self.assertIn("manifest['indexed_original_files']", text)
        self.assertIn("production.get('required_models'", text)
        self.assertNotIn('17356', text)

    def test_required_models_are_local_configured_models(self):
        prod = json.loads((ROOT / 'config/production.json').read_text(encoding='utf-8'))
        models = json.loads((ROOT / 'config/models.json').read_text(encoding='utf-8'))
        local = set()
        for role, names in models.items():
            if role != 'cloud_disabled':
                local.update(names)
        self.assertTrue(prod['required_models'])
        self.assertTrue(set(prod['required_models']).issubset(local))
        self.assertTrue(set(prod['required_models']).isdisjoint(set(models['cloud_disabled'])))

    def test_installer_has_production_certification_and_model_provisioning(self):
        exe = (ROOT / 'installer/EXE-INSTALL.ps1').read_text(encoding='utf-8')
        acceptance = (ROOT / 'sarus/acceptance.py').read_text(encoding='utf-8')
        self.assertIn('CERTIFY-SARUS.ps1', exe)
        self.assertIn('config\\production.json', exe)
        self.assertIn("'/api/pull'", acceptance)
        self.assertIn("SARUS_INSTALL_MODE", acceptance)
        self.assertIn("'stream': False", acceptance)

    def test_release_signing_is_sha256_and_timestamped(self):
        text = (ROOT / 'installer/SIGN-RELEASE.ps1').read_text(encoding='utf-8')
        self.assertIn('/fd SHA256', text)
        self.assertIn('/tr $TimestampUrl', text)
        self.assertIn('/td SHA256', text)
        self.assertIn('signtool.exe', text)

    def test_obsolete_transfer_automation_is_removed(self):
        obsolete = [
            '.github/workflows/cleanup-source-transfer.yml',
            '.github/workflows/final-17356-gate.yml',
            '.github/workflows/finalize-exact-source-snapshot.yml',
            '.github/workflows/materialize-all-17356.yml',
            '.github/workflows/materialize-sara-private-v4.yml',
            '.github/workflows/materialize-sara-secure-transfer.yml',
            '.github/workflows/materialize-sara-source.yml',
            '.github/workflows/materialize-sara-v2-existing.yml',
            '.github/workflows/repair-and-finalize-17356.yml',
            '.github/workflows/report-live-source-count.yml',
            '.github/workflows/vendor-public-sources.yml',
            '.github/workflows/verify-17356-source-files.yml',
            'CLEANUP_SOURCE_TRANSFER_READY',
            'FINALIZE_EXACT_17356',
            'RUN_FINAL_17356_GATE',
            'RUN_LIVE_SOURCE_REPORT',
            'RUN_REPAIR_FINAL_17356',
            'SARA_SAFE_V2_READY',
            'SARA_SECURE_TRANSFER_READY',
            'VENDOR_PUBLIC_SOURCES_READY',
            'VERIFY_17356_READY',
            'SOURCE_COUNT_STATUS.json',
            'UPLOAD_STATUS.md',
        ]
        self.assertEqual([p for p in obsolete if (ROOT / p).exists()], [])

    def test_active_workflows_have_no_transfer_endpoint_or_write_permission(self):
        workflows = ROOT / '.github/workflows'
        for path in workflows.glob('*.yml'):
            text = path.read_text(encoding='utf-8')
            self.assertNotIn('trycloudflare.com', text, path.name)
            self.assertNotRegex(text, re.compile(r'permissions:\s*\n\s*contents:\s*write'), path.name)

    def test_installer_workflow_keeps_one_canonical_artifact(self):
        text = (ROOT / '.github/workflows/build-windows-installer.yml').read_text(encoding='utf-8')
        self.assertIn("if: github.event_name == 'push' && github.ref == 'refs/heads/main'", text)
        self.assertIn('cleanup-old-installers:', text)
        self.assertIn('actions: write', text)
        self.assertIn("artifact.get('name') != 'SARUS-Windows-Installer'", text)
        self.assertIn('Expected exactly one installer artifact for current run', text)
        self.assertIn("method='DELETE'", text)

    def test_no_security_disable_in_release_scripts(self):
        combined = '\n'.join(
            (ROOT / p).read_text(encoding='utf-8').lower()
            for p in [
                'installer/EXE-INSTALL.ps1',
                'installer/CERTIFY-SARUS.ps1',
                'installer/SIGN-RELEASE.ps1',
                'driver/SarusRing0/INSTALL-RING0.ps1',
            ]
        )
        forbidden = ['bcdedit /set testsigning on', 'disableintegritychecks', 'set-mppreference -disablerealtimemonitoring']
        for token in forbidden:
            self.assertNotIn(token, combined)


if __name__ == '__main__':
    unittest.main(verbosity=2)
