#include <agent/MemoryDream.h>
#include <agent/SessionStore.h>
#include <QtTest/QTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <QtCore/QLockFile>
#include <QtCore/QProcess>
#include <atomic>
#include <iostream>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
class Model final:public a::Model {
public:
    std::atomic<int> calls=0;std::function<a::ModelReply(const a::ModelRequest&,const CancellationToken&)> action;
    a::ModelReply generate(const a::ModelRequest& input,const CancellationToken& token,const std::function<bool(const QString&)>&)override {
        ++calls;return action?action(input,token):a::ModelReply{"The existing memories need no changes.",{},Usage{21,9}};
    }
};
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("dream-XXXXXX")};QString work=root.filePath("work"),sessions=root.filePath("sessions"),owner;
    a::SessionStore store{sessions};std::shared_ptr<Model> model=std::make_shared<Model>();
    std::shared_ptr<a::ProjectMemory> memory=std::make_shared<a::ProjectMemory>(a::ProjectMemoryOptions{root.filePath("memory")});
    std::shared_ptr<a::SessionHistory> history=std::make_shared<a::SessionHistory>(sessions);
    std::shared_ptr<a::ToolRegistry> tools=std::make_shared<a::ToolRegistry>();
    std::shared_ptr<a::RulePolicy> policy=std::make_shared<a::RulePolicy>();a::MemoryDreamOptions options;
    Fixture(){QDir().mkpath(work);owner=store.create("fixture","system",work).id;a::registerWorkspaceTools(*tools,work,{},QStringList{sessions});
        memory->bindWorkspaceTools(*tools);tools->add(history->tool());options.automatic=true;}
    QString prior(QString text="Durable prior context") {auto id=store.create("fixture","system",work).id;store.acquire(id)->append({{},a::MessageRole::User,text});return id;}
    a::MemoryContext snapshot(int turn=1) {
        a::MemoryContext s;s.sessionId=owner;s.workspace=work;s.registry=tools;s.context={owner,{},work};s.context.protectedPaths={sessions};
        s.request.model="fixture";s.request.systemPrompt="Parent system";s.request.contextId=owner;s.request.generation.temperature=.31;s.request.tools=tools->definitions();
        s.messages={{"u"+QString::number(turn),a::MessageRole::User,"Continue our work"},{"a"+QString::number(turn),a::MessageRole::Assistant,"Done"}};s.request.messages=s.messages;return s;
    }
    QJsonObject latest(a::MemoryDream& dream) {const auto list=dream.status(owner)["records"].toArray();require(!list.isEmpty(),"No dream record");return list.last().toObject();}
};
}
class MemoryDreamTests:public QObject {
    Q_OBJECT
private slots:
    void automaticGatesAndSuccessfulTimeSurviveRestart() {
        Fixture f;for(int i=0;i<5;++i)f.prior();
        {
            a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());QVERIFY(dream.drain(5000));
            QCOMPARE(f.latest(dream)["status"].toString(),"completed");QCOMPARE(f.latest(dream)["sessions_reviewing"].toInt(),5);
            const auto stamp=f.latest(dream)["last_consolidated_ms"].toInteger();QVERIFY(stamp>0);
            dream.offer(f.snapshot(2));QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"time_gate");QCOMPARE(f.model->calls.load(),1);
        }
        a::MemoryDream resumed(f.memory,f.history,f.model,f.policy,f.options);resumed.offer(f.snapshot(3));QVERIFY(resumed.drain(5000));QCOMPARE(f.latest(resumed)["status"].toString(),"time_gate");QCOMPARE(f.model->calls.load(),1);
    }
    void sessionGateScanThrottleAndExplicitRequest() {
        Fixture f;f.prior();a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);
        dream.offer(f.snapshot());QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"session_gate");
        dream.offer(f.snapshot(2));QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"scan_throttled");
        dream.request(f.owner);QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"completed");QCOMPARE(f.model->calls.load(),1);
        f.options.automatic=false;a::MemoryDream manual(f.memory,f.history,f.model,f.policy,f.options);
        QCOMPARE(manual.offer(f.snapshot())["status"].toString(),"retained");QCOMPARE(f.model->calls.load(),1);
        manual.request(f.owner);QVERIFY(manual.drain(5000));QCOMPARE(f.latest(manual)["status"].toString(),"completed");
    }
    void sameParentPrefixHistorySearchAndNativeConsolidation() {
        Fixture f;const auto old=f.prior("A durable preference uses ORCHID");f.options.automatic=false;
        const auto parent=f.snapshot();const auto path=QDir(f.memory->directory(f.work)).filePath("preferences.md");int round=0;
        f.model->action=[&](const a::ModelRequest& request,const auto&)->a::ModelReply {
            require(request.model==parent.request.model&&request.systemPrompt==parent.request.systemPrompt&&request.contextId==parent.request.contextId,"Changed parent configuration");
            require(request.generation.temperature==parent.request.generation.temperature&&request.tools.size()==parent.request.tools.size(),"Changed parent sampling or tools");
            require(request.messages.mid(0,parent.request.messages.size())==parent.request.messages,"Changed parent prefix");
            if(++round==1)return {{},{{"search","SessionSearch",{{"query","ORCHID"}}}}};
            if(round==2){require(!request.messages.last().isError,"History search failed");require(request.messages.last().data["matches"].toArray().first().toObject()["session_id"]==old,"Search used another owner");
                return {{},{{"write","Write",{{"path",path},{"content","---\nname: Preference\ndescription: Durable preference\ntype: feedback\n---\nUse ORCHID.\n"}}}}};}
            require(!request.messages.last().isError,"Memory write failed");return {"Consolidated preference."};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(parent);dream.request(f.owner);QVERIFY(dream.drain(5000));
        const auto result=f.latest(dream);QVERIFY2(result["status"]=="completed",QJsonDocument(result).toJson().constData());
        QCOMPARE(result["written_paths"].toArray(),QJsonArray{path});QCOMPARE(result["phase"].toString(),"updating");QCOMPARE(result["recent_turns"].toArray().size(),3);
        QVERIFY(f.store.load(f.owner).messages.isEmpty());
    }
    void cancellationAndOtherOwnerLockDoNotAdvanceSuccessfulTime() {
        Fixture f;for(int i=0;i<5;++i)f.prior();std::atomic<bool> started=false;
        f.model->action=[&](const auto&,const CancellationToken& token)->a::ModelReply{started=true;while(!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();return {};};
        a::MemoryDream first(f.memory,f.history,f.model,f.policy,f.options);first.offer(f.snapshot());QTRY_VERIFY_WITH_TIMEOUT(started,3000);
        a::MemoryDream other(f.memory,f.history,f.model,f.policy,f.options);other.offer(f.snapshot());QVERIFY(other.drain(5000));QCOMPARE(f.latest(other)["status"].toString(),"locked");
        first.cancel(f.owner);QVERIFY(first.drain(5000));QCOMPARE(f.latest(first)["status"].toString(),"cancelled");QCOMPARE(f.latest(first)["last_consolidated_ms"].toInteger(),0);
        f.model->action={};other.request(f.owner);QVERIFY(other.drain(5000));QCOMPARE(f.latest(other)["status"].toString(),"completed");
    }
    void capabilityFailureAndModelFailureAreRetryable() {
        Fixture f;f.options.automatic=false;int callback=0;auto evil=f.tools->get("Read");evil.definition.name="ExternalRead";evil.isMcp=true;
        evil.validate=[&](const auto&,const auto&){++callback;};f.tools->add(evil);int round=0;
        f.model->action=[&](const auto&,const auto&)->a::ModelReply {
            if(++round==1)return {{},{{"external","ExternalRead",{{"path",f.work}}},{"write","Write",{{"path",QDir(f.work).filePath("bad.md")},{"content","bad"}}}}};return {"Unable to save."};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);QVERIFY(dream.drain(5000));
        QCOMPARE(f.latest(dream)["status"].toString(),"tool_error");QCOMPARE(callback,0);QVERIFY(!QFileInfo::exists(QDir(f.work).filePath("bad.md")));QCOMPARE(f.latest(dream)["last_consolidated_ms"].toInteger(),0);
        f.model->action=[](const auto&,const auto&)->a::ModelReply{throw std::runtime_error("model failed");};dream.request(f.owner);QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"failed");
        f.model->action={};dream.request(f.owner);QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"completed");
    }
    void liveProcessLockAndDeadHolderRecovery() {
        Fixture f;f.options.automatic=false;const auto directory=QFileInfo(f.memory->directory(f.work)).absolutePath();
        QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--hold-dream-lock",directory+"/.dream.lock"});
        QVERIFY(child.waitForStarted(3000));QVERIFY(child.waitForReadyRead(3000));QVERIFY(child.readAllStandardOutput().contains("LOCKED"));
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);
        QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"locked");QCOMPARE(f.model->calls.load(),0);
        QVERIFY(!QFileInfo::exists(directory+"/.dream-state.json"));
        child.kill();QVERIFY(child.waitForFinished(3000));QVERIFY(QFileInfo::exists(directory+"/.dream.lock"));
        dream.request(f.owner);QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"completed");
        QVERIFY(!QFileInfo::exists(directory+"/.dream.lock"));QVERIFY(QFileInfo::exists(directory+"/.dream-state.json"));
    }
    void corruptStateAndSymlinkFailWithoutRunningModel() {
        Fixture f;f.options.automatic=false;const auto directory=QFileInfo(f.memory->directory(f.work)).absolutePath();
        const auto state=directory+"/.dream-state.json";QFile file(state);QVERIFY(file.open(QIODevice::WriteOnly));file.write("{broken");file.close();
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);
        QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"failed");QCOMPARE(f.model->calls.load(),0);
        QVERIFY(file.remove());QFile outside(f.root.filePath("other.json"));QVERIFY(outside.open(QIODevice::WriteOnly));
        const QByteArray bytes="{\"version\":1,\"last_consolidated_ms\":0}";outside.write(bytes);outside.close();
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(outside.fileName(),state));dream.request(f.owner);QVERIFY(dream.drain(5000));
        QCOMPARE(f.latest(dream)["status"].toString(),"failed");QCOMPARE(f.model->calls.load(),0);QVERIFY(QFile::remove(state));
        QVERIFY(QFile::link(outside.fileName(),directory+"/.dream.lock"));dream.request(f.owner);QVERIFY(dream.drain(5000));
        QCOMPARE(f.latest(dream)["status"].toString(),"failed");QCOMPARE(f.model->calls.load(),0);QVERIFY(QFile::remove(directory+"/.dream.lock"));
