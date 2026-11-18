from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['llamaQuantizedKvReservation', 'budgetLruAndActiveProtection', 'expiryAndSmallMachinePolicy', 'durationAndOverflow', 'pinnedModelsAreNeverIdleEvictionCandidates', 'idleSchedulerDoesNotPollWithoutADeadline']

class ResidencyCTestContract(unittest.TestCase):
    def test_all_cases_are_preserved_without_qttest(self):
        source = (ROOT / "tests/residency_tests.cpp").read_text()
        for name in CASES:
            self.assertIn(f'runner.run("{name}"', source)
        self.assertIn("iillm_ctest::processFor(250)", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest_and_preserves_scheduler_source(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiLocalLLMResidencyTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("tests/residency_tests.cpp src/service/ModelResidencyManager.cpp src/service/Scheduler.cpp", cmake)

if __name__ == "__main__":
    unittest.main()
