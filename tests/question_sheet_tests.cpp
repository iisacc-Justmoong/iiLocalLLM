#include <agent/QuestionInbox.h>
#include <agent/UserQuestions.h>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <future>
using namespace iiLocalLLM;
namespace a=iiLocalLLM::agent;
namespace {
QJsonObject question(QString text="Which renderer?",bool multi=false) {
    return {{"questions",QJsonArray{QJsonObject{{"question",text},{"header","Renderer"},{"multiSelect",multi},
        {"options",QJsonArray{QJsonObject{{"label","Qt"},{"description","Native"},{"preview","<b>Native preview</b>"}},
            QJsonObject{{"label","Web"},{"description","Browser"},{"preview","**Browser preview**"}}}}}}}};
}
struct Fixture {
    QTemporaryDir directory{QDir::current().filePath("question-ui-XXXXXX")};
    a::QuestionInbox inbox;
    QQmlApplicationEngine engine;
    QStringList warnings;
    QQuickWindow* window=nullptr;
    std::vector<std::future<a::ToolResult>> pending;
    Fixture() {
        QObject::connect(&engine,&QQmlApplicationEngine::warnings,&engine,[this](const QList<QQmlError>& values){for(const auto& value:values)warnings.append(value.toString());});
        engine.addImportPath(QUESTION_LVRS_IMPORT_PATH);
        engine.setInitialProperties({{"testInbox",QVariant::fromValue(&inbox)},
            {"sheetUrl",QUrl::fromLocalFile(QString(QUESTION_QML_DIR)+"/UserQuestionsSheet.qml")}});
        engine.loadData(R"(
            import QtQuick
            import LVRS 1.0 as LV
            LV.ApplicationWindow {
                required property var testInbox
                required property url sheetUrl
                visible: true; width: 960; height: 900
                Loader { id: sheet; objectName: "questionLoader"; Component.onCompleted: setSource(sheetUrl, {inbox: testInbox}) }
            }
        )");
        if(!engine.rootObjects().isEmpty())window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    }
    ~Fixture(){inbox.close();}
    void ask(QJsonObject input=question(),CancellationToken token={}) {
        const auto hook=inbox.hook();const auto cwd=directory.path();const auto id=QString::number(pending.size());
        pending.push_back(std::async(std::launch::async,[=] {
            auto registry=std::make_shared<a::ToolRegistry>();registry->add(a::userQuestionTool({false,"markdown"}));
            a::ToolRunnerOptions options;options.hooks={hook};
            try {return a::ToolRunner(registry,std::make_shared<a::RulePolicy>(a::PermissionMode::Bypass),options)
                .run({id,"AskUserQuestion",input},{id,"run",cwd,{},token});}
            catch(const Error& error){if(error.code()!=ErrorCode::Cancelled)throw;return a::ToolResult{QString::fromUtf8(error.what()),{},true};}
        }));
    }
    QObject* visual(QQuickItem* item,const char* name) {
        if(item->objectName()==name)return item;
        if(auto* found=item->findChild<QObject*>(name))return found;
        for(auto* child:item->childItems())if(auto* found=visual(child,name))return found;
        return nullptr;
    }
    QObject* object(const char* name) {
        if(!window)return nullptr;
        if(auto* found=window->findChild<QObject*>(name))return found;
        // Popup content is visually reparented into the window overlay.
        return visual(window->contentItem(),name);
    }
    QQuickItem* item(const char* name){return qobject_cast<QQuickItem*>(object(name));}
    bool click(const char* name) {
        auto* target=item(name);if(!target||!target->isVisible())return false;
        const auto point=target->mapToScene(QPointF(target->width()/2,target->height()/2)).toPoint();
        if(!QRect(QPoint(0,0),window->size()).contains(point)){qWarning()<<name<<"outside window"<<point;return false;}
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point);return true;
    }
    bool type(const char* name,const QString& text) {
        auto* wrapper=object(name);if(!wrapper)return false;
        auto* editor=qvariant_cast<QQuickItem*>(wrapper->property("editorItem"));if(!editor)return false;
        editor->forceActiveFocus();QInputMethodEvent input;input.setCommitString(text);QCoreApplication::sendEvent(editor,&input);return true;
    }
};
}
class QuestionSheetTests:public QObject {
    Q_OBJECT
private slots:
    void singleSelectionAndPlainPreview() {
        Fixture f;QVERIFY2(f.window,qPrintable(f.warnings.join('\n')));f.ask();
        QTRY_VERIFY(f.object("agentQuestionChoice_0_0"));
        QTRY_VERIFY(f.object("agentQuestionsSheet")->property("opened").toBool());
        QVERIFY(f.click("agentQuestionChoice_0_0"));
        QVERIFY(f.click("agentQuestionChoice_0_0"));QVERIFY(f.object("agentQuestionChoice_0_0")->property("checked").toBool());
        QTRY_COMPARE(f.object("agentQuestionPreview_0")->property("text").toString(),"<b>Native preview</b>");
        QCOMPARE(f.object("agentQuestionPreview_0")->property("textFormat").toInt(),int(Qt::PlainText));
        f.window->grabWindow().save(QDir::current().filePath("question-before-submit.png"));
        QVERIFY(f.click("agentQuestionSubmit"));QTRY_VERIFY2_WITH_TIMEOUT(f.inbox.requests().isEmpty(),qPrintable(f.inbox.errorString()),3000);
        const auto result=f.pending.front().get();QVERIFY2(!result.isError,qPrintable(result.text));
        QCOMPARE(result.data["answers"].toObject()["Which renderer?"],"Qt");
        QCOMPARE(result.data["annotations"].toObject()["Which renderer?"].toObject()["preview"],"<b>Native preview</b>");
        QVERIFY2(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void multilineFreeAnswerAndNotesPreservePrototypeKeys() {
        Fixture f;QVERIFY(f.window);f.ask(question("__proto__",true));
        QTRY_VERIFY(f.object("agentQuestionOption_0_0"));QTRY_VERIFY(f.object("agentQuestionsSheet")->property("opened").toBool());
        QVERIFY(f.click("agentQuestionOption_0_1"));QVERIFY(f.click("agentQuestionOption_0_0"));
        QVERIFY(f.type("agentQuestionOther_0","직접 입력\n두 번째 줄"));
        QVERIFY(f.type("agentQuestionNotes_0","검토 메모\n두 번째 줄"));
        QVERIFY(f.click("agentQuestionSubmit"));QTRY_VERIFY2_WITH_TIMEOUT(f.inbox.requests().isEmpty(),qPrintable(f.inbox.errorString()),3000);
        const auto result=f.pending.front().get();QVERIFY2(!result.isError,qPrintable(result.text));
        QCOMPARE(result.data["answers"].toObject()["__proto__"],"Qt, Web, 직접 입력\n두 번째 줄");
        QCOMPARE(result.data["annotations"].toObject()["__proto__"].toObject()["notes"],"검토 메모\n두 번째 줄");
        QVERIFY2(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void newRequestDoesNotResetFocusOrDraftAndSkipAdvances() {
        Fixture f;QVERIFY(f.window);f.ask();
        QTRY_VERIFY(f.object("agentQuestionOther_0"));QTRY_VERIFY(f.object("agentQuestionsSheet")->property("opened").toBool());
        QVERIFY(f.type("agentQuestionOther_0","Draft"));auto* editor=f.window->activeFocusItem();QVERIFY(editor);
        f.ask(question("Second?"));QTRY_COMPARE(f.inbox.requests().size(),2);
        QCOMPARE(f.window->activeFocusItem(),editor);QCOMPARE(f.object("agentQuestionOther_0")->property("text").toString(),"Draft");
        QVERIFY(f.click("agentQuestionSkip"));QTRY_COMPARE(f.inbox.requests().size(),1);
        QCOMPARE(f.pending.front().get().data["answers"].toObject(),QJsonObject{});
        QTRY_COMPARE(f.object("agentQuestionText_0")->property("text").toString(),"Second?");
        QCOMPARE(f.object("agentQuestionOther_0")->property("text").toString(),"");
        QVERIFY(f.click("agentQuestionDecline"));QTRY_VERIFY(f.inbox.requests().isEmpty());QVERIFY(f.pending.back().get().isError);
        QVERIFY2(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void cancellationClosesAndEscapeDeclines() {
        Fixture f;QVERIFY(f.window);CancellationToken token;f.ask(question(),token);
        QTRY_VERIFY(f.object("agentQuestionOther_0"));token.cancel();QTRY_VERIFY(f.inbox.requests().isEmpty());
        QTRY_VERIFY(!f.object("agentQuestionsSheet")->property("visible").toBool());QVERIFY(f.pending.front().get().isError);
        f.ask();QTRY_VERIFY(f.object("agentQuestionOther_0"));QTRY_VERIFY(f.object("agentQuestionsSheet")->property("opened").toBool());
        QTest::keyClick(f.window,Qt::Key_Escape);QTRY_VERIFY(f.inbox.requests().isEmpty());QVERIFY(f.pending.back().get().isError);
        QVERIFY2(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void fourQuestionsAllowPartialFreeAnswers() {
        Fixture f;QVERIFY(f.window);auto input=question("First?");auto rows=input["questions"].toArray();
        for(const auto& text:{"Second?","Third?","Fourth?"})rows.append(question(text)["questions"].toArray().first());
        input["questions"]=rows;f.ask(input);QTRY_VERIFY(f.object("agentQuestionOther_3"));
        QTRY_VERIFY(f.object("agentQuestionsSheet")->property("opened").toBool());
        QVERIFY(f.click("agentQuestionChoice_0_0"));QVERIFY(f.type("agentQuestionOther_0","My own renderer"));
        QVERIFY(!f.object("agentQuestionChoice_0_0")->property("checked").toBool());
        auto* viewport=f.object("sheet_viewport");QVERIFY(viewport);
        QVERIFY(viewport->property("contentHeight").toReal()>viewport->property("height").toReal());
        viewport->setProperty("contentY",viewport->property("contentHeight").toReal()-viewport->property("height").toReal());
        QVERIFY(f.click("agentQuestionSubmit"));QTRY_VERIFY(f.inbox.requests().isEmpty());
        const auto result=f.pending.front().get();QVERIFY2(!result.isError,qPrintable(result.text));
        QCOMPARE(result.data["answers"].toObject(),(QJsonObject{{"First?","My own renderer"}}));
        QVERIFY(result.data["annotations"].toObject().isEmpty());
        QVERIFY2(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
    void multipleSelectionsDoNotUseAnUnselectedPreview() {
        Fixture f;QVERIFY(f.window);auto input=question("Renderer?",true);auto row=input["questions"].toArray().first().toObject();
        auto choices=row["options"].toArray();choices.append(QJsonObject{{"label","Qt, Web"},{"description","Another choice"},{"preview","Must not be attached"}});
        row["options"]=choices;input["questions"]=QJsonArray{row};f.ask(input);
        QTRY_VERIFY(f.object("agentQuestionOption_0_2"));QTRY_VERIFY(f.object("agentQuestionsSheet")->property("opened").toBool());
        QVERIFY(f.click("agentQuestionOption_0_0"));QVERIFY(f.click("agentQuestionOption_0_1"));
        QVERIFY(f.click("agentQuestionSubmit"));QTRY_VERIFY(f.inbox.requests().isEmpty());
        const auto result=f.pending.front().get();QVERIFY(!result.isError);
        QCOMPARE(result.data["answers"].toObject()["Renderer?"],"Qt, Web");QVERIFY(result.data["annotations"].toObject().isEmpty());
        QVERIFY2(f.warnings.isEmpty(),qPrintable(f.warnings.join('\n')));
    }
};
QTEST_MAIN(QuestionSheetTests)
#include "question_sheet_tests.moc"
