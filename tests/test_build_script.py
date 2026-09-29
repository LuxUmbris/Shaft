import importlib.util
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_build_script():
    specification = importlib.util.spec_from_file_location("shaft_build_script", ROOT / "build.py")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


class BuildScriptTests(unittest.TestCase):
    def test_build_uses_repository_absolute_source_and_build_paths(self):
        build_script = load_build_script()
        target = next(iter(build_script.TARGETS))
        expected_build_dir = ROOT / f"build-{build_script.OS}-{target}-release"
        commands = []

        with mock.patch.object(build_script.os, "makedirs") as makedirs:
            with mock.patch.object(build_script, "run", side_effect=commands.append):
                build_script.build(target, "release")

        makedirs.assert_called_once_with(expected_build_dir, exist_ok=True)
        self.assertEqual(commands[0][0:5], ["cmake", "-S", str(ROOT), "-B", str(expected_build_dir)])
        self.assertEqual(commands[1][0:3], ["cmake", "--build", str(expected_build_dir)])


if __name__ == "__main__":
    unittest.main()
