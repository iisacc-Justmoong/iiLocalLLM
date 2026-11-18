from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['connectedRpcCancellationFinishesTheEventStream', 'waitingRpcCannotOccupyThePermissionControlCapacity', 'slowResponseKeepsCapacityUntilTheSocketCloses', 'structuredToolsRoundTripAndSse', 'automaticResidencyAndKeepAlive', 'jsonConversationAndDiscovery', 'sseStreamsUnicodeUsageAndStop', 'validationAndHttpBoundaries', 'detailedGenerationParametersReachRuntime', 'runtimeFailureDisconnectAndCleanup', 'queueLimitsDeadlineAndSharedScheduler', 'outputLimitAndServerShutdown']

class HttpCTestContract(unittest.TestCase):
    def test_cases_and_data_rows_are_preserved(self):
        source = (ROOT / "tests/http_tests.cpp").read_text()
        for case in CASES:
            suffix = ':' if case in ("connectedRpcCancellationFinishesTheEventStream", "slowResponseKeepsCapacityUntilTheSocketCloses") else '"'
            self.assertIn(f'runner.run("{case}{suffix}', source)
        for row in ("hook-cancellation", "backend-shutdown", "json-work", "sse-work", "json-control", "sse-control"):
            self.assertIn(f':{row}"', source)
        self.assertNotIn("QDir::current().filePath", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_no_qttest_in_module_test_sources_or_cmake(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertNotRegex(cmake, r"Qt6::Test|find_package\(Qt6[^)]*\bTest\b")
        pattern = re.compile(r"#\s*include\s*[<\"](?:QtTest|QTest|QSignalSpy|QAbstractItemModelTester)|\b(?:QTEST_[A-Z_]+|QTest::|QSignalSpy|QAbstractItemModelTester)\b")
        for path in (ROOT / "tests").rglob("*"):
            if path.suffix in (".cpp", ".cc", ".h", ".hpp", ".mm") and path.is_file():
                with self.subTest(path=path.relative_to(ROOT)):
                    self.assertNotRegex(path.read_text(errors="replace"), pattern)

if __name__ == "__main__":
    unittest.main()
