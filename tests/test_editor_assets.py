#!/usr/bin/env python3
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class EditorAssetTests(unittest.TestCase):
    def syntax_groups(self, editor: str, syntax: Path) -> list[str]:
        sample = """namespace demo
{
    struct packet
    {
        reserve i32 count = 42;
        reserve char initial = 'x';
    }

    def async compute(packet item) -> i32
    {
        reserve packet value = item.count + 1; // total
        valid value > 0
    }

    @config.package.name in \"release\"
    @!config.build.target matches \"linux\"
    @error \"unsupported target\"
}
"""
        checks = [
            (1, "namespace"), (3, "struct"), (3, "packet"), (5, "reserve"), (5, "i32"), (5, "42"),
            (6, "reserve"), (6, "char"), (6, "'x'"), (9, "def"), (9, "async"), (9, "compute"),
            (9, "packet"), (9, "i32"), (11, "reserve"), (11, "packet"), (11, "value"), (11, "count"),
            (11, "+"), (11, "1"), (11, "//"), (15, "@config"), (15, "in"), (16, "@!config"),
            (16, "matches"), (16, "\"linux\""), (17, "@error"),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "highlighting.shaft"
            output = directory / "groups.txt"
            script = directory / "groups.vim"
            source.write_text(sample, encoding="utf-8")
            commands = [
                f"source {syntax}",
                f"redir! > {output}",
                *[f"silent echo synIDattr(synID({line}, {sample.splitlines()[line - 1].index(token) + 1}, 1), 'name')" for line, token in checks],
                "redir END",
                "qa!",
            ]
            script.write_text("\n".join(commands) + "\n", encoding="utf-8")
            invocation = [editor, "-Nu", "NONE", "-n", "-es", str(source)]
            if Path(editor).name == "nvim":
                invocation = [editor, "--headless", "-u", "NONE", "-n", str(source)]
            result = subprocess.run(invocation + ["-S", str(script)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return [line for line in output.read_text(encoding="utf-8").splitlines() if line]

    def test_vim_syntax_highlights_current_language_keywords_and_types(self):
        syntax = (ROOT / "editors" / "vim" / "syntax" / "shaft.vim").read_text(encoding="utf-8")
        keyword_line = next(line for line in syntax.splitlines() if line.startswith("syn keyword shaftKeyword"))
        type_line = next(line for line in syntax.splitlines() if line.startswith("syn keyword shaftType"))

        for keyword in ("align", "async", "await", "inline", "mut", "self", "sizeof", "start"):
            self.assertRegex(keyword_line, rf"\b{keyword}\b")
        self.assertNotRegex(keyword_line, r"\basyc\b")
        for type_name in ("bool", "char", "State", "Thread", "u8", "i64", "f64"):
            self.assertRegex(type_line, rf"\b{type_name}\b")

    def test_vscode_grammar_recognizes_negated_config_conditions_and_error_directives(self):
        grammar = json.loads((ROOT / "editors" / "vscode-shaft" / "syntaxes" / "shaft.tmLanguage.json").read_text(encoding="utf-8"))
        package = json.loads((ROOT / "editors" / "vscode-shaft" / "package.json").read_text(encoding="utf-8"))
        patterns = grammar["repository"]["macros"]["patterns"]
        directive = next((pattern for pattern in patterns if pattern.get("name") == "keyword.control.preprocessor.shaft"), None)
        condition = next(pattern for pattern in patterns if "in|matches" in pattern.get("match", ""))
        error = next(pattern for pattern in patterns if "@error" in pattern.get("match", ""))

        self.assertIsNotNone(directive)
        self.assertIn("!?config", directive["match"])
        self.assertEqual(directive["name"], "keyword.control.preprocessor.shaft")
        self.assertEqual(condition["captures"]["1"]["name"], "keyword.control.preprocessor.shaft")
        self.assertEqual(error["name"], "invalid.illegal.shaft")
        self.assertEqual(package["contributes"]["semanticTokenScopes"][0]["scopes"]["error"], ["invalid.illegal.shaft"])

    def test_vim_and_neovim_render_complete_shaft_syntax_groups(self):
        expected = [
            "shaftKeyword", "shaftKeyword", "shaftCustomType", "shaftKeyword", "shaftCustomType", "shaftNumber",
            "shaftKeyword", "shaftCustomType", "shaftCharacter", "shaftKeyword", "shaftFunctionModifier", "shaftFunction",
            "shaftCustomType", "shaftCustomType", "shaftKeyword", "shaftCustomType", "shaftVariable", "shaftProperty",
            "shaftOperator", "shaftNumber", "shaftComment", "shaftConfig", "shaftKeyword", "shaftConfig", "shaftKeyword", "shaftString", "shaftError",
        ]
        vim = shutil.which("vim")
        nvim = shutil.which("nvim")
        if not vim or not nvim:
            self.skipTest("Vim and Neovim are required for runtime syntax verification")
        vim_syntax = ROOT / "editors" / "vim" / "syntax" / "shaft.vim"
        neovim_syntax = ROOT / "editors" / "neovim" / "syntax" / "shaft.vim"
        self.assertEqual(self.syntax_groups(vim, vim_syntax), expected)
        self.assertEqual(self.syntax_groups(nvim, neovim_syntax), expected)


    def test_neovim_setup_enables_the_installed_shaft_syntax(self):
        integration = (ROOT / "editors" / "neovim" / "lua" / "shaft" / "init.lua").read_text(encoding="utf-8")
        self.assertIn("vim.cmd('syntax enable')", integration)
        self.assertIn("vim.bo[args.buf].syntax = 'shaft'", integration)


if __name__ == "__main__":
    unittest.main()
