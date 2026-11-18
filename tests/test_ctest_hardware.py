from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

class HardwareCTestContract(unittest.TestCase):
    def test_data_rows_and_offline_selection_are_preserved(self):
        source = (ROOT / "tests/hardware_tests.cpp").read_text()
        for name in ("no-gpu", "apple-silicon", "nvidia-cuda", "amd-vulkan", "intel-vulkan", "intel-mac-metal-is-not-apple-silicon", "nvidia-no-cuda", "unknown-vendor", "cuda-before-vulkan", "metal-before-cuda", "allocation", "driver-disappeared", "initialization", "cancelled", "invalid-model", "not-found"):
            self.assertIn(f'"{name}"', source)
        for name in ("nativeRuntimeLimits", "unsupportedAndUnavailableAccelerationIsExcluded", "neverCombinesCapabilitiesFromDifferentAdapters", "sameBackendUsesMemoryThenStableId", "unifiedMemoryIsNotReportedAsVram", "formatAndCapabilitiesDetermineRuntime", "overrideAndFailedCpuNeverPublishModel", "nativeSnapshotIsStableAndQueryable"):
            self.assertIn(f'runner.run("{name}"', source)
        self.assertIn('"--offline"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiLocalLLMHardwareTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiLocalLLM.hardware_policy_offline", cmake)

if __name__ == "__main__":
    unittest.main()
