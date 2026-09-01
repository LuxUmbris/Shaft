import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file(), "build shaftc before running this test")
class CheckerLifetimeTests(unittest.TestCase):
    def check_source(self, source_text):
        with tempfile.TemporaryDirectory(prefix="shaftc-checker-lifetime-") as directory:
            source = Path(directory) / "lifetime.shaft"
            source.write_text(source_text, encoding="utf-8")
            return subprocess.run(
                [str(SHAFTC), "--no-std", "--check-only", str(source)],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_borrow_expires_when_its_block_ends(self):
        result = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
            "{\n"
            "    i32 source = 7;\n"
            "    if (argc >= 0)\n"
            "    {\n"
            "        &i32 borrowed = ref source;\n"
            "    }\n"
            "    i32 moved = move source;\n"
            "    return moved;\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_many_expired_borrows_do_not_accumulate(self):
        blocks = "".join(
            "    if (argc >= 0)\n"
            "    {\n"
            f"        &i32 borrowed{index} = ref source;\n"
            "    }\n"
            for index in range(1_000)
        )
        result = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    i32 source = 7;\n"
            + blocks
            + "    i32 moved = move source;\n"
            "    return moved;\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_live_mutable_borrow_blocks_a_move(self):
        result = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
            "{\n"
            "    mut i32 source = 7;\n"
            "    &mut i32 writer = ref source;\n"
            "    i32 moved = move source;\n"
            "    return moved;\n"
            "}\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot move a borrowed variable", result.stderr)

    def test_live_borrow_still_blocks_a_move(self):
        result = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
            "{\n"
            "    i32 source = 7;\n"
            "    &i32 borrowed = ref source;\n"
            "    i32 moved = move source;\n"
            "    return moved;\n"
            "}\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot move a borrowed variable", result.stderr)

    def test_field_assignment_requires_mutable_container_and_field(self):
        cases = [
            (
                "immutable container",
                "struct Pair\n{\n    mut i32 value;\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    Pair pair;\n    pair.value = 7;\n    return 0;\n}\n",
                False,
                "containing value is not mutable",
            ),
            (
                "immutable field",
                "struct Pair\n{\n    i32 value;\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut Pair pair;\n    pair.value = 7;\n    return 0;\n}\n",
                False,
                "field is not mutable",
            ),
            (
                "both mutable",
                "struct Pair\n{\n    mut i32 value;\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut Pair pair;\n    pair.value = 7;\n    return pair.value;\n}\n",
                True,
                "",
            ),
        ]
        for label, source, expected_success, diagnostic in cases:
            with self.subTest(label=label):
                result = self.check_source(source)
                self.assertEqual(result.returncode == 0, expected_success, result.stdout + result.stderr)
                if diagnostic:
                    self.assertIn(diagnostic, result.stderr)

    def test_nested_aggregate_borrow_releases_at_scope_end(self):
        result = self.check_source(
            "struct Cell\n{\n    mut i32 value;\n}\n"
            "struct Row\n{\n    mut Cell[] cells;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut Row row;\n"
            "    if (argc >= 0)\n"
            "    {\n"
            "        &i32 view = ref row.cells[0].value;\n"
            "    }\n"
            "    Row moved = move row;\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_checker_handles_many_scopes_and_shadowed_values(self):
        scopes = "".join(
            "    if (argc >= 0)\n"
            "    {\n"
            f"        i32 value = {index};\n"
            "        &i32 borrowed = ref value;\n"
            "    }\n"
            for index in range(2_000)
        )
        result = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    i32 value = 0;\n"
            + scopes
            + "    return value;\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_checker_accepts_partial_move_of_cleanup_owning_aggregate(self):
        result = self.check_source(
            "struct Token\n{\n    mut *i32 resource;\n}\n"
            "struct Pair\n{\n    mut Token[2] tokens;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut Pair pair;\n"
            "    Token extracted = move pair.tokens[0];\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_borrow_stress_releases_ten_thousand_aggregate_path_borrows(self):
        blocks = "".join(
            "    if (argc >= 0)\n"
            "    {\n"
            f"        &i32 view{index} = ref row.cells[0].value;\n"
            "    }\n"
            for index in range(10_000)
        )
        result = self.check_source(
            "struct Cell\n{\n    mut i32 value;\n}\n"
            "struct Row\n{\n    mut Cell[1] cells;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut Row row;\n"
            + blocks
            + "    Row moved = move row;\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_checker_rejects_borrow_of_a_moved_subobject_but_allows_a_sibling_move(self):
        invalid = self.check_source(
            "struct Token\n{\n    mut *i32 resource;\n}\n"
            "struct Holder\n{\n    mut Token[2] tokens;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut Holder holder;\n"
            "    Token first = move holder.tokens[0];\n"
            "    &Token view = ref holder.tokens[0];\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertNotEqual(invalid.returncode, 0)
        self.assertIn("Cannot borrow from moved subobject", invalid.stderr)

        valid = self.check_source(
            "struct Token\n{\n    mut *i32 resource;\n}\n"
            "struct Holder\n{\n    mut Token[2] tokens;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut Holder holder;\n"
            "    Token first = move holder.tokens[0];\n"
            "    Token second = move holder.tokens[1];\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
    def test_immutable_binding_rejects_direct_reassignment(self):
        result = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    i32 value = 1;\n"
            "    value = 2;\n"
            "    return value;\n"
            "}\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("binding is not mutable", result.stderr)

    def test_mutable_binding_allows_direct_reassignment(self):
        result = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut i32 value = 1;\n"
            "    value = 2;\n"
            "    return value;\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_many_read_borrows_are_allowed_but_conflict_with_a_writer(self):
        readers = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    i32 source = 7;\n"
            "    &i32 first = ref source;\n"
            "    &i32 second = ref source;\n"
            "    return source;\n"
            "}\n"
        )
        self.assertEqual(readers.returncode, 0, readers.stdout + readers.stderr)

        writer_after_readers = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut i32 source = 7;\n"
            "    &i32 reader = ref source;\n"
            "    &mut i32 writer = ref source;\n"
            "    return source;\n"
            "}\n"
        )
        self.assertNotEqual(writer_after_readers.returncode, 0)
        self.assertIn("already borrowed", writer_after_readers.stderr)

    def test_live_borrows_block_writes_through_the_original_binding(self):
        shared_reader = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut i32 source = 7;\n"
            "    &i32 reader = ref source;\n"
            "    source = 8;\n"
            "    return source;\n"
            "}\n"
        )
        self.assertNotEqual(shared_reader.returncode, 0)
        self.assertIn("borrowed variable", shared_reader.stderr)

        writer = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut i32 source = 7;\n"
            "    &mut i32 writer = ref source;\n"
            "    source = 8;\n"
            "    return source;\n"
            "}\n"
        )
        self.assertNotEqual(writer.returncode, 0)
        self.assertIn("borrowed variable", writer.stderr)

    def test_mutable_borrow_requires_mutable_source_and_is_exclusive(self):
        immutable_source = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    i32 source = 7;\n"
            "    &mut i32 writer = ref source;\n"
            "    return source;\n"
            "}\n"
        )
        self.assertNotEqual(immutable_source.returncode, 0)
        self.assertIn("source is not mutable", immutable_source.stderr)

        two_writers = self.check_source(
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut i32 source = 7;\n"
            "    &mut i32 first = ref source;\n"
            "    &mut i32 second = ref source;\n"
            "    return source;\n"
            "}\n"
        )
        self.assertNotEqual(two_writers.returncode, 0)
        self.assertIn("already borrowed", two_writers.stderr)

    def test_tunnel_rejects_reference_to_function_local_storage(self):
        result = self.check_source(
            "def borrow() -> &i32 output\n{\n"
            "    i32 value = 7;\n"
            "    tunnel ref value -> &i32 output;\n"
            "}\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("reference to function-local storage", result.stderr)


if __name__ == "__main__":
    unittest.main()
