from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['immutableIndexesAndCompiledPatternsPreserveValidation', 'catalogCoversInferenceTrainingAndAdapters', 'typedValuesRejectUnknownNamesAndWrongTypes', 'configurationRoundTripPreservesNativeNames', 'trainingConfigurationDoesNotClaimAnInferenceBinding', 'inheritedTrainingControlsAndOptimizerTuples', 'defaultsNullUnsetAndValidationBeforeExport', 'nativeGenerationBindingsAreStrictAndRoundTrip', 'importedSchemasEnforceUnionBoundsAndNestedRequiredFields', 'redactionFollowsNestedObjectReferences']

class ParametersCTestContract(unittest.TestCase):
    def test_every_case_is_registered_without_qttest(self):
        source = (ROOT / "tests/parameter_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiLocalLLMParameterTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))

if __name__ == "__main__":
    unittest.main()
