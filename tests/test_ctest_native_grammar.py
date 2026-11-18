from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

class NativeGrammarCTestContract(unittest.TestCase):
    def test_cases_and_parameter_rows_are_preserved(self):
        source = (ROOT / "tests/native_grammar_tests.cpp").read_text()
        for name in ("requiredJsonToolPrefillStartsInsideTheCallEnvelope", "taggedRootAndLargeSignatures", "conditionalArguments:", "singleCallLimit:"):
            self.assertIn(f'runner.run("{name}"', source) if name[-1] != ":" else self.assertIn(name, source)
        for filename in ("Qwen3.5-4B.jinja", "Qwen-Qwen3-0.6B.jinja", "Qwen-Qwen2.5-7B-Instruct.jinja"):
            self.assertIn(filename, source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiLocalLLMNativeGrammarTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        source = (ROOT / "tests/native_grammar_tests.cpp").read_text()
        self.assertIn("for (const bool required : {false, true})", source)
        self.assertIn("for (const bool parallel : {false, true})", source)

if __name__ == "__main__":
    unittest.main()
