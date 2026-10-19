"""Runtime hosting choices do not bind firmware to a deployment."""
from pathlib import Path
import runpy
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BuildIdentityTests(unittest.TestCase):
    def identity(self, origin, relay="enabled"):
        with tempfile.TemporaryDirectory() as directory:
            class Environment(dict):
                def subst(self, value):
                    return {"$PROJECT_DIR": str(ROOT), "$BUILD_DIR": directory,
                            "$PIOENV": "magbound_v2_esp32c3"}[value]
                def Append(self, **_kwargs):
                    pass
                def Depends(self, *_args):
                    pass
            env = Environment(OPENSMARTTIMER_RELAY=relay, OPENSMARTTIMER_PARTNER_ORIGIN=origin)
            runpy.run_path(str(ROOT / "scripts/build/build_identity.py"),
                           init_globals={"env": env, "Import": lambda _name: None})
            return (Path(directory) / "generated_build_identity.h").read_text()

    def test_runtime_origin_does_not_change_build_identity(self):
        first = self.identity("https://partner.example.com")
        self.assertEqual(first, self.identity("https://partner.example.com"))
        self.assertEqual(first, self.identity("https://other.example.com"))

    def test_relay_compilation_switch_changes_build_identity(self):
        self.assertNotEqual(self.identity(""), self.identity("", "disabled"))


if __name__ == "__main__":
    unittest.main()
