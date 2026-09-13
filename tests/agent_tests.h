#pragma once
#include <QtCore/QObject>
class AgentTests : public QObject {
    Q_OBJECT
private slots:
    void schemaAndRegistry();
    void permissionsAndHookRevalidation();
    void transcriptInvariantsAndRecovery();
    void toolLoopAndPersistentResume();
    void interruptedToolIsNotExecutedAgain();
    void schemaFailureBecomesToolResult();
    void cancellationAndConsumerFailure();
    void workspaceReadEditAndStaleGuard();
    void parallelToolsAndExclusiveBarrier();
    void stopHookAndTurnLimit();
    void shellTimeoutAndExitStatus();
    void invalidResultsAndDuplicateCalls();
    void registrySnapshotDuringExecution();
};
