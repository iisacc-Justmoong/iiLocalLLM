from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['embeddingUsesDeclaredCapabilityAndReleasesTheModel', 'embeddingReleasesCachedContextsWithoutDeletingSessions', 'conversationBudgetUsesTokenizerWithoutGeneration', 'structuredConversationPreservesToolsAndModelScopedCache', 'structuredConversationRejectsBrokenToolProtocol', 'bundledStarterAliasSupportsConversation', 'residencyReuseLruAndSessionRestoration', 'hostPinnedModelSurvivesIdleAndMemoryPressure', 'expiryRunsWithoutRequestsAndZeroLifetimeReleasesAfterCancellation', 'admissionRejectsBeforeRuntimeAllocation', 'managedLifecycleAndIntegrityGate', 'manifestCapabilitiesGateChat', 'ipcOwnsExecutionChoice', 'multiTurnAndStreaming', 'cancellationRollsBackAndInvalidatesCache', 'fifoAndQueueLimit', 'lruAndModelLifecycle', 'contextTrimsCompleteTurnsAndRejectsOversize', 'failuresAndInvalidRequests', 'stopAcrossChunks', 'shutdownSettlesActiveAndQueuedRequests', 'queuedTurnsObserveCommittedHistory', 'cacheTokenBudgetAndRegistrationErrors', 'ipcRoundTripAndFraming', 'utf8BoundariesAndTruncatedFinalToken', 'ipcCancellationBypassesFullConnectionLimit', 'ipcDisconnectCancelsAndOutputLimitRollsBack']

class ServiceCTestContract(unittest.TestCase):
    def test_cases_and_fixture_lifecycle_are_preserved(self):
        source = (ROOT / "tests/service_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runCase("{case}"', source)
        self.assertIn("tests.init()", source)
        self.assertIn("tests.cleanup()", source)
        self.assertIn("qScopeGuard", source)
        self.assertIn('"--offline"', source)
        self.assertNotIn("QDir::current().filePath", source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiLocalLLMServiceTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))
        self.assertIn("iiLocalLLM.service_offline", cmake)

if __name__ == "__main__":
    unittest.main()
