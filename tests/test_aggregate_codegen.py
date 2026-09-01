import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file(), "build shaftc before running this test")
class AggregateCodegenTests(unittest.TestCase):
    def compile_and_run(self, source_text):
        with tempfile.TemporaryDirectory(prefix="shaftc-aggregate-") as directory:
            source = Path(directory) / "aggregate.shaft"
            binary = Path(directory) / "aggregate"
            source.write_text(source_text, encoding="utf-8")
            compilation = subprocess.run(
                [str(SHAFTC), "--no-std", "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            return subprocess.run([str(binary)], capture_output=True, check=False)

    def test_nested_aos_field_storage_executes_without_lifecycle_methods(self):
        execution = self.compile_and_run(
            "struct Cell\n{\n    mut i32 value;\n}\n"
            "struct Grid\n{\n    mut Cell[2] cells;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut Grid grid;\n"
            "    grid.cells[0].value = 40;\n"
            "    grid.cells[1].value = 2;\n"
            "    return grid.cells[0].value + grid.cells[1].value;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_named_runtime_array_dynamic_index_tracks_exact_owned_element(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Resource\n{\n    mut *u8 data;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    u64 count = 1;\n"
            "    {\n"
            "        mut Resource[count] resources;\n"
            "        u64 offset = 0;\n"
            "        resources[offset].data = __shaft_alloc_or_exit(900000);\n"
            "    }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_named_runtime_array_releases_cleanup_bearing_elements(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Resource\n{\n    mut *u8 data;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    u64 count = 2;\n"
            "    {\n"
            "        mut Resource[count] resources;\n"
            "        resources[0].data = __shaft_alloc_or_exit(450000);\n"
            "        resources[1].data = __shaft_alloc_or_exit(450000);\n"
            "    }\n"
            "    *u8 reuse0 = __shaft_alloc_or_exit(450000);\n"
            "    *u8 reuse1 = __shaft_alloc_or_exit(450000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_tunnelled_cleanup_aggregate_transfers_cleanup_to_caller(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Resource\n{\n    mut *u8 data;\n}\n"
            "def make_resource() -> Resource output\n{\n"
            "    reserve mut Resource value;\n"
            "    value.data = __shaft_alloc_or_exit(900000);\n"
            "    tunnel value -> Resource output;\n"
            "}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    {\n        reserve Resource resource = make_resource();\n    }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_tunnelled_nested_cleanup_aggregate_transfers_cleanup_to_caller(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Inner\n{\n    mut *u8 data;\n}\n"
            "struct Outer\n{\n    mut Inner inner;\n}\n"
            "def make_outer() -> Outer output\n{\n"
            "    reserve mut Outer value;\n"
            "    value.inner.data = __shaft_alloc_or_exit(900000);\n"
            "    tunnel value -> Outer output;\n"
            "}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    { reserve Outer outer = make_outer(); }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_present_optional_owned_tunnel_transfers_cleanup_to_caller(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Resource\n{\n    mut *u8 data;\n}\n"
            "def make_resource() ?-> Resource output\n{\n"
            "    reserve mut Resource value;\n"
            "    value.data = __shaft_alloc_or_exit(900000);\n"
            "    tunnel value -> Resource output;\n"
            "}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    { reserve ?Resource resource = make_resource(); }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_absent_optional_owned_tunnel_keeps_cleanup_leaves_inactive(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Resource\n{\n    mut *u8 data;\n}\n"
            "def no_resource() ?-> Resource output\n{\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    { reserve ?Resource resource = no_resource(); }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_multi_reserve_owned_tunnel_transfers_cleanup_to_caller(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Resource\n{\n    mut *u8 data;\n}\n"
            "def make_pair() -> Resource resource, -> i32 number\n{\n"
            "    reserve mut Resource value;\n"
            "    value.data = __shaft_alloc_or_exit(900000);\n"
            "    tunnel value -> Resource resource;\n"
            "    tunnel 7 -> i32 number;\n"
            "}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    { reserve Resource resource, i32 number = make_pair(); }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_standalone_multiple_optional_nested_owned_tunnel_outputs_transfer_cleanup(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Inner\n{\n    mut *u8 data;\n}\n"
            "struct Outer\n{\n    mut Inner inner;\n}\n"
            "def pair() ?-> Outer first, ?-> Outer second\n{\n"
            "    reserve mut Outer value;\n"
            "    value.inner.data = __shaft_alloc_or_exit(900000);\n"
            "    tunnel value -> Outer first;\n"
            "}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    { reserve ?Outer first; reserve ?Outer second; pair(); }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_standalone_multiple_optional_owned_tunnel_outputs_transfer_cleanup(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Resource\n{\n    mut *u8 data;\n}\n"
            "def pair() ?-> Resource first, ?-> Resource second\n{\n"
            "    reserve mut Resource value;\n"
            "    value.data = __shaft_alloc_or_exit(900000);\n"
            "    tunnel value -> Resource first;\n"
            "}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    { reserve ?Resource first; reserve ?Resource second; pair(); }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_standalone_owned_reservation_transfers_cleanup_to_caller(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Resource\n{\n    mut *u8 data;\n}\n"
            "def make_resource() -> Resource resource\n{\n"
            "    reserve mut Resource value;\n"
            "    value.data = __shaft_alloc_or_exit(900000);\n"
            "    tunnel value -> Resource resource;\n"
            "}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    { reserve Resource resource; make_resource(); }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_tunnel_materializes_a_present_optional_payload(self):
        execution = self.compile_and_run(
            "def produce() -> ?i32 output\n{\n"
            "    tunnel 42 -> ?i32 output;\n"
            "}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    ?i32 value = produce();\n"
            "    valid value\n    {\n        return value;\n    }\n"
            "    return 1;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_reserve_fresh_allocation_is_released_at_scope_exit(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    {\n"
            "        reserve *u8 value = __shaft_alloc_or_exit(900000);\n"
            "    }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_scope_exit_releases_compiler_managed_raw_allocation(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    {\n"
            "        *u8 first = __shaft_alloc_or_exit(900000);\n"
            "    }\n"
            "    *u8 second = __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_optional_fresh_allocation_is_released_at_scope_exit(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    {\n"
            "        ?*u8 value = __shaft_alloc_or_exit(900000);\n"
            "    }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_optional_assignment_releases_and_rearms_ownership(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    mut ?*u8 value;\n"
            "    value = __shaft_alloc_or_exit(450000);\n"
            "    value = __shaft_alloc_or_exit(450000);\n"
            "    value = __shaft_alloc_or_exit(450000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_pointer_copy_is_a_borrowed_alias_not_a_second_owner(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    *u8 owner = __shaft_alloc_or_exit(900000);\n"
            "    {\n"
            "        *u8 alias = owner;\n"
            "    }\n"
            "    __shaft_alloc_or_exit(900000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 70, execution.stderr)

    def test_recursive_struct_cleanup_releases_aos_pointer_fields(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct Buffer\n{\n    mut *u8 data;\n}\n"
            "struct Pair\n{\n    mut Buffer[2] buffers;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    {\n"
            "        mut Pair pair;\n"
            "        pair.buffers[0].data = __shaft_alloc_or_exit(450000);\n"
            "        pair.buffers[1].data = __shaft_alloc_or_exit(450000);\n"
            "    }\n"
            "    *u8 replacement0 = __shaft_alloc_or_exit(450000);\n"
            "    *u8 replacement1 = __shaft_alloc_or_exit(450000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_recursive_struct_cleanup_releases_soa_backing_buffers(self):
        execution = self.compile_and_run(
            "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
            "struct SoaGrid\n{\n    mut *u8 columns;\n    mut *u8 rows;\n}\n"
            "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
            "    {\n"
            "        mut SoaGrid grid;\n"
            "        grid.columns = __shaft_alloc_or_exit(450000);\n"
            "        grid.rows = __shaft_alloc_or_exit(450000);\n"
            "    }\n"
            "    *u8 replacement0 = __shaft_alloc_or_exit(450000);\n"
            "    *u8 replacement1 = __shaft_alloc_or_exit(450000);\n"
            "    return 42;\n}\n"
        )
        self.assertEqual(execution.returncode, 42, execution.stderr)

    def test_lifecycle_drop_method_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-no-manual-drop-") as directory:
            source = Path(directory) / "manual-drop.shaft"
            source.write_text(
                "struct Resource\n{\n    def drop(&mut self)\n    {\n    }\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 0;\n}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "--no-std", "--emit", "object", "-o", str(Path(directory) / "manual-drop.o"), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(compilation.returncode, 0)
            self.assertIn("manual lifecycle methods are not supported", compilation.stderr)


if __name__ == "__main__":
    unittest.main()
