#include <agent/SessionHistory.h>
#include <agent/SessionStore.h>
#include <QtTest/QTest>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
struct Fixture {
    QTemporaryDir root{QDir::current().filePath("history-XXXXXX")};
    QString work=root.filePath("work"),other=root.filePath("other"),directory=root.filePath("sessions");
    a::SessionStore store{directory};QString viewer;
    Fixture(){QDir().mkpath(work);QDir().mkpath(other);viewer=store.create("fixture","private owner header",work).id;}
    QString session(QString text,QString workspace={}) {
        auto s=store.create("fixture","private-system-needle",workspace.isEmpty()?work:workspace);
        auto lease=store.acquire(s.id);a::Message m{{},a::MessageRole::User,text};m.metadata={{"secret","private-metadata-needle"}};lease->append(m);return s.id;
    }
    QString path(const QString& id){return QDir(directory).filePath(id+"/transcript.jsonl");}
};
QJsonArray matches(const QJsonObject& page){return page["matches"].toArray();}
}
class SessionHistoryTests:public QObject {
    Q_OBJECT
private slots:
    void literalUnicodeSearchAndOwnedWorkspace() {
        Fixture f;const auto own=f.session("Use 한글 and Qt 6.8 [ready].");f.session("foreign 한글",f.other);
        a::SessionHistory history(f.directory);
        auto page=history.search(f.viewer,f.work,{{"query","한글"}});QCOMPARE(matches(page).size(),1);
        const auto hit=matches(page).first().toObject();QCOMPARE(hit["session_id"].toString(),own);
        QCOMPARE(hit["line"].toInt(),2);QCOMPARE(hit["role"].toString(),"user");QCOMPARE(hit["field"].toString(),"text");
        QVERIFY(hit["byte_offset"].toInteger()>0);QVERIFY(hit["snippet"].toString().contains("한글"));
        QCOMPARE(matches(history.search(f.viewer,f.work,{{"query","qT 6.8 ["}})).size(),1);
        QCOMPARE(matches(history.search(f.viewer,f.work,{{"query","Qt.*"}})).size(),0);
        QCOMPARE(matches(history.search(f.viewer,f.work,{{"query","private-system-needle"}})).size(),0);
        QCOMPARE(matches(history.search(f.viewer,f.work,{{"query","private-metadata-needle"}})).size(),0);
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.other,{{"query","한글"}}));
    }
    void paginatesLargeHistoryWithoutWriterLeaseOrDuplicateResults() {
        Fixture f;const auto id=f.session("begin needle");auto lease=f.store.acquire(id);
        for(int i=0;i<32;++i)lease->append({{},a::MessageRole::User,QString(50000,'x')+QString(" needle %1").arg(i)});
        a::SessionHistoryOptions options;options.maxScanBytes=131072;options.maxRecordBytes=65536;
        a::SessionHistory history(f.directory,options);QJsonObject args{{"query","needle"},{"limit",2}};QSet<QString> ids;int pages=0;
        do {
            auto page=history.search(f.viewer,f.work,args);QVERIFY(page["scanned_bytes"].toInteger()<=options.maxScanBytes);
            for(const auto& value:matches(page)){const auto hit=value.toObject();QVERIFY(!ids.contains(hit["message_id"].toString()));ids.insert(hit["message_id"].toString());}
            args={{"cursor",page["next_cursor"]},{"limit",2}};QVERIFY(++pages<100);
            // A model appends its own search result between calls. This must
            // not invalidate a search of older, unchanged conversations.
            {auto viewer=f.store.acquire(f.viewer);viewer->append({{},a::MessageRole::User,"Current query progressed"});}
        }while(!args["cursor"].toString().isEmpty());
        QCOMPARE(ids.size(),33);QVERIFY(pages>16);QCOMPARE(lease->session().messages.size(),33);
    }
    void cursorCannotCrossOwnerQueryOrSnapshot() {
        Fixture f;const auto id=f.session("needle");{auto lease=f.store.acquire(id);lease->append({{},a::MessageRole::User,"needle second"});}
        const auto other=f.session("another session");a::SessionHistory history(f.directory);
        const auto page=history.search(f.viewer,f.work,{{"query","needle"},{"limit",1}});const auto cursor=page["next_cursor"].toString();QVERIFY(!cursor.isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error,history.search(other,f.work,{{"cursor",cursor}}));
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"cursor",cursor},{"query","changed"}}));
        a::SessionHistory foreign(f.directory);QVERIFY_THROWS_EXCEPTION(Error,foreign.search(f.viewer,f.work,{{"cursor",cursor}}));
        {auto lease=f.store.acquire(id);lease->append({{},a::MessageRole::User,"needle appended"});}
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"cursor",cursor}}));
    }
    void selectedSessionBoundsAndSymlinks() {
        Fixture f;const auto id=f.session("needle"),other=f.session("foreign needle",f.other);a::SessionHistory history(f.directory);
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{other}}}));
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{"../other"}}}));
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query","needle"},{"path",f.path(other)}}));
        const auto linked=QUuid::createUuid().toString(QUuid::WithoutBraces);QVERIFY(QFile::link(QDir(f.directory).filePath(other),QDir(f.directory).filePath(linked)));
        QCOMPARE(matches(history.search(f.viewer,f.work,{{"query","needle"}})).size(),1);
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{linked}}}));
        const auto ownOther=f.session("linked needle");QVERIFY(QFile::remove(f.path(ownOther)));QVERIFY(QFile::link(f.path(other),f.path(ownOther)));
        QCOMPARE(matches(history.search(f.viewer,f.work,{{"query","needle"}})).size(),1);
    }
    void partialTailRemainsUntouchedAndMalformedRecordsFail() {
        Fixture f;const auto id=f.session("needle");QFile file(f.path(id));QVERIFY(file.open(QIODevice::Append));file.write("{\"type\":");file.close();
        const auto before=QFileInfo(file).size();a::SessionHistory history(f.directory);
        auto page=history.search(f.viewer,f.work,{{"query","needle"}});QCOMPARE(matches(page).size(),1);
        QCOMPARE(page["incomplete_tails"].toInt(),1);QCOMPARE(QFileInfo(file).size(),before);
        QVERIFY(file.open(QIODevice::Append));file.write("INVALID}\n");file.close();
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query","needle"}}));
    }
    void cancellationLimitsAndNativeToolValidation() {
        Fixture f;const auto id=f.session("needle");a::SessionHistory history(f.directory);CancellationToken token;token.cancel();
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query","needle"}},token));
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query"," "}}));
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query","needle"},{"limit",1.5}}));
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{id,id}}}));
        a::ToolRegistry registry;registry.add(history.tool());registry.validateInput("SessionSearch",{{"query","needle"}});
        QVERIFY_THROWS_EXCEPTION(Error,registry.validateInput("SessionSearch",{{"query","needle"},{"workspace",f.other}}));
        const auto tool=registry.get("SessionSearch");QVERIFY(tool.definition.readOnly);QVERIFY(!tool.isMcp);
        QCOMPARE(matches(tool.execute({{"query","needle"}},{f.viewer,"run",f.work}).data).size(),1);
        a::SessionHistoryOptions tiny;tiny.maxSessions=2;a::SessionHistory limited(f.directory,tiny);f.session("another");
        QVERIFY_THROWS_EXCEPTION(Error,limited.search(f.viewer,f.work,{{"query","needle"}}));
        QCOMPARE(matches(limited.search(f.viewer,f.work,{{"query","needle"},{"session_ids",QJsonArray{id}}})).size(),1);
    }
    void structuredFieldsAndUnicodeExcerpts() {
        Fixture f;const auto id=f.session("Begin");
        {auto lease=f.store.acquire(id);
            lease->append({{},a::MessageRole::Assistant,{},{{"call","Lookup",{{"query","ARGUMENT_NEEDLE"}}}}});
            a::Message result{{},a::MessageRole::Tool,"Result"};
            result.toolCallId="call";result.data={{"value","DATA_NEEDLE"}};lease->append(result);
            lease->append({{},a::MessageRole::User,QString::fromUtf8("😀").repeated(300)+"UNICODE_NEEDLE"+QString::fromUtf8("😀").repeated(300)});
        }
        a::SessionHistoryOptions options;options.maxSnippetCharacters=512;a::SessionHistory history(f.directory,options);
        auto hit=matches(history.search(f.viewer,f.work,{{"query","ARGUMENT_NEEDLE"}})).first().toObject();QCOMPARE(hit["field"].toString(),"tool_calls");
        hit=matches(history.search(f.viewer,f.work,{{"query","DATA_NEEDLE"}})).first().toObject();QCOMPARE(hit["field"].toString(),"data");
        hit=matches(history.search(f.viewer,f.work,{{"query","UNICODE_NEEDLE"}})).first().toObject();const auto snippet=hit["snippet"].toString();
        QVERIFY(snippet.size()<=512);QVERIFY(snippet.contains("UNICODE_NEEDLE"));QCOMPARE(QString::fromUtf8(snippet.toUtf8()),snippet);QVERIFY(hit["truncated"].toBool());
    }
    void cursorsAreSingleUseBoundedAndExpiring() {
        Fixture f;const auto id=f.session("needle first");{auto lease=f.store.acquire(id);lease->append({{},a::MessageRole::User,"needle second"});}
        a::SessionHistoryOptions options;options.maxCursors=1;options.cursorLifetimeMs=200;
        a::SessionHistory history(f.directory,options);const QJsonObject args{{"query","needle"},{"limit",1}};
        auto first=history.search(f.viewer,f.work,args);auto cursor=first["next_cursor"].toString();QVERIFY(!cursor.isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,args));
        const auto next=history.search(f.viewer,f.work,{{"cursor",cursor}});QVERIFY(next["complete"].toBool());QCOMPARE(matches(next).size(),1);
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"cursor",cursor}}));
        cursor=history.search(f.viewer,f.work,args)["next_cursor"].toString();QTest::qWait(250);
        QVERIFY_THROWS_EXCEPTION(Error,history.search(f.viewer,f.work,{{"cursor",cursor}}));
        QVERIFY(!history.search(f.viewer,f.work,args)["next_cursor"].toString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(SessionHistoryTests)
#include "session_history_tests.moc"
