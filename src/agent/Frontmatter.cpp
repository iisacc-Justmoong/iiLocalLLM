#include "Frontmatter.h"
#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringDecoder>
#include <yaml.h>
namespace iiLocalLLM::agent::detail {
namespace {
void require(bool ok,const QString& message){if(!ok)throw Error(ErrorCode::ProtocolError,message);}
QString decode(const QByteArray& bytes){QStringDecoder decoder(QStringDecoder::Utf8);QString s=decoder(bytes);require(!decoder.hasError()&&!s.contains(QChar::Null),"Frontmatter must be valid UTF-8 without NUL");return s;}
struct Parser {
    yaml_parser_t value{};
    explicit Parser(const QByteArray& bytes){require(yaml_parser_initialize(&value),"Cannot create YAML parser");yaml_parser_set_input_string(&value,reinterpret_cast<const unsigned char*>(bytes.constData()),size_t(bytes.size()));}
    ~Parser(){yaml_parser_delete(&value);}
};
}
FrontmatterDocument readFrontmatter(const QByteArray& bytes,const CancellationToken& token) {
    token.throwIfCancelled();auto body=decode(bytes.startsWith("\xEF\xBB\xBF")?bytes.mid(3):bytes);body.replace("\r\n","\n");
    const auto opening=QRegularExpression("^---[ \\t]*\\n").match(body);
    if(!opening.hasMatch())return {{},body};
    const auto start=opening.capturedEnd(),end=body.indexOf(QRegularExpression("(?m)^---[ \\t]*(?:\\n|$)"),start);
    require(end>=0,"Unclosed frontmatter");const auto yaml=body.mid(start,end-start).toUtf8();require(yaml.size()<=16384,"Frontmatter exceeds 16 KiB");
    Parser events(yaml);int depth=0,count=0,documents=0;
    for(;;){
        token.throwIfCancelled();yaml_event_t event{};require(yaml_parser_parse(&events.value,&event),"Malformed YAML frontmatter");
        const auto type=event.type;yaml_event_delete(&event);
        if(type==YAML_DOCUMENT_START_EVENT)++documents;
        if(type==YAML_SEQUENCE_START_EVENT||type==YAML_MAPPING_START_EVENT)++depth;
        if(type==YAML_SEQUENCE_END_EVENT||type==YAML_MAPPING_END_EVENT)--depth;
        require(type!=YAML_ALIAS_EVENT&&documents<=1&&depth<=16&&++count<=4096,"YAML aliases, depth, event or document limit exceeded");
        if(type==YAML_STREAM_END_EVENT)break;
    }
    Parser parser(yaml);struct Document {yaml_document_t value{};~Document(){yaml_document_delete(&value);}} doc;
    require(yaml_parser_load(&parser.value,&doc.value),"Malformed YAML frontmatter");
    std::function<QJsonValue(yaml_node_t*)> convert=[&](yaml_node_t* node)->QJsonValue {
        token.throwIfCancelled();require(node,"Missing YAML node");
        if(node->type==YAML_SCALAR_NODE){
            const auto text=decode(QByteArray(reinterpret_cast<const char*>(node->data.scalar.value),qsizetype(node->data.scalar.length)));
            if(node->data.scalar.style==YAML_PLAIN_SCALAR_STYLE&&(text.isEmpty()||text=="~"||text.compare("null",Qt::CaseInsensitive)==0))return QJsonValue::Null;
            return text;
        }
        if(node->type==YAML_SEQUENCE_NODE){QJsonArray array;for(auto p=node->data.sequence.items.start;p!=node->data.sequence.items.top;++p)array.append(convert(yaml_document_get_node(&doc.value,*p)));return array;}
        require(node->type==YAML_MAPPING_NODE,"Invalid YAML node type");QJsonObject object;
        for(auto p=node->data.mapping.pairs.start;p!=node->data.mapping.pairs.top;++p){
            const auto key=convert(yaml_document_get_node(&doc.value,p->key));require(key.isString()&&!key.toString().isEmpty()&&key.toString().size()<=128&&!object.contains(key.toString()),"Invalid or duplicate YAML key");
            object[key.toString()]=convert(yaml_document_get_node(&doc.value,p->value));
        }
        return object;
    };
    auto* root=yaml_document_get_root_node(&doc.value);const auto fields=root?convert(root):QJsonValue(QJsonObject{});require(fields.isObject(),"Frontmatter must be a mapping");
    const auto newline=body.indexOf('\n',end);return {fields.toObject(),newline<0?QString():body.mid(newline+1)};
}
}
