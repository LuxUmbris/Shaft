import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
COMPILER = pathlib.Path(os.environ.get("SHAFTC", REPOSITORY / "build" / "shaftc"))


@unittest.skipUnless(COMPILER.is_file(), "shaftc build artifact is required")
class CompilerFlagsTests(unittest.TestCase):
    def run_compiler(self, *arguments):
        return subprocess.run(
            [str(COMPILER), *arguments],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_check_only_rejects_backend_unsupported_binary_operations(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "unsupported-binary.shaft"
            source.write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { 3.0 % 2.0; return 0; }\n", encoding="utf-8")
            result = subprocess.run([str(COMPILER), "--no-std", "--check-only", str(source)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Invalid operation", result.stdout + result.stderr)

    def test_check_only_rejects_unsupported_c_abi_aggregate_returns(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "c-abi-aggregate-return.shaft"
            source.write_text("cdef impossible() -> ?i32 { return 1; }\n", encoding="utf-8")
            result = subprocess.run([str(COMPILER), "--no-std", "--check-only", str(source)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("C-ABI functions cannot return optional or array values", result.stdout + result.stderr)

    def test_check_only_rejects_unknown_custom_type(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "unknown-type.shaft"
            source.write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { Missing value; return 0; }\n", encoding="utf-8")
            result = subprocess.run([str(COMPILER), "--no-std", "--check-only", str(source)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unknown type 'Missing'", result.stdout + result.stderr)

    def test_no_std_allows_a_user_defined_printf_function(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "user-printf.shaft"
            binary = pathlib.Path(directory) / "user-printf"
            source.write_text(
                "cdef printf(i32 value) -> i32 { return value + 1; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { return printf(41); }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_cstr_literal_is_lowered_for_a_c_declaration_parameter(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "cstr-literal.shaft"
            binary = pathlib.Path(directory) / "cstr-literal"
            source.write_text(
                "cdef first_byte(cstr text) -> i32 { return text.data[0]; }\n"
                "def main() { exit(first_byte(\"A\")); }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, ord("A"))

    def test_cdec_cstr_literal_compiles_to_an_object_without_an_llvm_cast_error(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "external-cstr-literal.shaft"
            artifact = pathlib.Path(directory) / "external-cstr-literal.o"
            source.write_text(
                "cdec consume(cstr text);\n"
                "def main() { consume(\"test\"); }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--emit", "object", "-o", str(artifact), str(source))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(artifact.is_file())

    def test_ellipsis_inclusive_range_executes_in_foreach(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "ellipsis-range.shaft"
            binary = pathlib.Path(directory) / "ellipsis-range"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut i64 total = 0;\n"
                "    foreach (i64 value : 1...6)\n    {\n"
                "        total = total + value;\n"
                "    }\n"
                "    return total;\n}\n",
                encoding="utf-8",
            )
            result = subprocess.run([str(COMPILER), "--no-std", "-o", str(binary), str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 21)

    def test_cdef_rejects_returning_a_fresh_owned_allocation_without_transfer_abi(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "owned-return.shaft"
            source.write_text(
                "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
                "cdef make() -> *u8\n{\n"
                "    return __shaft_alloc_or_exit(1);\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("owned allocation", result.stderr)

    def test_vector_accepts_aggregate_element_types(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "vector-aggregate.shaft"
            binary = pathlib.Path(directory) / "vector-aggregate"
            source.write_text(
                "struct Cell\n{\n    mut i64 value;\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    Vector<Cell> cells;\n"
                "    return 42;\n}\n",
                encoding="utf-8",
            )
            result = subprocess.run([str(COMPILER), "-o", str(binary), str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_version_prints_identity_without_an_input_file(self):
        result = self.run_compiler("--version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "shaftc alpha-0.1-bootstrap\n")

    def test_check_only_checks_without_creating_an_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "checked.shaft"
            source.write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 0;\n}\n")
            result = self.run_compiler("--check-only", str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((pathlib.Path(directory) / "checked").exists())
            self.assertFalse((pathlib.Path(directory) / "checked.ll").exists())

    def test_native_reference_read_dereferences_the_borrowed_storage(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "reference-read.shaft"
            binary = pathlib.Path(directory) / "reference-read"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut i32 value = 42;\n"
                "    &i32 view = ref value;\n"
                "    return *view;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_fresh_allocation_tunnel_transfers_cleanup_to_caller(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "fresh-owned-tunnel.shaft"
            binary = pathlib.Path(directory) / "fresh-owned-tunnel"
            source.write_text(
                "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
                "def produce() -> *u8 output {\n"
                "    tunnel __shaft_alloc_or_exit(900000) -> *u8 output;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    { reserve *u8 value = produce(); }\n"
                "    __shaft_alloc_or_exit(900000);\n"
                "    return 42;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_check_only_rejects_dynamic_move_from_named_runtime_array(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "runtime-array-dynamic-move.shaft"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    u64 count = 1;\n"
                "    mut *u8[count] values;\n"
                "    u64 offset = 0;\n"
                "    *u8 value = move values[offset];\n"
                "    return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("dynamic runtime-array index", result.stdout + result.stderr)

    def test_check_only_rejects_runtime_array_fields_without_local_count(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "runtime-array-field.shaft"
            source.write_text(
                "struct Invalid {\n"
                "    mut *u8[count] values;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("runtime array length", result.stdout + result.stderr)

    def test_check_only_rejects_signed_named_runtime_array_length(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "signed-runtime-array-length.shaft"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    i32 count = 2;\n"
                "    i32[count] values;\n"
                "    return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("runtime array length", result.stdout + result.stderr)

    def test_export_wraps_a_native_declaration_without_changing_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "export.shaft"
            binary = pathlib.Path(directory) / "export"
            source.write_text(
                "export cdef answer() -> i32 { return 42; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { return answer(); }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_unbound_multi_output_call_consumes_visible_reserves(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "unbound-multi-tunnel.shaft"
            binary = pathlib.Path(directory) / "unbound-multi-tunnel"
            source.write_text(
                "def pair() -> i32 first, -> i32 second {\n"
                "    tunnel 1 -> i32 first;\n"
                "    tunnel 2 -> i32 second;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    reserve i32 first;\n"
                "    reserve i32 second;\n"
                "    { pair(); }\n"
                "    return first * 10 + second;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 12)

    def test_inner_reservation_expires_and_outer_slot_is_restored(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "scoped-reservation.shaft"
            binary = pathlib.Path(directory) / "scoped-reservation"
            source.write_text(
                "def produce() -> i32 result, -> i32 other { tunnel 3 -> i32 result; tunnel 4 -> i32 other; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "  reserve i32 result; reserve i32 other; { reserve i32 result; }\n"
                "  produce(); return result * 10 + other;\n}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 34)

    def test_unbound_single_output_call_consumes_named_reservation(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "unbound-single-tunnel.shaft"
            binary = pathlib.Path(directory) / "unbound-single-tunnel"
            source.write_text(
                "def produce() -> i32 result { tunnel 3 -> i32 result; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "  reserve i32 result; produce(); return result;\n}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 3)

    def test_multi_output_tunnel_materializes_optional_bindings(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "optional-multi-tunnel.shaft"
            binary = pathlib.Path(directory) / "optional-multi-tunnel"
            source.write_text(
                "def pair() ?-> i32 first, -> i32 second { tunnel 4 -> i32 first; tunnel 2 -> i32 second; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "  reserve ?i32 first, i32 second = pair();\n"
                "  valid first { return second; }\n"
                "  return 1;\n}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 2)

    def test_standalone_multiple_optional_tunnel_outputs_preserve_each_presence(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "standalone-optionals.shaft"
            binary = pathlib.Path(directory) / "standalone-optionals"
            source.write_text(
                "def pair() ?-> i32 first, ?-> i32 second {\n"
                "    tunnel 5 -> i32 first;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    reserve ?i32 first;\n"
                "    reserve ?i32 second;\n"
                "    pair();\n"
                "    valid first {\n"
                "        valid second { return 0; }\n"
                "        return 5;\n"
                "    }\n"
                "    return 0;\n}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 5)

    def test_multi_output_tunnel_tracks_each_optional_presence_independently(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "two-optional-tunnel.shaft"
            binary = pathlib.Path(directory) / "two-optional-tunnel"
            source.write_text(
                "def pair() ?-> i32 first, ?-> i32 second { tunnel 5 -> i32 first; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "  reserve ?i32 first, ?i32 second = pair();\n"
                "  valid first { valid second { return 1; } return first; }\n"
                "  return 2;\n}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 5)

    def test_unbound_optional_multi_output_preserves_absence(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "unbound-optional-absent.shaft"
            binary = pathlib.Path(directory) / "unbound-optional-absent"
            source.write_text(
                "def pair() ?-> i32 first, -> i32 second { tunnel 2 -> i32 second; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "  reserve ?i32 first; reserve i32 second; { pair(); }\n"
                "  valid first { return 1; }\n"
                "  return second;\n}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 2)

    def test_multi_output_tunnel_binds_each_result_once(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "multi-tunnel.shaft"
            binary = pathlib.Path(directory) / "multi-tunnel"
            source.write_text(
                "def pair() -> i32 first, -> i32 second {\n"
                "    tunnel 1 -> i32 first;\n"
                "    tunnel 2 -> i32 second;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    reserve i32 first, i32 second = pair();\n"
                "    return first * 10 + second;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 12)

    def test_invalid_string_hex_escape_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "invalid-string-escape.shaft"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                + r'    *i8 text = "\xQ0";' + "\n"
                + "    return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("invalid hexadecimal string escape", result.stderr)

    def test_string_hex_and_unicode_escapes_lower_to_utf8(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "string-escapes.shaft"
            binary = pathlib.Path(directory) / "string-escapes"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                + r'    *i8 text = "x\u002A\x2A";' + "\n"
                + "    return text[1] + text[2] - 42;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_pointer_minus_numeric_offset_addresses_typed_elements(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "pointer-negative-offset.shaft"
            binary = pathlib.Path(directory) / "pointer-negative-offset"
            source.write_text(
                "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    *i32 values = __shaft_alloc_or_exit(12);\n"
                "    *(values + 1) = 42;\n"
                "    return *(values + 2 - 1);\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_pointer_plus_numeric_offset_addresses_typed_elements(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "pointer-offset.shaft"
            binary = pathlib.Path(directory) / "pointer-offset"
            source.write_text(
                "cdec __shaft_alloc_or_exit(u64 count) -> *u8;\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    *i32 values = __shaft_alloc_or_exit(8);\n"
                "    *(values + 1) = 42;\n"
                "    return *(values + 1);\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_mutable_self_receiver_updates_the_original_object(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "mut-self.shaft"
            binary = pathlib.Path(directory) / "mut-self"
            source.write_text(
                "class Counter {\n"
                "    mut i32 value;\n"
                "    def increment(&mut self) -> i32 result {\n"
                "        self.value = self.value + 1;\n"
                "        tunnel self.value -> i32 result;\n"
                "    }\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    mut Counter counter;\n"
                "    counter.value = 41;\n"
                "    return counter.increment();\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_value_self_receiver_executes_with_a_copied_object(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "value-self.shaft"
            binary = pathlib.Path(directory) / "value-self"
            source.write_text(
                "class Box {\n"
                "    mut i32 value;\n"
                "    def read(self) -> i32 result { tunnel self.value -> i32 result; }\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    mut Box box;\n"
                "    box.value = 42;\n"
                "    return box.read();\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_derived_class_inherits_index_and_init_backing_field(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "inherited-index-init.shaft"
            binary = pathlib.Path(directory) / "inherited-index-init"
            source.write_text(
                "cdec __shaft_alloc(u64 count) -> *u8;\n"
                "class Base {\n"
                "    mut *i32 data;\n"
                "    index data;\n"
                "    init data;\n"
                "}\n"
                "class Derived : Base { mut i32 tag; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    mut Derived values = {40, 0};\n"
                "    values[1] = 2;\n"
                "    return values[0] + values[1];\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_inherited_method_checks_explicit_arguments_and_executes(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "inherited-method-arguments.shaft"
            binary = pathlib.Path(directory) / "inherited-method-arguments"
            source.write_text(
                "class Base {\n"
                "    mut i32 left;\n"
                "    def add(&self, i32 amount) -> i32 result {\n"
                "        tunnel self.left + amount -> i32 result;\n"
                "    }\n"
                "}\n"
                "class Derived : Base { mut i32 right; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    mut Derived value = {20, 20};\n"
                "    return value.add(2) + value.right;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_inherited_method_uses_self_receiver_and_base_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "inherited-method.shaft"
            binary = pathlib.Path(directory) / "inherited-method"
            source.write_text(
                "class Base {\n"
                "    mut i32 left;\n"
                "    def get(&self) -> i32 result { tunnel self.left -> i32 result; }\n"
                "}\n"
                "class Pair : Base { mut i32 right; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    mut Pair value;\n"
                "    value.left = 42;\n"
                "    return value.get();\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_class_inheritance_flattens_base_fields_for_native_access(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "class-inheritance.shaft"
            binary = pathlib.Path(directory) / "class-inheritance"
            source.write_text(
                "class Base { mut i32 left; }\n"
                "class Pair : Base { mut i32 right; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "  mut Pair pair; pair.left = 20; pair.right = 22; return pair.left + pair.right;\n}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_logical_operators_short_circuit_rhs_side_effects(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "short-circuit.shaft"
            binary = pathlib.Path(directory) / "short-circuit"
            source.write_text(
                "cdef bump(*i32 value) -> bool { value[0] = value[0] + 1; return true; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "  mut i32 count = 0;\n"
                "  if (false && bump(&count)) { count = 99; }\n"
                "  if (true || bump(&count)) { }\n"
                "  return count;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)

    def test_native_for_loop_runs_initializer_condition_post_and_body(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "for-loop.shaft"
            binary = pathlib.Path(directory) / "for-loop"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut i32 total = 0;\n"
                "    for (mut i32 cursor = 0; cursor < 4; cursor += 1)\n"
                "    {\n"
                "        total += cursor;\n"
                "    }\n"
                "    return total;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 6)

    def test_native_match_selects_case_and_default_bodies(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "match.shaft"
            binary = pathlib.Path(directory) / "match"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    i32 value = 2;\n"
                "    match (value)\n"
                "    {\n"
                "        case 1 { return 1; }\n"
                "        case 2 { return 42; }\n"
                "        default { return 3; }\n"
                "    }\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_native_positional_struct_literal_initializes_fields_in_order(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "struct-literal.shaft"
            binary = pathlib.Path(directory) / "struct-literal"
            source.write_text(
                "struct Pair { i32 first; i32 second; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    Pair pair = { 17, 25 };\n"
                "    return pair.first + pair.second;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_native_foreach_visits_named_runtime_array_elements(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "runtime-foreach.shaft"
            binary = pathlib.Path(directory) / "runtime-foreach"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "  mut u64 count = 3; mut i32[count] values;\n"
                "  values[0] = 10; values[1] = 20; values[2] = 12;\n"
                "  mut i32 total = 0; foreach (i32 value : values) { total = total + value; }\n"
                "  return total;\n}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_native_foreach_visits_each_fixed_array_element(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "foreach.shaft"
            binary = pathlib.Path(directory) / "foreach"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut i32[3] values;\n"
                "    values[0] = 10;\n"
                "    values[1] = 14;\n"
                "    values[2] = 18;\n"
                "    mut i32 total = 0;\n"
                "    foreach (i32 item : values) { total += item; }\n"
                "    return total;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_native_reserve_tunnel_binding_writes_the_bound_output_slot(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "tunnel-binding.shaft"
            binary = pathlib.Path(directory) / "tunnel-binding"
            source.write_text(
                "def add(i32 left, i32 right) -> i32 sum\n{\n"
                "    reserve mut i32 output <- sum;\n"
                "    output = left + right;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    return add(19, 23);\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_native_struct_literal_can_be_assigned_to_existing_storage(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "struct-literal-assignment.shaft"
            binary = pathlib.Path(directory) / "struct-literal-assignment"
            source.write_text(
                "struct Pair { i32 first; i32 second; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut Pair pair;\n"
                "    pair = { 17, 25 };\n"
                "    return pair.first + pair.second;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_shaft_declaration_can_precede_its_matching_definition(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "declaration.shaft"
            binary = pathlib.Path(directory) / "declaration"
            source.write_text(
                "dec increment(i32 value) -> i32 output;\n"
                "def increment(i32 value) -> i32 output\n{\n"
                "    tunnel value + 1 -> i32 output;\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    return increment(41);\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_control_flow_requires_boolean_conditions(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "nonboolean-condition.shaft"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    if (argc) { return 1; }\n"
                "    return 0;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Boolean condition", result.stderr)

    def test_usize_uses_target_pointer_width_in_llvm_ir(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "usize-32.shaft"
            output = pathlib.Path(directory) / "usize-32.ll"
            source.write_text(
                "cdef id(usize value) -> usize { return value; }\n",
                encoding="utf-8",
            )
            result = self.run_compiler(
                "--no-std", "--target", "i386-unknown-linux-gnu", "--emit", "llvm", "-o", str(output), str(source)
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("define i32 @id(i32", output.read_text(encoding="utf-8"))

    def test_usize_high_bit_values_use_unsigned_ordering_and_arithmetic(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "usize-unsigned.shaft"
            binary = pathlib.Path(directory) / "usize-unsigned"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    usize maximum = 18446744073709551615;\n"
                "    if (maximum <= 0 || maximum < 1) { return 1; }\n"
                "    if (maximum / 2 != 9223372036854775807) { return 2; }\n"
                "    if (maximum % 2 != 1) { return 3; }\n"
                "    return 42;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_target_accepts_an_llvm_target_triple(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "targeted.shaft"
            source.write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 0;\n}\n")
            result = self.run_compiler("--target", "x86_64-unknown-linux-gnu", "--check-only", str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            output = pathlib.Path(directory) / "targeted.ll"
            result = self.run_compiler("--target=x86_64-unknown-linux-gnu", "--emit", "llvm", "-o", str(output), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('target triple = "x86_64-unknown-linux-gnu"', output.read_text())

    def test_optimization_level_accepts_standard_spelling(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "optimized.shaft"
            output = pathlib.Path(directory) / "optimized.ll"
            source.write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return argc + 1;\n}\n")
            for level in ("-O0", "-O1", "-O2", "-O3"):
                result = self.run_compiler(level, "--emit", "llvm", "-o", str(output), str(source))
                self.assertEqual(result.returncode, 0, level + ": " + result.stderr)
            result = self.run_compiler("-O9", "--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("optimization level", result.stderr)

    def test_default_optimization_matches_o2_stack_promotion(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "default-optimization.shaft"
            output = pathlib.Path(directory) / "default-optimization.ll"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    i32 value = argc + 1;\n"
                "    return value;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "--emit", "llvm", "-o", str(output), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("alloca", output.read_text())

    def test_native_cpu_option_accepts_native_code_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "native.shaft"
            output = pathlib.Path(directory) / "native.ll"
            source.write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return argc;\n}\n")
            result = self.run_compiler("--native", "--emit", "llvm", "-o", str(output), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.is_file())

    def test_native_cpu_rejects_an_explicit_cross_target(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "cross-native.shaft"
            output = pathlib.Path(directory) / "cross-native.ll"
            source.write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return argc;\n}\n")
            result = self.run_compiler(
                "--native", "--target", "aarch64-unknown-linux-gnu", "--emit", "llvm", "-o", str(output), str(source)
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot be combined", result.stderr)
            self.assertFalse(output.exists())

    def test_namespaced_enums_keep_distinct_member_values(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "namespaced-enums.shaft"
            binary = pathlib.Path(directory) / "namespaced-enums"
            source.write_text(
                "namespace A\n{\n    enum Color { Red, Blue }\n}\n"
                "namespace B\n{\n    enum Color { Green, Blue, Red }\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return A::Color::Red;\n}\n"
            )
            result = self.run_compiler("-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)

    def test_normal_main_rejects_non_string_array_parameter(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "main-u64.shaft"
            source.write_text("def main(u64 value)\n{\n}\n")
            result = self.run_compiler("--emit", "llvm", "-o", str(pathlib.Path(directory) / "main-u64.ll"), str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("main must take no arguments or String[] args", result.stderr)

    def test_normal_main_rejects_tunnel_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "main-tunnel.shaft"
            source.write_text("def main(String[] args) -> i32 result\n{\n    tunnel 0 -> i32 result;\n}\n")
            result = self.run_compiler("--emit", "llvm", "-o", str(pathlib.Path(directory) / "main-tunnel.ll"), str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("main cannot declare tunnel outputs", result.stderr)

    def test_normal_main_without_arguments_remains_supported(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "main-empty.shaft"
            binary = pathlib.Path(directory) / "main-empty"
            source.write_text("def main()\n{\n}\n")
            result = self.run_compiler("-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)

    def test_o3_native_binary_preserves_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "optimized-native.shaft"
            binary = pathlib.Path(directory) / "optimized-native"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return argc + 40;\n}\n"
            )
            result = self.run_compiler("-O3", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            execution = subprocess.run([str(binary)], check=False)
            self.assertEqual(execution.returncode, 41)

    def test_verbose_reports_frontend_stages(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "verbose.shaft"
            source.write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 0;\n}\n")
            result = self.run_compiler("--verbose", "--check-only", str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("shaftc: reading", result.stderr)
            self.assertIn("shaftc: lexing and parsing", result.stderr)
            self.assertIn("shaftc: checking", result.stderr)
            self.assertIn("shaftc: check completed", result.stderr)

    def test_enums_honor_underlying_type_and_explicit_member_values_natively(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "enum-values.shaft"
            binary = pathlib.Path(directory) / "enum-values"
            source.write_text(
                "enum Status : u8 { Cold = 3, Warm = Cold + 1, Hot = Warm + 5 }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    return Status::Warm + Status::Hot;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 13)

    def test_check_only_rejects_incompatible_standalone_reservation_type(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "standalone-reservation-type.shaft"
            source.write_text(
                "def produce() -> i32 result { tunnel 3 -> i32 result; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { reserve u8 result; produce(); return 0; }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("standalone reserve binding type does not match tunnel output", result.stderr)

    def test_check_only_rejects_partial_standalone_multi_output_reservations(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "partial-multi-reservation.shaft"
            source.write_text(
                "def pair() -> i32 first, -> i32 second { tunnel 1 -> i32 first; tunnel 2 -> i32 second; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { reserve i32 first; pair(); return first; }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unbound multi-output call requires one compatible reserve declaration per tunnel slot", result.stderr)

    def test_check_only_rejects_consuming_a_standalone_reservation_twice(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "duplicate-reservation-consumption.shaft"
            source.write_text(
                "def produce() -> i32 result { tunnel 3 -> i32 result; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    reserve i32 result;\n"
                "    produce();\n"
                "    produce();\n"
                "    return result;\n}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("tunnel reservation 'result' was already consumed", result.stderr)

    def test_check_only_rejects_multi_reserve_same_scope_redeclaration(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "duplicate-multi-reserve.shaft"
            source.write_text(
                "def pair() -> i32 first, -> i32 second {\n"
                "    tunnel 1 -> i32 first;\n"
                "    tunnel 2 -> i32 second;\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    i32 first = 0;\n"
                "    reserve i32 first, i32 second = pair();\n"
                "    return 0;\n}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Redeclaration of variable 'first'", result.stderr)

    def test_check_only_rejects_multi_reserve_slot_count_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "bad-multi-reserve-count.shaft"
            source.write_text(
                "def pair() -> i32 first, -> i32 second {\n"
                "    tunnel 1 -> i32 first;\n"
                "    tunnel 2 -> i32 second;\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    reserve i32 first = pair();\n"
                "    return 0;\n}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("reserve binding count does not match tunnel outputs", result.stderr)

    def test_check_only_rejects_multi_reserve_slot_type_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "bad-multi-reserve.shaft"
            source.write_text(
                "def pair() -> i32 first, -> i32 second {\n"
                "    tunnel 1 -> i32 first;\n"
                "    tunnel 2 -> i32 second;\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 {\n"
                "    reserve i32 first, u8 second = pair();\n"
                "    return 0;\n}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("multi-result reserve binding type does not match", result.stderr)

    def test_enum_rejects_signed_backing_underflow(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "enum-signed-underflow.shaft"
            source.write_text("enum SignedByte : i8 { Underflow = -129 }\n", encoding="utf-8")
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("enum member value does not fit backing type", result.stderr)

    def test_enum_rejects_implicit_increment_past_backing_range(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "enum-implicit-overflow.shaft"
            source.write_text("enum Byte : u8 { Maximum = 255, Overflow }\n", encoding="utf-8")
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("enum member value does not fit backing type", result.stderr)

    def test_enum_rejects_non_integer_backing_type(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "enum-float-backing.shaft"
            source.write_text("enum Invalid : f32 { Value = 1 }\n", encoding="utf-8")
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("enum backing type must be an integer", result.stderr)

    def test_enum_rejects_signed_backing_overflow(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "enum-signed-overflow.shaft"
            source.write_text("enum SignedByte : i8 { Overflow = 128 }\n", encoding="utf-8")
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("enum member value does not fit backing type", result.stderr)

    def test_enum_rejects_value_outside_declared_backing_range(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "enum-overflow.shaft"
            source.write_text("enum Byte : u8 { Overflow = 256 }\n")
            result = self.run_compiler("--no-std", "--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("does not fit backing type", result.stderr)

    def test_tunnel_restatement_recursively_matches_pointer_payload_type(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "nested-restatement-mismatch.shaft"
            source.write_text(
                "def produce() -> **i32 output {\n"
                "    **i32 value;\n"
                "    tunnel value -> **u8 output;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("tunnel output type restatement does not match declared slot type", result.stderr)

    def test_tunnel_restatement_must_match_declared_slot_type(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "tunnel-restatement.shaft"
            source.write_text(
                "def produce() -> i32 output { tunnel 1 -> u8 output; }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("tunnel output type restatement", result.stderr)

    def test_tunnel_restatement_must_match_declared_optional_payload_type(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "tunnel-reference-restatement.shaft"
            source.write_text(
                "def produce() -> ?i32 output { tunnel 1 -> ?u8 output; }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "--check-only", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("tunnel output type restatement", result.stderr)

    def test_aligned_struct_requests_its_declared_storage_alignment(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "aligned.shaft"
            output = pathlib.Path(directory) / "aligned.ll"
            binary = pathlib.Path(directory) / "aligned"
            source.write_text(
                "align 16 struct Lane { mut u8 value; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut Lane lane;\n"
                "    lane.value = 42;\n"
                "    return lane.value;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-O0", "--emit", "llvm", "-o", str(output), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertRegex(output.read_text(), r"alloca %Lane, align 16")
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_valid_block_mutates_a_mutable_optional_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "valid-mutable-optional.shaft"
            binary = pathlib.Path(directory) / "valid-mutable-optional"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut ?i64 value = 1;\n"
                "    valid value { value += 41; }\n"
                "    valid value { return value; }\n"
                "    return 1;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_valid_block_keeps_its_payload_binding_under_shadowing(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "valid-shadowed-optional.shaft"
            binary = pathlib.Path(directory) / "valid-shadowed-optional"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut ?i64 outer = 1;\n"
                "    mut i64 result = 0;\n"
                "    valid outer { mut ?i64 outer; outer = 42; valid outer { result = outer; } }\n"
                "    return result;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_optional_value_validity_and_payload_are_lowered_natively(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "optional.shaft"
            binary = pathlib.Path(directory) / "optional"
            source.write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    ?u8 present = 42;\n"
                "    ?u8 absent;\n"
                "    ?u8 copied = absent;\n"
                "    mut u8 output = 1;\n"
                "    valid present { output = present; } else { output = 2; }\n"
                "    valid copied { output = 3; } else { output = output; }\n"
                "    return output;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_generic_struct_instantiations_have_distinct_concrete_layouts(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "generic-layout.shaft"
            output = pathlib.Path(directory) / "generic-layout.ll"
            binary = pathlib.Path(directory) / "generic-layout"
            source.write_text(
                "struct<T> Pair { mut T first; mut T second; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n"
                "    mut Pair<u8> small;\n"
                "    mut Pair<u64> large;\n"
                "    small.first = 2;\n"
                "    large.first = 40;\n"
                "    return small.first + large.first;\n"
                "}\n"
            )
            result = self.run_compiler("--no-std", "-O0", "--emit", "llvm", "-o", str(output), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            ir = output.read_text()
            self.assertRegex(ir, r"%Pair\.u8 = type \{ i8, i8 \}")
            self.assertRegex(ir, r"%Pair\.u64 = type \{ i64, i64 \}")
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_generic_function_instantiation_emits_concrete_symbol(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "generic-function.shaft"
            output = pathlib.Path(directory) / "generic-function.ll"
            binary = pathlib.Path(directory) / "generic-function"
            source.write_text(
                "def identity(T value)<T> -> T answer { tunnel value -> T answer; }\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { return identity::<u8>(42); }\n"
            )
            result = self.run_compiler("--no-std", "-O0", "--emit", "llvm", "-o", str(output), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("define void @identity.u8(i8", output.read_text())
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)
    def test_build_uses_default_shaft_build_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            project = pathlib.Path(directory)
            (project / "main.shaft").write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 42;\n}\n"
            )
            (project / "Shaft.build").write_text(
                "[package]\n"
                "name = \"configured-app\"\n"
                "version = \"0.1.0\"\n\n"
                "[build]\n"
                "entry = \"main.shaft\"\n"
                "output = \"artifacts/configured-app\"\n"
                "emit = \"binary\"\n"
                "optimization = \"O0\"\n"
                "no_std = true\n"
            )
            result = subprocess.run(
                [str(COMPILER), "--build"], cwd=project, text=True, capture_output=True, check=False
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            binary = project / "artifacts" / "configured-app"
            self.assertTrue(binary.is_file())
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_build_configures_hosted_c_library_linkage(self):
        clang = shutil.which("clang")
        archiver = shutil.which("ar")
        if not clang or not archiver:
            self.skipTest("clang and ar are required for build-linkage validation")
        with tempfile.TemporaryDirectory(prefix="shaftc-build-linkage-") as directory:
            project = pathlib.Path(directory)
            library_source = project / "answer.c"
            library_object = project / "answer.o"
            library_archive = project / "libanswer.a"
            source = project / "main.shaft"
            binary = project / "artifacts" / "configured-app"
            library_source.write_text("int answer_twice(int value) { return value * 2; }\n", encoding="utf-8")
            source.write_text(
                "cdec answer_twice(i32 value) -> i32;\n"
                "cdef main() -> i32 { return answer_twice(21); }\n",
                encoding="utf-8",
            )
            compiled_library = subprocess.run(
                [clang, "-c", str(library_source), "-o", str(library_object)], text=True, capture_output=True, check=False
            )
            self.assertEqual(compiled_library.returncode, 0, compiled_library.stdout + compiled_library.stderr)
            archived_library = subprocess.run(
                [archiver, "rcs", str(library_archive), str(library_object)], text=True, capture_output=True, check=False
            )
            self.assertEqual(archived_library.returncode, 0, archived_library.stdout + archived_library.stderr)
            (project / "Shaft.build").write_text(
                "[build]\n"
                "entry = \"main.shaft\"\n"
                "output = \"artifacts/configured-app\"\n"
                "no_std = true\n"
                "hosted = true\n"
                "link_directories = [\".\"]\n"
                "links = [\"answer\"]\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [str(COMPILER), "--build"], cwd=project, text=True, capture_output=True, check=False
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(binary.is_file())
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_build_accepts_an_explicit_config_and_emits_requested_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            project = pathlib.Path(directory)
            config_dir = project / "config"
            source_dir = project / "source"
            config_dir.mkdir()
            source_dir.mkdir()
            (source_dir / "entry.shaft").write_text(
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 0;\n}\n"
            )
            build_file = config_dir / "Project.build"
            build_file.write_text(
                "# Config-relative paths must work from any working directory.\n"
                "[package]\nname = \"project\"\nversion = \"1.2.3\"\n\n"
                "[build]\n"
                "entry = \"../source/entry.shaft\"\n"
                "output = \"generated/project.ll\"\n"
                "emit = \"llvm\"\n"
                "optimization = \"3\"\n"
                "no_std = true\n"
                "verbose = true\n"
            )
            result = self.run_compiler("--build", str(build_file))
            self.assertEqual(result.returncode, 0, result.stderr)
            output = config_dir / "generated" / "project.ll"
            self.assertTrue(output.is_file())
            self.assertIn("@__shaft_entry(", output.read_text())
            self.assertIn("shaftc: generating LLVM IR", result.stderr)

    def test_build_rejects_unknown_configuration_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            project = pathlib.Path(directory)
            (project / "main.shaft").write_text("cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { return 0; }\n")
            build_file = project / "Project.build"
            build_file.write_text("[build]\nentry = \"main.shaft\"\noptimisation = \"O2\"\n")
            result = self.run_compiler("--build", str(build_file))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown configuration key", result.stderr)
    def test_build_entry_compiles_its_relative_imports_as_project_modules(self):
        with tempfile.TemporaryDirectory() as directory:
            project = pathlib.Path(directory)
            (project / "app").mkdir()
            (project / "lib").mkdir()
            (project / "lib" / "answer.shaft").write_text(
                "cdef answer() -> i32 { return 42; }\n"
            )
            (project / "app" / "main.shaft").write_text(
                "import \"../lib/answer.shaft\";\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32 { return answer(); }\n"
            )
            (project / "Shaft.build").write_text(
                "[build]\nentry = \"app/main.shaft\"\noutput = \"build/app\"\nno_std = true\n"
            )
            result = subprocess.run(
                [str(COMPILER), "--build"], cwd=project, text=True, capture_output=True, check=False
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(project / "build" / "app")], check=False).returncode, 42)
    def test_build_reports_import_cycles_without_crashing(self):
        with tempfile.TemporaryDirectory() as directory:
            project = pathlib.Path(directory)
            (project / "a.shaft").write_text("import \"b.shaft\";\n")
            (project / "b.shaft").write_text("import \"a.shaft\";\n")
            (project / "Shaft.build").write_text("[build]\nentry = \"a.shaft\"\nno_std = true\n")
            result = subprocess.run(
                [str(COMPILER), "--build"], cwd=project, text=True, capture_output=True, check=False
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cyclic import", result.stderr)

    def test_build_reports_missing_imports_without_crashing(self):
        with tempfile.TemporaryDirectory() as directory:
            project = pathlib.Path(directory)
            (project / "main.shaft").write_text("import \"missing.shaft\";\n")
            (project / "Shaft.build").write_text("[build]\nentry = \"main.shaft\"\nno_std = true\n")
            result = subprocess.run(
                [str(COMPILER), "--build"], cwd=project, text=True, capture_output=True, check=False
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("failed to resolve source module", result.stderr)
    def test_optional_tunnel_call_preserves_a_present_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "optional-tunnel-present.shaft"
            binary = pathlib.Path(directory) / "optional-tunnel-present"
            source.write_text(
                "def maybe(i32 value) ?-> i32 result\n"
                "{\n"
                "    if (value != 0) { tunnel 42 -> i32 result; }\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    ?i32 result = maybe(1);\n"
                "    valid result { return result; } else { return 1; }\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_optional_tunnel_call_preserves_an_absent_presence_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "optional-tunnel-call.shaft"
            binary = pathlib.Path(directory) / "optional-tunnel-call"
            source.write_text(
                "def maybe(i32 value) ?-> i32 result\n"
                "{\n"
                "    if (value != 0) { tunnel 42 -> i32 result; }\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    ?i32 result = maybe(0);\n"
                "    valid result { return 1; } else { return 42; }\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_match_cases_can_each_fill_the_same_required_tunnel_slot(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "match-tunnel.shaft"
            binary = pathlib.Path(directory) / "match-tunnel"
            source.write_text(
                "def choose(i32 value) -> i32 result\n"
                "{\n"
                "    match (value)\n"
                "    {\n"
                "        case 1 { tunnel 11 -> i32 result; }\n"
                "        case 2 { tunnel 17 -> i32 result; }\n"
                "        default { tunnel 14 -> i32 result; }\n"
                "    }\n"
                "}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n"
                "{\n"
                "    mut i32 total = 0;\n"
                "    { reserve i32 result; choose(1); total += result; }\n"
                "    { reserve i32 result; choose(2); total += result; }\n"
                "    { reserve i32 result; choose(9); total += result; }\n"
                "    return total;\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "-o", str(binary), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_match_without_default_cannot_guarantee_a_required_tunnel_slot(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "incomplete-match-tunnel.shaft"
            source.write_text(
                "def choose(i32 value) -> i32 result\n"
                "{\n"
                "    match (value) { case 1 { tunnel 11 -> i32 result; } }\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--check-only", "--no-std", str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Function exiting without populating required tunnel slot 'result'", result.stderr)

    def test_optional_tunnel_slot_can_remain_unfilled(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "optional-tunnel.shaft"
            output = pathlib.Path(directory) / "optional-tunnel.o"
            source.write_text(
                "def absent() ? -> i64 result\n{\n}\n"
                "cdef __shaft_entry(i32 argc, *i8 argv) -> i32\n{\n    return 42;\n}\n"
            )
            result = self.run_compiler("--no-std", "--emit", "object", "-o", str(output), str(source))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.is_file())

    def test_no_std_hosted_rejects_a_void_cdef_main(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-hosted-void-main-") as directory:
            source = pathlib.Path(directory) / "void-main.shaft"
            binary = pathlib.Path(directory) / "void-main"
            source.write_text(
                "cdef status() -> i32 { return 42; }\n"
                "cdef main() { status(); }\n",
                encoding="utf-8",
            )
            result = self.run_compiler("--no-std", "--hosted", "-O0", "-o", str(binary), str(source))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cdef main() -> i32", result.stdout + result.stderr)

    def test_hosted_accepts_an_explicit_host_target(self):
        llvm_config = shutil.which("llvm-config")
        if not llvm_config:
            self.skipTest("llvm-config is required to determine the host target")
        target = subprocess.run([llvm_config, "--host-target"], capture_output=True, text=True, check=True).stdout.strip()
        with tempfile.TemporaryDirectory(prefix="shaftc-hosted-host-target-") as directory:
            source = pathlib.Path(directory) / "host-target-main.shaft"
            binary = pathlib.Path(directory) / "host-target-main"
            source.write_text("cdef main() -> i32 { return 42; }\n", encoding="utf-8")
            result = self.run_compiler(
                "--no-std", "--hosted", "--target", target, "-o", str(binary), str(source)
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_hosted_linking_calls_a_function_from_a_static_c_library(self):
        clang = shutil.which("clang")
        archiver = shutil.which("ar")
        if not clang or not archiver:
            self.skipTest("clang and ar are required for C-library integration validation")
        with tempfile.TemporaryDirectory(prefix="shaftc-hosted-c-library-") as directory:
            project = pathlib.Path(directory)
            library_source = project / "answer.c"
            library_object = project / "answer.o"
            library_archive = project / "libanswer.a"
            source = project / "main.shaft"
            hosted_standard_source = project / "hosted-standard.shaft"
            binary = project / "main"
            hosted_standard_binary = project / "hosted-standard"
            library_source.write_text("int answer_twice(int value) { return value * 2; }\n", encoding="utf-8")
            source.write_text(
                "cdec answer_twice(i32 value) -> i32;\n"
                "cdef main() -> i32 { return answer_twice(21); }\n",
                encoding="utf-8",
            )
            compiled_library = subprocess.run(
                [clang, "-c", str(library_source), "-o", str(library_object)], text=True, capture_output=True, check=False
            )
            self.assertEqual(compiled_library.returncode, 0, compiled_library.stdout + compiled_library.stderr)
            archived_library = subprocess.run(
                [archiver, "rcs", str(library_archive), str(library_object)], text=True, capture_output=True, check=False
            )
            self.assertEqual(archived_library.returncode, 0, archived_library.stdout + archived_library.stderr)
            result = self.run_compiler(
                "--no-std", "--hosted", "--link-dir", str(project), "--link", "answer", "-o", str(binary), str(source)
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)
            hosted_standard_source.write_text(
                "cdec answer_twice(i32 value) -> i32;\n"
                "def main(String[] args) { exit(answer_twice(21)); }\n",
                encoding="utf-8",
            )
            hosted_standard_result = self.run_compiler(
                "--hosted", "--link-dir", str(project), "--link", "answer", "-o", str(hosted_standard_binary), str(hosted_standard_source)
            )
            self.assertEqual(hosted_standard_result.returncode, 0, hosted_standard_result.stdout + hosted_standard_result.stderr)
            self.assertEqual(subprocess.run([str(hosted_standard_binary)], check=False).returncode, 42)

    def test_short_l_links_a_library_visible_to_the_host_linker(self):
        clang = shutil.which("clang")
        archiver = shutil.which("ar")
        if not clang or not archiver:
            self.skipTest("clang and ar are required for short -l validation")
        with tempfile.TemporaryDirectory(prefix="shaftc-short-l-") as directory:
            project = pathlib.Path(directory)
            library_source = project / "answer.c"
            library_object = project / "answer.o"
            library_archive = project / "libanswer.a"
            source = project / "main.shaft"
            binary = project / "main"
            library_source.write_text("int answer(void) { return 42; }\n", encoding="utf-8")
            source.write_text(
                "cdec answer() -> i32;\n"
                "cdef main() -> i32 { return answer(); }\n",
                encoding="utf-8",
            )
            compiled = subprocess.run([clang, "-c", str(library_source), "-o", str(library_object)],
                                     text=True, capture_output=True, check=False)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            archived = subprocess.run([archiver, "rcs", str(library_archive), str(library_object)],
                                     text=True, capture_output=True, check=False)
            self.assertEqual(archived.returncode, 0, archived.stdout + archived.stderr)
            result = self.run_compiler(
                "--no-std", "--hosted", "--link-dir", str(project), "-lanswer",
                "-o", str(binary), str(source),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_raw_object_archive_and_bitcode_inputs_link_with_a_shaft_source(self):
        clang = shutil.which("clang")
        archiver = shutil.which("ar")
        if not clang or not archiver:
            self.skipTest("clang and ar are required for raw linker-input validation")
        with tempfile.TemporaryDirectory(prefix="shaftc-raw-link-inputs-") as directory:
            project = pathlib.Path(directory)
            object_source = project / "object.c"
            object_file = project / "object.o"
            archive_source = project / "archive.c"
            archive_object = project / "archive.o"
            archive_file = project / "archive.a"
            bitcode_source = project / "bitcode.c"
            bitcode_file = project / "bitcode.bc"
            llvm_source = project / "llvm.c"
            llvm_file = project / "llvm.ll"
            source = project / "main.shaft"
            binary = project / "main"
            object_source.write_text("int from_object(void) { return 10; }\n", encoding="utf-8")
            archive_source.write_text("int from_archive(void) { return 20; }\n", encoding="utf-8")
            bitcode_source.write_text("int from_bitcode(void) { return 5; }\n", encoding="utf-8")
            llvm_source.write_text("int from_llvm(void) { return 7; }\n", encoding="utf-8")
            source.write_text(
                "cdec from_object() -> i32;\n"
                "cdec from_archive() -> i32;\n"
                "cdec from_bitcode() -> i32;\n"
                "cdec from_llvm() -> i32;\n"
                "cdef main() -> i32 { return from_object() + from_archive() + from_bitcode() + from_llvm(); }\n",
                encoding="utf-8",
            )
            for command in (
                [clang, "-c", str(object_source), "-o", str(object_file)],
                [clang, "-c", str(archive_source), "-o", str(archive_object)],
                [clang, "-emit-llvm", "-c", str(bitcode_source), "-o", str(bitcode_file)],
                [clang, "-S", "-emit-llvm", str(llvm_source), "-o", str(llvm_file)],
            ):
                compiled = subprocess.run(command, text=True, capture_output=True, check=False)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            archived = subprocess.run([archiver, "rcs", str(archive_file), str(archive_object)],
                                     text=True, capture_output=True, check=False)
            self.assertEqual(archived.returncode, 0, archived.stdout + archived.stderr)
            result = self.run_compiler(
                "--no-std", "--hosted", "-o", str(binary), str(source),
                str(object_file), str(archive_file), str(bitcode_file), str(llvm_file),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 42)

    def test_native_linking_rejects_an_invalid_lld_override(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-native-lld-") as directory:
            source = pathlib.Path(directory) / "native-lld.shaft"
            binary = pathlib.Path(directory) / "native-lld"
            missing_lld = pathlib.Path(directory) / "missing-ld.lld"
            source.write_text("def main(String[] args) { exit(0); }\n", encoding="utf-8")
            result = subprocess.run(
                [str(COMPILER), "-o", str(binary), str(source)],
                text=True,
                capture_output=True,
                env=os.environ | {"SHAFT_LLD": str(missing_lld)},
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("SHAFT_LLD does not name an LLD executable", result.stdout + result.stderr)

    @unittest.skipUnless(os.environ.get("SHAFT_LLD"), "SHAFT_LLD is required for Linux cross-link validation")
    def test_cross_links_aarch64_linux_binary_with_lld(self):
        lld = pathlib.Path(os.environ["SHAFT_LLD"])
        if not lld.is_file():
            self.skipTest("SHAFT_LLD does not name an executable")
        with tempfile.TemporaryDirectory(prefix="shaftc-cross-lld-") as directory:
            source = pathlib.Path(directory) / "cross.shaft"
            binary = pathlib.Path(directory) / "cross-aarch64"
            source.write_text("def main(String[] args) { exit(0); }\n", encoding="utf-8")
            environment = os.environ | {"SHAFT_LLD": str(lld)}
            result = subprocess.run(
                [str(COMPILER), "--target", "aarch64-unknown-linux-gnu", "-o", str(binary), str(source)],
                text=True,
                capture_output=True,
                env=environment,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            file_result = subprocess.run(["file", str(binary)], text=True, capture_output=True, check=False)
            self.assertEqual(file_result.returncode, 0, file_result.stderr)
            self.assertIn("ARM aarch64", file_result.stdout)
            qemu = os.environ.get("SHAFT_QEMU_AARCH64")
            if qemu:
                execution = subprocess.run([qemu, str(binary)], text=True, capture_output=True, check=False)
                self.assertEqual(execution.returncode, 0, execution.stdout + execution.stderr)


if __name__ == "__main__":
    unittest.main()
