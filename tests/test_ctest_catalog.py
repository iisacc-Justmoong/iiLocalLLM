from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ['minimalManifestAndCanonicalUri', 'invalidManifestsAreRejected', 'installRejectsUnlistedOnnxWithoutChangingSource', 'projectorIsPackageOwnedAndIntegrityChecked', 'installVerifyResolvePersistAndRemove', 'integrityDetectsChangedMissingAndUnlistedFiles', 'expectedHashesAndInterruptedInstallationNeverPublish', 'metadataProblemsAreVisibleAndDoNotHideOtherModels', 'mlxDirectoryAndDeclaredFormat', 'symlinksAndManifestTraversalAreRejected']

class CatalogCTestContract(unittest.TestCase):
    def test_every_case_is_registered_without_qttest(self):
        source = (ROOT / "tests/catalog_tests.cpp").read_text()
        for case in CASES:
            self.assertIn(f'runner.run("{case}"', source)
        self.assertNotRegex(source, r"\b(?:QTest|QSignalSpy|QAbstractItemModelTester)\b|\bQTEST_|<QtTest|\bQ(?:VERIFY|COMPARE|TRY|FETCH|SKIP|FAIL)\w*")
        self.assertNotIn("QDir::current().filePath", source)

    def test_target_does_not_link_qttest(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        link = re.search(r"target_link_libraries\(iiLocalLLMCatalogTests\s+([^)]*)\)", cmake)
        self.assertIsNotNone(link)
        self.assertNotIn("Qt6::Test", link.group(1))

if __name__ == "__main__":
    unittest.main()
