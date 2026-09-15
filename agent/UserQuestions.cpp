#include "UserQuestions.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>

namespace iiLocalLLM::agent {
namespace {
void require(bool value,const QString& message) {if(!value)throw Error(ErrorCode::InvalidArgument,message);}
QJsonObject object(QJsonObject properties,QJsonArray required={}) {
    return {{"type","object"},{"additionalProperties",false},{"properties",properties},{"required",required}};
}
QJsonObject string(int max,int min=0) {return {{"type","string"},{"minLength",min},{"maxLength",max}};}
QJsonObject questionsSchema() {
    const auto option=object({{"label",string(512,1)},{"description",string(8192)},{"preview",string(32768)}},{"label","description"});
    const auto question=object({{"question",string(8192,1)},{"header",string(128,1)},
        {"options",QJsonObject{{"type","array"},{"minItems",2},{"maxItems",4},{"items",option}}},
        {"multiSelect",QJsonObject{{"type","boolean"},{"default",false}}}},{"question","header","options"});
    return {{"type","array"},{"minItems",1},{"maxItems",4},{"items",question}};
}
QJsonObject answersSchema() {return {{"type","object"},{"maxProperties",4},{"additionalProperties",string(32768)}};}
QJsonObject annotationsSchema() {return {{"type","object"},{"maxProperties",4},
    {"additionalProperties",object({{"preview",string(32768)},{"notes",string(32768)}})}};}
void noNul(const QJsonValue& value) {
    if(value.isString())require(!value.toString().contains(QChar::Null),"Question text cannot contain NUL");
    if(value.isArray())for(const auto& child:value.toArray())noNul(child);
    if(value.isObject()){const auto obj=value.toObject();for(auto i=obj.begin();i!=obj.end();++i){noNul(i.key());noNul(i.value());}}
}
QJsonObject identity(QJsonObject args) {args.remove("answers");args.remove("annotations");return args;}
void htmlFragment(const QString& value) {
    // Reference-compatible intent check, deliberately not an HTML sanitizer.
    static const QRegularExpression forbidden("<\\s*(html|body|!doctype|script|style)\\b",QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tag("<[a-z][^>]*>",QRegularExpression::CaseInsensitiveOption);
    require(!forbidden.match(value).hasMatch()&&tag.match(value).hasMatch(),
        "HTML preview must be a fragment with a tag, without document, script or style tags");
}
}
Tool userQuestionTool(UserQuestionOptions options) {
    require(options.previewFormat=="markdown"||options.previewFormat=="html","Unknown question preview format");
    Tool tool;tool.definition={"AskUserQuestion",
        "Ask the user 1 to 4 questions with 2 to 4 choices each. Use concise headers (12 characters recommended). "
        "The host also offers free text; do not add an Other choice. multiSelect permits multiple choices. "
        "Optional previews use "+options.previewFormat+". Only the host response may supply answers or annotations. "
        "An absent answer means that question was skipped.",
        object({{"questions",questionsSchema()},{"answers",answersSchema()},{"annotations",annotationsSchema()},
            {"metadata",object({{"source",string(512)}})}},{"questions"}),
        object({{"questions",questionsSchema()},{"answers",answersSchema()},{"annotations",annotationsSchema()}},{"questions","answers"}),
        true,true,false,options.deferred,{{"source","builtin.user-question"},{"requires_user_interaction",true},{"preview_format",options.previewFormat}}};
    tool.requiresPermission=true;
    tool.validate=[options](const QJsonObject& args,const ToolContext& c) {
        require(!c.verificationAgent,"Verification agents cannot ask user questions");
        require(QJsonDocument(args).toJson(QJsonDocument::Compact).size()<=262144,"Question payload exceeds 262144 bytes");noNul(args);
        QSet<QString> questions;
        for(const auto& value:args["questions"].toArray()) {
            const auto q=value.toObject();const auto text=q["question"].toString();
            require(!text.trimmed().isEmpty()&&!questions.contains(text),"Question text must be nonempty and unique");questions.insert(text);
            require(!q["header"].toString().trimmed().isEmpty(),"Question header must be nonempty");QSet<QString> labels;
            for(const auto& item:q["options"].toArray()) {
                const auto option=item.toObject();const auto label=option["label"].toString();
                require(!label.trimmed().isEmpty()&&!labels.contains(label),"Option labels must be nonempty and unique within a question");labels.insert(label);
                if(options.previewFormat=="html"&&option.contains("preview"))htmlFragment(option["preview"].toString());
            }
        }
        const auto approved=c.approvedToolPreview["user_question"].toObject();
        if(approved.isEmpty())require(!args.contains("answers")&&!args.contains("annotations"),"Only the host response may supply answers or annotations");
        else require(approved["input"]==identity(args)&&approved["preview_format"]==options.previewFormat,
            "A question response cannot change the reviewed questions or metadata");
        const auto answers=args["answers"].toObject(),annotations=args["annotations"].toObject();
        for(auto i=answers.begin();i!=answers.end();++i)require(questions.contains(i.key()),"Answer refers to an unknown question");
        for(auto i=annotations.begin();i!=annotations.end();++i) {
            require(questions.contains(i.key()),"Annotation refers to an unknown question");
            if(options.previewFormat=="html"&&i.value().toObject().contains("preview"))htmlFragment(i.value().toObject()["preview"].toString());
        }
    };
    tool.execute=[](const QJsonObject& args,const ToolContext& c) {
        c.cancellation.throwIfCancelled();QJsonObject result{{"questions",args["questions"]},{"answers",args.value("answers").toObject()}};
        if(args.contains("annotations"))result["annotations"]=args["annotations"];
        return ToolResult{"User question response (missing answers were skipped):\n"+QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)),result};
    };
    tool.prepare=[definition=tool.definition,execute=tool.execute,options](const QJsonObject& args,const ToolContext& c) {
        auto preview=definition;preview.metadata["user_question"]=QJsonObject{{"schema","iisacc.user-question/1"},
            {"input",identity(args)},{"preview_format",options.previewFormat},{"free_text",true},{"partial_answers",true},
            {"response_field","updatedInput"},{"max_input_bytes",262144}};
        return PreparedTool{preview,[execute,args,c]{return execute(args,c);}};
    };
    return tool;
}
}