#endif
        QVERIFY(outside.open(QIODevice::ReadOnly));QCOMPARE(outside.readAll(),bytes);outside.close();
        dream.request(f.owner);QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"completed");
    }
    void timeoutRetryAndWorkerCallbackLifecycle() {
        Fixture f;f.options.automatic=false;f.options.timeoutMs=40;std::atomic<bool> callbackGuard=false;
        f.model->action=[](const auto&,const CancellationToken& token)->a::ModelReply {while(!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();return {};};
        a::MemoryDream* owner=nullptr;f.options.completed=[&](const auto&){try{owner->drain(1);}catch(const Error&){callbackGuard=true;}};
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);owner=&dream;dream.offer(f.snapshot());dream.request(f.owner);
        QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"timeout");QVERIFY(callbackGuard);
        QCOMPARE(f.latest(dream)["last_consolidated_ms"].toInteger(),0);f.model->action={};dream.request(f.owner);
        QVERIFY(dream.drain(5000));QCOMPARE(f.latest(dream)["status"].toString(),"completed");
    }
    void queuedWorkCoalescesAndIdleContextsEvict() {
        Fixture f;f.options.automatic=false;f.options.maxSessions=1;f.options.maxRecords=2;
        std::atomic<bool> started=false,release=false;QStringList seen;
        f.model->action=[&](const a::ModelRequest& request,const CancellationToken& token)->a::ModelReply {
            started=true;while(!release&&!token.isCancelled())QThread::msleep(1);token.throwIfCancelled();
            seen.append(request.messages[1].id);return {"Done."};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);QTRY_VERIFY_WITH_TIMEOUT(started,3000);
        auto other=f.snapshot();other.sessionId=f.prior();QVERIFY_THROWS_EXCEPTION(Error,dream.offer(other));
        dream.offer(f.snapshot(2));dream.request(f.owner);dream.offer(f.snapshot(3));dream.request(f.owner);
        release=true;QVERIFY(dream.drain(5000));QCOMPARE(seen,(QStringList{"a1","a3"}));QVERIFY(dream.status(f.owner)["count"].toInt()<=2);
        QCOMPARE(dream.offer(other)["status"].toString(),"retained");QVERIFY(!dream.status(f.owner)["has_context"].toBool());
        QCOMPARE(dream.request(f.owner)["status"].toString(),"no_context");dream.close();QVERIFY_THROWS_EXCEPTION(Error,dream.request(other.sessionId));
    }
    void recentTurnWindowAndUnicodeRemainBounded() {
        Fixture f;f.options.automatic=false;f.options.maxTurns=35;int turn=0;
        const auto path=QDir(f.work).filePath("note.txt");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
        f.model->action=[&](const auto&,const auto&)->a::ModelReply {
            return {QString(1023,'x')+QString::fromUtf8("😀")+QString(500,'y'),{{QString::number(++turn),"Read",{{"path",path}}}}};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);QVERIFY(dream.drain(5000));
        const auto result=f.latest(dream);QCOMPARE(result["status"].toString(),"turn_limit");QCOMPARE(result["tool_errors"].toInt(),0);
        QCOMPARE(result["last_consolidated_ms"].toInteger(),0);QVERIFY(result["turns_truncated"].toBool());
        const auto recent=result["recent_turns"].toArray();QVERIFY(!recent.isEmpty()&&recent.size()<=30);
        QVERIFY(QJsonDocument(recent).toJson(QJsonDocument::Compact).size()<=32768);
        for(const auto& value:recent){const auto item=value.toObject();QVERIFY(item["text_truncated"].toBool());QCOMPARE(item["text"].toString(),QString(1023,'x'));}
    }
    void historyMetadataFiltersCurrentWorkspaceAndTimestamp() {
        Fixture f;const auto previous=f.prior();f.store.acquire(f.owner)->append({{},a::MessageRole::User,"Current conversation"});
        const auto elsewhere=f.root.filePath("elsewhere");QDir().mkpath(elsewhere);f.store.create("fixture","system",elsewhere);
        const auto recent=f.history->recent(f.owner,f.work,0);QCOMPARE(recent.size(),1);const auto item=recent.first().toObject();
        QCOMPARE(item["session_id"].toString(),previous);QVERIFY(item["modified_ms"].toInteger()>0);QVERIFY(item["size_bytes"].toInteger()>0);
        QVERIFY(f.history->recent(f.owner,f.work,item["modified_ms"].toInteger()).isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error,f.history->recent(f.owner,elsewhere,0));CancellationToken cancelled;cancelled.cancel();
        QVERIFY_THROWS_EXCEPTION(Error,f.history->recent(f.owner,f.work,0,cancelled));
    }
    void completionRequiresAUniqueBoundedIndex() {
        Fixture f;f.options.automatic=false;const auto index=QDir(f.memory->directory(f.work)).filePath("MEMORY.md");
        const QByteArray duplicated="- [First](preferences.md)\n- [Second](./preferences.md)\n";
        QFile file(index);QVERIFY(file.open(QIODevice::WriteOnly));file.write(duplicated);file.close();int turn=0;
        f.model->action=[&](const a::ModelRequest& request,const auto&)->a::ModelReply {
            if(++turn==1)return {"No changes."};
            if(turn==2){require(request.messages.last().text.contains("duplicate"),"Missing host index review");return {{},{{"read-index","Read",{{"path",index}}}}};}
            if(turn==3)return {{},{{"fix-index","Write",{{"path",index},{"content","- [Preferences](preferences.md)\n"}}}}};
            return {"Removed the duplicate link."};
        };
        a::MemoryDream dream(f.memory,f.history,f.model,f.policy,f.options);dream.offer(f.snapshot());dream.request(f.owner);QVERIFY(dream.drain(5000));
        const auto result=f.latest(dream);QCOMPARE(result["status"].toString(),"completed");QCOMPARE(result["validation_retries"].toInt(),1);
        QVERIFY(result["completion_validated"].toBool());QCOMPARE(result["turns"].toInt(),4);QVERIFY(result["last_consolidated_ms"].toInteger()>0);
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("- [Preferences](preferences.md)\n"));file.close();

        Fixture stubborn;stubborn.options.automatic=false;stubborn.options.maxTurns=2;
        QFile longIndex(QDir(stubborn.memory->directory(stubborn.work)).filePath("MEMORY.md"));QVERIFY(longIndex.open(QIODevice::WriteOnly));
        longIndex.write(QByteArray(25001,'x'));longIndex.close();
        a::MemoryDream bounded(stubborn.memory,stubborn.history,stubborn.model,stubborn.policy,stubborn.options);bounded.offer(stubborn.snapshot());bounded.request(stubborn.owner);
        QVERIFY(bounded.drain(5000));const auto failure=stubborn.latest(bounded);QCOMPARE(failure["status"].toString(),"turn_limit");
        QCOMPARE(failure["validation_retries"].toInt(),2);QVERIFY(!failure["completion_validated"].toBool());QCOMPARE(failure["last_consolidated_ms"].toInteger(),0);
    }
};
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--hold-dream-lock") {
        QLockFile lock(QString::fromLocal8Bit(argv[2]));lock.setStaleLockTime(0);if(!lock.tryLock(0))return 2;
        std::cout<<"LOCKED"<<std::endl;return app.exec();
    }
    MemoryDreamTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "memory_dream_tests.moc"
