import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
SHAFTC = REPOSITORY / "build" / "shaftc"


@unittest.skipUnless(SHAFTC.is_file() and os.name == "posix", "requires Linux shaftc")
class StdStringConversionTests(unittest.TestCase):
    def test_from_int_and_from_float_produce_nul_terminated_decimal_strings(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-conversion-") as directory:
            work = Path(directory)
            source = work / "string-conversion.shaft"
            binary = work / "string-conversion"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String integer = String::from_int(-42);\n"
                "    if (integer.length != 3 || integer.data[0] != 45 || integer.data[1] != 52 || integer.data[2] != 50 || integer.data[3] != 0)\n"
                "    {\n"
                "        exit(1);\n"
                "    }\n"
                "    reserve String decimal = String::from_float(3.5);\n"
                "    if (decimal.length != 8 || decimal.data[0] != 51 || decimal.data[1] != 46 || decimal.data[2] != 53 || decimal.data[7] != 48 || decimal.data[8] != 0)\n"
                "    {\n"
                "        exit(2);\n"
                "    }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"")

    def test_string_parsers_return_numbers_and_reject_invalid_text(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-parsers-") as directory:
            work = Path(directory)
            source = work / "string-parsers.shaft"
            binary = work / "string-parsers"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String signedValue = String::from_int(-42);\n"
                "    reserve String unsignedValue = String::from_u64(18446744073709551615);\n"
                "    reserve String decimalValue = String::from_float(-3.5);\n"
                "    reserve mut str invalidText; invalidText.data = \"12x\"; invalidText.length = 3; reserve String invalidValue = String::from_str(invalidText);\n"
                "    { reserve bool parsed; reserve ?i64 value; signedValue.parse_i64(); valid value { if (!parsed || value != -42) { exit(1); } } }\n"
                "    { reserve bool parsed; reserve ?u64 value; unsignedValue.parse_u64(); valid value { if (!parsed || value != 18446744073709551615) { exit(2); } } }\n"
                "    { reserve bool parsed; reserve ?f64 value; decimalValue.parse_f64(); valid value { if (!parsed || value != -3.5) { exit(3); } } }\n"
                "    { reserve bool parsed; reserve ?i64 value; invalidValue.parse_i64(); if (parsed) { exit(4); } valid value { exit(5); } }\n"
                "    { reserve bool parsed; reserve ?u64 value; signedValue.parse_u64(); if (parsed) { exit(6); } valid value { exit(7); } }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_string_constructors_cover_primitives_and_str(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-constructors-") as directory:
            work = Path(directory)
            source = work / "constructors.shaft"
            binary = work / "constructors"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String i8Text = String::from_i8(-8); reserve String u8Text = String::from_u8(8);\n"
                "    reserve String i16Text = String::from_i16(-16); reserve String u16Text = String::from_u16(16);\n"
                "    reserve String i32Text = String::from_i32(-32); reserve String u32Text = String::from_u32(32);\n"
                "    reserve String i64Text = String::from_i64(-64); reserve String u64Text = String::from_u64(18446744073709551615);\n"
                "    usize size = 65; reserve String sizeText = String::from_usize(size);\n"
                "    reserve String f32Text = String::from_f32(1.5); reserve String f64Text = String::from_f64(-2.25);\n"
                "    reserve String boolText = String::from_bool(true); reserve String charText = String::from_char('Q');\n"
                "    reserve mut str name; name.data = \"Ada\"; name.length = 3; reserve String nameText = String::from_str(name);\n"
                "    printf(\"{String} {String} {String} {String} {String} {String} {String} {String} {String} {String} {String} {String} {String} {String}\\n\", i8Text, u8Text, i16Text, u16Text, i32Text, u32Text, i64Text, u64Text, sizeText, f32Text, f64Text, boolText, charText, nameText);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"-8 8 -16 16 -32 32 -64 18446744073709551615 65 1.500000 -2.250000 true Q Ada\n")

    def test_from_str_exits_cleanly_when_copy_allocation_is_exhausted(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-copy-exhaustion-") as directory:
            work = Path(directory)
            source = work / "string-copy-exhaustion.shaft"
            binary = work / "string-copy-exhaustion"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String source = String::with_capacity(700000);\n"
                "    reserve mut str view;\n"
                "    view.data = source.data; view.length = 700000;\n"
                "    reserve String copied = String::from_str(view);\n"
                "    exit(0);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-O0", "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 70, execution.stderr)

    def test_reverse_exits_cleanly_when_result_allocation_is_exhausted(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-reverse-exhaustion-") as directory:
            work = Path(directory)
            source = work / "string-reverse-exhaustion.shaft"
            binary = work / "string-reverse-exhaustion"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut String source = String::with_capacity(700000);\n"
                "    source.length = 700000;\n"
                "    reserve String reversed = source.reverse();\n"
                "    exit(0);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-O0", "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 70, execution.stderr)

    def test_hash_map_init_exits_cleanly_when_backing_allocations_are_exhausted(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-hash-map-exhaustion-") as directory:
            work = Path(directory)
            source = work / "hash-map-exhaustion.shaft"
            binary = work / "hash-map-exhaustion"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::HashMap<u64, u64> map;\n"
                "    reserve bool initialized = Collections::hash_map_init::<u64, u64>(ref map, 65536);\n"
                "    reserve bool inserted = Collections::hash_map_insert::<u64, u64>(ref map, 1, 2);\n"
                "    if (!initialized || !inserted) { exit(1); }\n"
                "    exit(0);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-O0", "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 70, execution.stderr)

    def test_hash_set_init_exits_cleanly_when_backing_allocations_are_exhausted(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-hash-set-exhaustion-") as directory:
            work = Path(directory)
            source = work / "hash-set-exhaustion.shaft"
            binary = work / "hash-set-exhaustion"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::HashSet<u64> set;\n"
                "    reserve bool initialized = Collections::hash_set_init::<u64>(ref set, 65536);\n"
                "    reserve bool inserted = Collections::hash_set_insert::<u64>(ref set, 1);\n"
                "    if (!initialized || !inserted) { exit(1); }\n"
                "    exit(0);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-O0", "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 70, execution.stderr)

    def test_str_literals_initialize_struct_fields_and_str_method_arguments(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-str-aggregate-") as directory:
            work = Path(directory)
            source = work / "quiz.shaft"
            binary = work / "quiz"
            source.write_text(
                "struct Question\n"
                "{\n"
                "    str text;\n"
                "    str a;\n"
                "    str b;\n"
                "    str c;\n"
                "    str corr_ans;\n"
                "}\n\n"
                "def main(String[] args)\n"
                "{\n"
                "    Question question = {\"What is the capital of France?\", \"London\", \"Paris\", \"Berlin\", \"b\"};\n"
                "    Vector<Question> quiz;\n"
                "    quiz.push(question);\n"
                "    foreach(Question q : quiz)\n"
                "    {\n"
                "        printf(\"{str}\\na) {str}\\nb) {str}\\nc) {str}\\n>> \", q.text, q.a, q.b, q.c);\n"
                "        reserve String result = readline();\n"
                "        bool is_equal = result.equals(q.corr_ans);\n"
                "        if (is_equal) { println(\"Correct!\"); }\n"
                "        else { print(\"Incorrect.\"); }\n"
                "    }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-O0", "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], input=b"b\n", capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertIn(b"Correct!", execution.stdout)

    def test_compiler_printf_formats_all_primitive_and_text_types(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-printf-") as directory:
            work = Path(directory)
            source = work / "printf.shaft"
            binary = work / "printf"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    i8 si8 = -8; u8 ui8 = 8; i16 si16 = -16; u16 ui16 = 16;\n"
                "    i32 si32 = -32; u32 ui32 = 32; i64 si64 = -64; u64 ui64 = 18446744073709551615; usize size = 65;\n"
                "    f32 single = 1.5; f64 decimal = -2.25; bool truth = true; char letter = 'Q';\n"
                "    reserve mut str name; name.data = \"Ada\"; name.length = 3;\n"
                "    reserve String owned = String::from_int(42);\n"
                "    printf(\"{i8} {u8} {i16} {u16} {i32} {u32} {i64} {u64} {usize} {f32} {f64} {bool} {char} {str} {String}\\n\", si8, ui8, si16, ui16, si32, ui32, si64, ui64, size, single, decimal, truth, letter, name, owned);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run([str(SHAFTC), "-o", str(binary), str(source)], capture_output=True, text=True, check=False)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)
            self.assertEqual(execution.stdout, b"-8 8 -16 16 -32 32 -64 18446744073709551615 65 1.500000 -2.250000 true Q Ada 42\n")

    def test_str_and_string_is_empty_are_native_methods(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-empty-") as directory:
            work = Path(directory)
            source = work / "string-empty.shaft"
            binary = work / "string-empty"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut str empty;\n"
                "    empty.length = 0;\n"
                "    reserve String text = String::from_int(7);\n"
                "    if (!empty.is_empty() || text.is_empty())\n"
                "    {\n"
                "        exit(1);\n"
                "    }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_find_tunnels_contains_and_optional_position_for_string_and_str(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-find-") as directory:
            work = Path(directory)
            source = work / "string-find.shaft"
            binary = work / "string-find"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String text = String::from_int(12345);\n"
                "    reserve mut str needle;\n"
                "    needle.data = text.data;\n"
                "    needle.length = 2;\n"
                "    {\n"
                "        reserve bool contains;\n"
                "        reserve ?u64 position;\n"
                "        text.find(needle);\n"
                "        valid position\n"
                "        {\n"
                "            if (!contains) { exit(1); }\n"
                "        }\n"
                "    }\n"
                "    {\n"
                "        reserve bool contains;\n"
                "        reserve ?u64 position;\n"
                "        needle.find(needle);\n"
                "        valid position\n"
                "        {\n"
                "            if (contains) { exit(0); }\n"
                "        }\n"
                "    }\n"
                "    exit(2);\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_reverse_returns_an_owned_reversed_string(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-reverse-") as directory:
            work = Path(directory)
            source = work / "string-reverse.shaft"
            binary = work / "string-reverse"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String text = String::from_int(12345);\n"
                "    reserve String stringReversed = text.reverse();\n"
                "    if (stringReversed.length != 5) { exit(1); }\n"
                "    if (stringReversed.data[0] != 53 || stringReversed.data[4] != 49) { exit(2); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_math_round_returns_nearest_float(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-math-round-") as directory:
            work = Path(directory)
            source = work / "math-round.shaft"
            binary = work / "math-round"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve f64 positive = Math::round(1.5);\n"
                "    reserve f64 negative = Math::round(-1.5);\n"
                "    if (positive != 2.0 || negative != -2.0) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_math_square_multiplies_float_value_by_itself(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-math-square-") as directory:
            work = Path(directory)
            source = work / "math-square.shaft"
            binary = work / "math-square"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve f64 positive = Math::square(3.5);\n"
                "    reserve f64 negative = Math::square(-2.0);\n"
                "    if (positive != 12.25 || negative != 4.0) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_math_sqrt_returns_principal_root_for_perfect_squares(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-math-sqrt-") as directory:
            work = Path(directory)
            source = work / "math-sqrt.shaft"
            binary = work / "math-sqrt"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve f64 nine = Math::sqrt(9.0);\n"
                "    reserve f64 zero = Math::sqrt(0.0);\n"
                "    if (nine != 3.0 || zero != 0.0) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_math_root_returns_requested_positive_degree_root(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-math-root-") as directory:
            work = Path(directory)
            source = work / "math-root.shaft"
            binary = work / "math-root"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve f64 cube = Math::root(27.0, 3);\n"
                "    reserve f64 fourth = Math::root(16.0, 4);\n"
                "    if (cube != 3.0 || fourth != 2.0) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_math_cos_returns_one_at_zero(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-math-cos-") as directory:
            work = Path(directory)
            source = work / "math-cos.shaft"
            binary = work / "math-cos"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve f64 result = Math::cos(0.0);\n"
                "    reserve f64 pi = Math::cos(3.141592653589793);\n"
                "    if (result != 1.0) { exit(1); }\n"
                "    if (pi > -0.999 || pi < -1.001) { exit(2); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_math_fcos_matches_cos_for_float_input(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-math-fcos-") as directory:
            work = Path(directory)
            source = work / "math-fcos.shaft"
            binary = work / "math-fcos"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve f64 result = Math::fcos(0.0);\n"
                "    if (result != 1.0) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_math_cin_returns_circular_sine_for_quarter_turn(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-math-cin-") as directory:
            work = Path(directory)
            source = work / "math-cin.shaft"
            binary = work / "math-cin"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve f64 result = Math::cin(1.5707963267948966);\n"
                "    if (result < 0.999 || result > 1.001) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_math_fcin_matches_cin_for_float_input(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-math-fcin-") as directory:
            work = Path(directory)
            source = work / "math-fcin.shaft"
            binary = work / "math-fcin"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve f64 circularSine = Math::cin(1.5707963267948966);\n"
                "    reserve f64 floatCircularSine = Math::fcin(1.5707963267948966);\n"
                "    if (floatCircularSine < 0.999 || floatCircularSine > 1.001) { exit(1); }\n"
                "    if (floatCircularSine != circularSine) { exit(2); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_collections_pair_is_a_native_generic_key_value_type(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-collections-pair-") as directory:
            work = Path(directory)
            source = work / "collections-pair.shaft"
            binary = work / "collections-pair"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::Pair<i32, bool> pair;\n"
                "    pair.key = 41;\n"
                "    pair.value = true;\n"
                "    if (pair.key != 41 || !pair.value) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_string_compare_is_lexicographic(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-compare-") as directory:
            work = Path(directory)
            source = work / "string-compare.shaft"
            binary = work / "string-compare"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String twelve = String::from_int(12);\n"
                "    reserve String thirteen = String::from_int(13);\n"
                "    reserve String oneHundredTwenty = String::from_int(120);\n"
                "    reserve i32 equal = twelve.compare(\"12\");\n"
                "    reserve i32 before = twelve.compare(\"13\");\n"
                "    reserve i32 after = thirteen.compare(\"12\");\n"
                "    reserve i32 prefix = oneHundredTwenty.compare(\"12\");\n"
                "    if (equal != 0 || before >= 0 || after <= 0 || prefix <= 0) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_collections_generic_hash_specializes_for_distinct_integer_types(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-collections-hash-") as directory:
            work = Path(directory)
            source = work / "collections-hash.shaft"
            binary = work / "collections-hash"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve u64 signedFirst = Collections::hash::<i32>(-7);\n"
                "    reserve u64 signedSecond = Collections::hash::<i32>(-7);\n"
                "    reserve u64 unsignedValue = Collections::hash::<u64>(7);\n"
                "    if (signedFirst != signedSecond || signedFirst == unsignedValue) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_collections_hash_map_is_generic_for_integer_keys_and_boolean_values(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-collections-generic-map-") as directory:
            work = Path(directory)
            source = work / "collections-generic-map.shaft"
            binary = work / "collections-generic-map"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::HashMap<i32, bool> map;\n"
                "    reserve bool initialized = Collections::hash_map_init::<i32, bool>(ref map, 4);\n"
                "    reserve bool first = Collections::hash_map_insert::<i32, bool>(ref map, -7, true);\n"
                "    reserve bool second = Collections::hash_map_insert::<i32, bool>(ref map, 2, false);\n"
                "    reserve bool replaced = Collections::hash_map_insert::<i32, bool>(ref map, -7, false);\n"
                "    reserve bool found = Collections::hash_map_contains::<i32, bool>(ref map, -7);\n"
                "    reserve bool missing = Collections::hash_map_contains::<i32, bool>(ref map, 99);\n"
                "    reserve bool value = Collections::hash_map_get::<i32, bool>(ref map, -7);\n"
                "    if (!initialized || !first || !second || !replaced || !found || missing || value) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_collections_hash_set_is_generic_for_signed_integers(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-collections-generic-set-") as directory:
            work = Path(directory)
            source = work / "collections-generic-set.shaft"
            binary = work / "collections-generic-set"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::HashSet<i32> set;\n"
                "    reserve bool initialized = Collections::hash_set_init::<i32>(ref set, 4);\n"
                "    reserve bool first = Collections::hash_set_insert::<i32>(ref set, -3);\n"
                "    reserve bool duplicate = Collections::hash_set_insert::<i32>(ref set, -3);\n"
                "    reserve bool second = Collections::hash_set_insert::<i32>(ref set, 5);\n"
                "    reserve bool found = Collections::hash_set_contains::<i32>(ref set, -3);\n"
                "    reserve bool missing = Collections::hash_set_contains::<i32>(ref set, 99);\n"
                "    if (!initialized || !first || duplicate || !second || !found || missing) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_vector_pushes_generic_values_into_preallocated_contiguous_storage(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-generic-vector-") as directory:
            work = Path(directory)
            source = work / "generic-vector.shaft"
            binary = work / "generic-vector"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Vector<i32> values;\n"
                "    reserve bool initialized = vector_init::<i32>(ref values, 4);\n"
                "    reserve bool first = values.push(-2);\n"
                "    reserve bool second = values.push(7);\n"
                "    if (!initialized || !first || !second || values.length != 2 || values[0] != -2 || values[1] != 7) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_vector_reserve_grows_geometrically_and_preserves_values(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-vector-reserve-") as directory:
            work = Path(directory)
            source = work / "vector-reserve.shaft"
            binary = work / "vector-reserve"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Vector<i32> values;\n"
                "    reserve bool initialized = vector_init::<i32>(ref values, 1);\n"
                "    mut i32 value = 0;\n"
                "    while (value < 8) { values.push(value); value = value + 1; }\n"
                "    reserve bool grown = vector_reserve::<i32>(ref values, 9);\n"
                "    reserve bool appended = values.push(8);\n"
                "    if (!initialized || !grown || !appended || values.capacity < 16 || values.length != 9) { exit(1); }\n"
                "    if (values[0] != 0 || values[7] != 7 || values[8] != 8) { exit(2); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_vector_push_grows_automatically_when_full(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-vector-auto-grow-") as directory:
            work = Path(directory)
            source = work / "vector-auto-grow.shaft"
            binary = work / "vector-auto-grow"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Vector<i32> values;\n"
                "    reserve bool initialized = vector_init::<i32>(ref values, 1);\n"
                "    mut i32 value = 0;\n"
                "    while (value < 9) { values.push(value); value = value + 1; }\n"
                "    if (!initialized || values.capacity < 16 || values.length != 9) { exit(1); }\n"
                "    if (values[0] != 0 || values[7] != 7 || values[8] != 8) { exit(2); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_string_hash_is_content_based_and_order_sensitive(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-hash-") as directory:
            work = Path(directory)
            source = work / "string-hash.shaft"
            binary = work / "string-hash"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String first = String::from_int(1234);\n"
                "    reserve String same = String::from_int(1234);\n"
                "    reserve String reversed = String::from_int(4321);\n"
                "    reserve u64 firstHash = first.hash();\n"
                "    reserve u64 sameHash = Collections::hash_str(\"1234\");\n"
                "    reserve u64 reversedHash = reversed.hash();\n"
                "    if (firstHash != sameHash || firstHash == reversedHash) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_hash_collections_accept_content_equal_string_keys(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-key-collections-") as directory:
            work = Path(directory)
            source = work / "string-key-collections.shaft"
            binary = work / "string-key-collections"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String first = String::from_int(1234);\n"
                "    reserve String second = String::from_int(1234);\n"
                "    reserve mut str firstKey;\n"
                "    firstKey.data = first.data; firstKey.length = first.length;\n"
                "    reserve mut str secondKey;\n"
                "    secondKey.data = second.data; secondKey.length = second.length;\n"
                "    reserve mut Collections::HashMap<str, i32> map;\n"
                "    reserve bool mapInitialized = Collections::hash_map_init::<str, i32>(ref map, 4);\n"
                "    reserve bool inserted = Collections::hash_map_str_insert::<i32>(ref map, firstKey, 7);\n"
                "    reserve bool updated = Collections::hash_map_str_insert::<i32>(ref map, secondKey, 9);\n"
                "    reserve u64 mapLength = Collections::hash_map_len::<str, i32>(ref map);\n"
                "    reserve bool found = Collections::hash_map_str_contains::<i32>(ref map, secondKey);\n"
                "    reserve bool absent = Collections::hash_map_str_contains::<i32>(ref map, \"other\");\n"
                "    reserve mut Collections::HashSet<str> set;\n"
                "    reserve bool setInitialized = Collections::hash_set_init::<str>(ref set, 4);\n"
                "    reserve bool added = Collections::hash_set_str_insert(ref set, firstKey);\n"
                "    reserve bool setFound = Collections::hash_set_str_contains(ref set, secondKey);\n"
                "    if (!mapInitialized || !inserted || !updated || mapLength != 1 || !found || absent || !setInitialized || !added || !setFound) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_hash_map_grows_automatically_without_losing_generic_entries(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-hash-map-growth-") as directory:
            work = Path(directory)
            source = work / "hash-map-growth.shaft"
            binary = work / "hash-map-growth"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::HashMap<i32, i32> map;\n"
                "    reserve bool initialized = Collections::hash_map_init::<i32, i32>(ref map, 1);\n"
                "    mut i32 key = 0;\n"
                "    while (key < 9) { Collections::hash_map_insert::<i32, i32>(ref map, key, key + 10); key = key + 1; }\n"
                "    reserve bool first = Collections::hash_map_contains::<i32, i32>(ref map, 0);\n"
                "    reserve bool last = Collections::hash_map_contains::<i32, i32>(ref map, 8);\n"
                "    if (!initialized || map.length != 9 || map.capacity < 16 || !first || !last) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_hash_set_grows_automatically_without_losing_generic_entries(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-hash-set-growth-") as directory:
            work = Path(directory)
            source = work / "hash-set-growth.shaft"
            binary = work / "hash-set-growth"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::HashSet<i32> set;\n"
                "    reserve bool initialized = Collections::hash_set_init::<i32>(ref set, 1);\n"
                "    mut i32 value = 0;\n"
                "    while (value < 9) { Collections::hash_set_insert::<i32>(ref set, value); value = value + 1; }\n"
                "    reserve bool first = Collections::hash_set_contains::<i32>(ref set, 0);\n"
                "    reserve bool last = Collections::hash_set_contains::<i32>(ref set, 8);\n"
                "    if (!initialized || set.length != 9 || set.capacity < 16 || !first || !last) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_string_key_hash_collections_grow_automatically(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-key-growth-") as directory:
            work = Path(directory)
            source = work / "string-key-growth.shaft"
            binary = work / "string-key-growth"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::HashMap<str, i32> map;\n"
                "    Collections::hash_map_init::<str, i32>(ref map, 1);\n"
                "    Collections::hash_map_str_insert::<i32>(ref map, \"a0\", 0); Collections::hash_map_str_insert::<i32>(ref map, \"a1\", 1); Collections::hash_map_str_insert::<i32>(ref map, \"a2\", 2);\n"
                "    Collections::hash_map_str_insert::<i32>(ref map, \"a3\", 3); Collections::hash_map_str_insert::<i32>(ref map, \"a4\", 4); Collections::hash_map_str_insert::<i32>(ref map, \"a5\", 5);\n"
                "    Collections::hash_map_str_insert::<i32>(ref map, \"a6\", 6); Collections::hash_map_str_insert::<i32>(ref map, \"a7\", 7); Collections::hash_map_str_insert::<i32>(ref map, \"a8\", 8);\n"
                "    reserve bool mapFound = Collections::hash_map_str_contains::<i32>(ref map, \"a8\");\n"
                "    reserve mut Collections::HashSet<str> set;\n"
                "    Collections::hash_set_init::<str>(ref set, 1);\n"
                "    Collections::hash_set_str_insert(ref set, \"a0\"); Collections::hash_set_str_insert(ref set, \"a1\"); Collections::hash_set_str_insert(ref set, \"a2\");\n"
                "    Collections::hash_set_str_insert(ref set, \"a3\"); Collections::hash_set_str_insert(ref set, \"a4\"); Collections::hash_set_str_insert(ref set, \"a5\");\n"
                "    Collections::hash_set_str_insert(ref set, \"a6\"); Collections::hash_set_str_insert(ref set, \"a7\"); Collections::hash_set_str_insert(ref set, \"a8\");\n"
                "    reserve bool setFound = Collections::hash_set_str_contains(ref set, \"a8\");\n"
                "    if (map.length != 9 || map.capacity < 16 || !mapFound || set.length != 9 || set.capacity < 16 || !setFound) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_hash_map_get_or_returns_a_generic_aggregate_value(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-hash-map-generic-value-") as directory:
            work = Path(directory)
            source = work / "hash-map-generic-value.shaft"
            binary = work / "hash-map-generic-value"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::Pair<i32, bool> payload; payload.key = 42; payload.value = true;\n"
                "    reserve mut Collections::Pair<i32, bool> fallback; fallback.key = 0; fallback.value = false;\n"
                "    reserve mut Collections::HashMap<i32, Collections::Pair<i32, bool>> map;\n"
                "    Collections::hash_map_init::<i32, Collections::Pair<i32, bool>>(ref map, 1);\n"
                "    Collections::hash_map_insert::<i32, Collections::Pair<i32, bool>>(ref map, 7, payload);\n"
                "    reserve Collections::Pair<i32, bool> fetched = Collections::hash_map_get_or::<i32, Collections::Pair<i32, bool>>(ref map, 7, fallback);\n"
                "    reserve Collections::Pair<i32, bool> missing = Collections::hash_map_get_or::<i32, Collections::Pair<i32, bool>>(ref map, 8, fallback);\n"
                "    if (fetched.key != 42 || !fetched.value || missing.key != 0 || missing.value) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_legacy_u64_hash_collections_use_fast_growing_generic_backends(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-u64-hash-backend-") as directory:
            work = Path(directory)
            source = work / "u64-hash-backend.shaft"
            binary = work / "u64-hash-backend"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Collections::HashMap<u64, u64> map;\n"
                "    Collections::hash_map_u64_init(ref map, 1);\n"
                "    mut u64 value = 0; while (value < 9) { Collections::hash_map_u64_insert(ref map, value, value + 10); value = value + 1; }\n"
                "    reserve bool mapFound = Collections::hash_map_u64_contains(ref map, 8);\n"
                "    reserve mut Collections::HashSet<u64> set;\n"
                "    Collections::hash_set_u64_init(ref set, 1);\n"
                "    value = 0; while (value < 9) { Collections::hash_set_u64_insert(ref set, value); value = value + 1; }\n"
                "    reserve bool setFound = Collections::hash_set_u64_contains(ref set, 8);\n"
                "    if (map.length != 9 || map.capacity < 16 || !mapFound || set.length != 9 || set.capacity < 16 || !setFound) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_string_binary_equality_compares_content_across_independent_allocations(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-binary-equality-") as directory:
            work = Path(directory)
            source = work / "string-binary-equality.shaft"
            binary = work / "string-binary-equality"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String first = String::from_int(1234);\n"
                "    reserve String same = String::from_int(1234);\n"
                "    reserve String different = String::from_int(1235);\n"
                "    reserve mut str textFirst; textFirst.data = \"same\"; textFirst.length = 4;\n"
                "    reserve mut str textSame; textSame.data = \"same\"; textSame.length = 4;\n"
                "    reserve mut str textDifferent; textDifferent.data = \"else\"; textDifferent.length = 4;\n"
                "    if (!(first == same) || first != same || first == different || !(first != different)) { exit(1); }\n"
                "    if (!(first == \"1234\") || first != \"1234\" || first == \"1235\" || !(first != \"1235\")) { exit(3); }\n"
                "    if (!(textFirst == textSame) || textFirst != textSame || textFirst == textDifferent || !(textFirst != textDifferent)) { exit(2); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_string_equals_compares_content_across_independent_allocations(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-string-equals-") as directory:
            work = Path(directory)
            source = work / "string-equals.shaft"
            binary = work / "string-equals"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve String first = String::from_int(1234);\n"
                "    reserve String second = String::from_int(1234);\n"
                "    reserve mut str equalText; equalText.data = second.data; equalText.length = second.length;\n"
                "    reserve bool equal = first.equals(equalText);\n"
                "    reserve bool different = first.equals(\"1235\");\n"
                "    if (!equal || different) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_vector_methods_infer_element_size_with_sizeof(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-vector-methods-") as directory:
            work = Path(directory)
            source = work / "vector-methods.shaft"
            binary = work / "vector-methods"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Vector<i32> values;\n"
                "    reserve bool initialized = vector_init::<i32>(ref values, 1);\n"
                "    reserve bool pushed = values.push(42);\n"
                "    if (!initialized || !pushed || values.element_size != sizeof(i32) || values.length != 1 || values.data[0] != 42) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_vector_pop_returns_last_value_and_reports_empty(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-vector-pop-") as directory:
            work = Path(directory)
            source = work / "vector-pop.shaft"
            binary = work / "vector-pop"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    reserve mut Vector<i32> values;\n"
                "    reserve bool initialized = vector_init::<i32>(ref values, 1);\n"
                "    reserve bool first = values.push(-2);\n"
                "    reserve bool second = values.push(7);\n"
                "    { reserve bool popped; reserve ?i32 value; values.pop(); valid value { if (!popped || value != 7 || values.length != 1 || values[0] != -2) { exit(1); } } }\n"
                "    { reserve bool popped; reserve ?i32 value; values.pop(); valid value { if (!popped || value != -2 || values.length != 0) { exit(2); } } }\n"
                "    { reserve bool popped; reserve ?i32 value; values.pop(); if (popped) { exit(3); } valid value { exit(4); } }\n"
                "    if (!initialized || !first || !second) { exit(5); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)

    def test_sizeof_reports_scalar_and_specialized_aggregate_abi_sizes(self):
        with tempfile.TemporaryDirectory(prefix="shaftc-sizeof-") as directory:
            work = Path(directory)
            source = work / "sizeof.shaft"
            binary = work / "sizeof"
            source.write_text(
                "def main(String[] args)\n"
                "{\n"
                "    if (sizeof(bool) != 1 || sizeof(i32) != 4 || sizeof(str) != 16 || sizeof(Vector<i32>) != 32) { exit(1); }\n"
                "}\n",
                encoding="utf-8",
            )
            compilation = subprocess.run(
                [str(SHAFTC), "-o", str(binary), str(source)], capture_output=True, text=True, check=False
            )
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            execution = subprocess.run([str(binary)], capture_output=True, check=False)
            self.assertEqual(execution.returncode, 0, execution.stderr)


if __name__ == "__main__":
    unittest.main()
