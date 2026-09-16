#include "WebContent.h"
#include <QtCore/QRegularExpression>
#include <lexbor/html/html.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/encoding/encoding.h>
#include <memory>

namespace iiLocalLLM::agent::detail {
QString webExcerpt(QString value,int maximum) {
    if(value.size()>maximum){value.truncate(maximum);if(!value.isEmpty()&&value.back().isHighSurrogate())value.chop(1);value.squeeze();}return value;
}
namespace {
void require(bool value,const char* message){if(!value)throw Error(ErrorCode::ResourceLimit,message);}
QString decode(const QByteArray& bytes,const QByteArray& type,const CancellationToken& token) {
    static const QRegularExpression charset("charset\\s*=\\s*[\"']?([^;\\s\"']+)",QRegularExpression::CaseInsensitiveOption);
    const auto match=charset.match(QString::fromLatin1(type));auto name=match.hasMatch()?match.captured(1).toLatin1():QByteArray("utf-8");
    const auto* begin=reinterpret_cast<const lxb_char_t*>(bytes.constData());const auto* end=begin+bytes.size();
    const auto bom=lxb_encoding_bom_sniff(begin,size_t(bytes.size()));
    if(bom==LXB_ENCODING_DEFAULT&&!match.hasMatch()&&type.toLower().startsWith("text/html")) {
        lxb_html_encoding_t scan{};require(lxb_html_encoding_init(&scan)==LXB_STATUS_OK,"Cannot initialize HTML encoding detector");
        size_t length=0;const auto* detected=lxb_html_encoding_prescan(&scan,begin,begin+std::min<qsizetype>(bytes.size(),1024),&length);
        if(detected)name=QByteArray(reinterpret_cast<const char*>(detected),qsizetype(length));lxb_html_encoding_destroy(&scan,false);
    }
    const auto* encoding=bom==LXB_ENCODING_DEFAULT?lxb_encoding_data_by_pre_name(reinterpret_cast<const lxb_char_t*>(name.constData()),size_t(name.size())):lxb_encoding_data(bom);
    if(!encoding||encoding->encoding==LXB_ENCODING_REPLACEMENT)throw Error(ErrorCode::ProtocolError,"Unsupported web character encoding");
    if(bom==LXB_ENCODING_UTF_8)begin+=3;else if(bom==LXB_ENCODING_UTF_16BE||bom==LXB_ENCODING_UTF_16LE)begin+=2;
    lxb_codepoint_t buffer[4096],replacement=0xfffd;lxb_encoding_decode_t decoder;
    require(lxb_encoding_decode_init(&decoder,encoding,buffer,4096)==LXB_STATUS_OK,"Cannot initialize web decoder");
    lxb_encoding_decode_replace_set(&decoder,&replacement,1);QString result;
    auto drain=[&]{char32_t points[4096];for(size_t i=0;i<decoder.buffer_used;++i)points[i]=buffer[i];result+=QString::fromUcs4(points,qsizetype(decoder.buffer_used));decoder.buffer_used=0;};
    while(begin<end){token.throwIfCancelled();const auto status=encoding->decode(&decoder,&begin,end);drain();
        require(status==LXB_STATUS_OK||status==LXB_STATUS_SMALL_BUFFER||status==LXB_STATUS_CONTINUE,"Cannot decode web response");}
    require(lxb_encoding_decode_finish(&decoder)==LXB_STATUS_OK,"Cannot finish web decoder");drain();return result;
}
QString escaped(const QString& value){QString result;result.reserve(value.size());
    for(qsizetype i=0;i<value.size();++i){const auto c=value[i];const bool intraword=c=='_'&&i>0&&i+1<value.size()
        &&value[i-1].isLetterOrNumber()&&value[i+1].isLetterOrNumber();
        if(QString("\\`*_[]").contains(c)&&!intraword)result+='\\';result+=c;}return result;}
class Markdown {
    const QUrl base;const int limit;const CancellationToken& token;int nodes=0;QString output;
    void append(const QString& text){if(output.size()<=limit)output+=webExcerpt(text,limit+1-int(output.size()));}
    void line(int count=2){int present=0;for(qsizetype i=output.size();i>0&&output[i-1]=='\n';--i)++present;append(QString(std::max(0,count-present),'\n'));}
    QString attr(lxb_dom_node_t* node,const char* key){size_t size=0;const auto* text=lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
        reinterpret_cast<const lxb_char_t*>(key),strlen(key),&size);return text?QString::fromUtf8(reinterpret_cast<const char*>(text),qsizetype(size)):QString();}
    QString url(QString value){const auto resolved=base.resolved(QUrl(value,QUrl::StrictMode));
        if(!resolved.isValid()||(resolved.scheme()!="http"&&resolved.scheme()!="https"&&resolved.scheme()!="mailto")||!resolved.userInfo().isEmpty())return {};
        return resolved.toString(QUrl::FullyEncoded).replace('(',"%28").replace(')',"%29");}
    void children(lxb_dom_node_t* node,int depth,bool pre){for(auto* c=node->first_child;c&&output.size()<=limit;c=c->next)walk(c,depth+1,pre);}
    void walk(lxb_dom_node_t* node,int depth,bool pre=false) {
        token.throwIfCancelled();require(depth<=256&&++nodes<=200000,"Web HTML tree exceeds traversal limits");
        if(node->type==LXB_DOM_NODE_TYPE_TEXT){auto* data=lxb_dom_interface_character_data(node);QString text=QString::fromUtf8(reinterpret_cast<const char*>(data->data.data),qsizetype(data->data.length));
            if(!pre){text.replace(QRegularExpression("[\\s\\x{00a0}]+")," ");text=escaped(text);if((output.isEmpty()||output.back().isSpace())&&text.startsWith(' '))text.remove(0,1);}append(text);return;}
        if(node->type!=LXB_DOM_NODE_TYPE_ELEMENT){children(node,depth,pre);return;}
        const auto tag=node->local_name;
        if(tag==LXB_TAG_SCRIPT||tag==LXB_TAG_STYLE||tag==LXB_TAG_HEAD||tag==LXB_TAG_TEMPLATE||tag==LXB_TAG_NOSCRIPT)return;
        if(lxb_dom_element_has_attribute(lxb_dom_interface_element(node),reinterpret_cast<const lxb_char_t*>("hidden"),6))return;
        const bool heading=tag>=LXB_TAG_H1&&tag<=LXB_TAG_H6;
        const bool block=heading||tag==LXB_TAG_P||tag==LXB_TAG_DIV||tag==LXB_TAG_SECTION||tag==LXB_TAG_ARTICLE||tag==LXB_TAG_UL||tag==LXB_TAG_OL||tag==LXB_TAG_TABLE||tag==LXB_TAG_BLOCKQUOTE;
        if(block)line();if(heading)append(QString(int(tag-LXB_TAG_H1)+1,'#')+' ');
        if(tag==LXB_TAG_BR){line(1);return;}if(tag==LXB_TAG_HR){line();append("---");line();return;}
        if(tag==LXB_TAG_IMG){auto label=escaped(attr(node,"alt")),target=url(attr(node,"src"));if(!label.isEmpty())append(target.isEmpty()?label:"!["+label+"]("+target+")");return;}
        if(tag==LXB_TAG_LI){line(1);append("- ");}if(tag==LXB_TAG_TR)line(1);
        if(tag==LXB_TAG_TD||tag==LXB_TAG_TH)append(" | ");
        if(tag==LXB_TAG_PRE){line();append("````\n");children(node,depth,true);line(1);append("````");line();return;}
        const auto marker=(tag==LXB_TAG_STRONG||tag==LXB_TAG_B)?QString("**"):(tag==LXB_TAG_EM||tag==LXB_TAG_I)?QString("*"):
            tag==LXB_TAG_CODE&&!pre?QString("`"):QString();
        const auto link=tag==LXB_TAG_A?url(attr(node,"href")):QString();
        if(!link.isEmpty())append("[");append(marker);children(node,depth,pre);append(marker);
        if(!link.isEmpty())append("]("+link+")");if(block)line();
    }
public:
    Markdown(QUrl url,int maximum,const CancellationToken& t):base(std::move(url)),limit(maximum),token(t){}
    QString convert(lxb_dom_node_t* root,bool& truncated){walk(root,0);truncated=output.size()>limit;return webExcerpt(output,limit).trimmed();}
};
}
QString webContent(const QByteArray& bytes,const QByteArray& type,const QUrl& url,int maximum,bool& truncated,const CancellationToken& token) {
    const auto text=decode(bytes,type,token);if(!type.toLower().startsWith("text/html")&&!type.toLower().startsWith("application/xhtml+xml")){
        truncated=text.size()>maximum;return webExcerpt(text,maximum);}
    std::unique_ptr<lxb_html_parser_t,decltype(&lxb_html_parser_destroy)> parser(lxb_html_parser_create(),lxb_html_parser_destroy);
    require(parser&&lxb_html_parser_init(parser.get())==LXB_STATUS_OK,"Cannot initialize HTML parser");
    std::unique_ptr<lxb_html_document_t,decltype(&lxb_html_document_destroy)> document(lxb_html_parse_chunk_begin(parser.get()),lxb_html_document_destroy);
    require(bool(document),"Cannot allocate HTML document");const auto data=text.toUtf8();auto* tokenizer=lxb_html_parser_tokenizer(parser.get());
    struct Budget {int tokens=0;const CancellationToken& token;lxb_html_tokenizer_token_f next;void* context;};
    Budget budget{0,token,tokenizer->callback_token_done,tokenizer->callback_token_ctx};
    lxb_html_tokenizer_callback_token_done_set(tokenizer,[](lxb_html_tokenizer_t* tokenizer,lxb_html_token_t* htmlToken,void* state)->lxb_html_token_t* {
        auto& budget=*static_cast<Budget*>(state);
        if(++budget.tokens>200000||budget.token.isCancelled()||lxb_html_tokenizer_tree(tokenizer)->open_elements->length>256){
            lxb_html_tokenizer_status_set(tokenizer,LXB_STATUS_ERROR);return nullptr;}
        return budget.next(tokenizer,htmlToken,budget.context);
    },&budget);
    for(qsizetype pos=0;pos<data.size();pos+=16384){token.throwIfCancelled();const auto status=lxb_html_parse_chunk_process(parser.get(),
        reinterpret_cast<const lxb_char_t*>(data.constData()+pos),size_t(std::min<qsizetype>(16384,data.size()-pos)));token.throwIfCancelled();
        require(status==LXB_STATUS_OK,"HTML parsing failed or exceeded its token limit");}
    require(lxb_html_parse_chunk_end(parser.get())==LXB_STATUS_OK,"Cannot finish HTML parser");
    return Markdown(url,maximum,token).convert(lxb_dom_interface_node(document->body),truncated);
}
}
