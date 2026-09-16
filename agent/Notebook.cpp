#include "Notebook.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringConverter>
#include <QtCore/QUuid>

namespace iiLocalLLM::agent {
namespace {
constexpr qsizetype maximumBytes=1024*1024;
void require(bool value,const QString& message,ErrorCode code=ErrorCode::InvalidArgument) {
    if(!value)throw Error(code,message);
}
bool integer(const QJsonValue& value) {
    return value.isDouble()&&value.toDouble()==value.toInteger()&&value.toInteger()>=0;
}
void source(const QJsonValue& value) {
    if(value.isString())return;
    require(value.isArray(),"Notebook cell source must be a string or string array");
    for(const auto& line:value.toArray())require(line.isString(),"Notebook cell source contains a non-string line");
}
bool knownType(const QString& value) {return value=="code"||value=="markdown"||value=="raw";}
QJsonObject notebook(const QByteArray& bytes,const CancellationToken& token) {
    require(bytes.size()<=maximumBytes,"Notebook exceeds 1 MiB",ErrorCode::ResourceLimit);
    QStringDecoder decoder(QStringDecoder::Utf8);const QString text=decoder(bytes);Q_UNUSED(text);
    require(!decoder.hasError(),"Notebook is not valid UTF-8");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(bytes,&error);
    require(error.error==QJsonParseError::NoError&&document.isObject(),"Notebook is not a JSON object");
    const auto result=document.object();
    require(result["nbformat"].isDouble()&&result["nbformat"].toDouble()==4&&integer(result["nbformat_minor"]),"Only nbformat 4 notebooks are supported");
    require(result["metadata"].isObject()&&result["cells"].isArray(),"Notebook requires metadata and cells");
    const auto cells=result["cells"].toArray();require(cells.size()<=10000,"Notebook exceeds 10000 cells",ErrorCode::ResourceLimit);
    const bool idsRequired=result["nbformat_minor"].toInteger()>=5;
    static const QRegularExpression validId("\\A[A-Za-z0-9_-]{1,64}\\z");QSet<QString> ids;
    for(const auto& value:cells) {
        token.throwIfCancelled();require(value.isObject(),"Notebook contains a non-object cell");const auto cell=value.toObject();
        require(cell["cell_type"].isString()&&!cell["cell_type"].toString().isEmpty(),"Notebook cell type is missing");
        if(idsRequired||cell.contains("id")) {
            require(cell["id"].isString()&&!cell["id"].toString().isEmpty(),"Notebook cell ID is missing or invalid");const auto id=cell["id"].toString();
            require(!idsRequired||validId.match(id).hasMatch(),"Notebook cell ID is invalid for nbformat 4.5");
            require(!ids.contains(id),"Notebook has duplicate cell IDs");ids.insert(id);
        }
        // Preserve unrecognized future cell types without interpreting or
        // normalizing their payload. Editing one explicitly is rejected below.
        if(!knownType(cell["cell_type"].toString()))continue;
        require(cell["metadata"].isObject(),"Notebook cell metadata must be an object");source(cell["source"]);
        if(cell["cell_type"]=="code") {
            require(cell["outputs"].isArray(),"Code cell outputs must be an array");
            for(const auto& output:cell["outputs"].toArray())require(output.isObject(),"Code cell output must be an object");
            require(cell["execution_count"].isNull()||integer(cell["execution_count"]),"Code cell execution count must be null or a nonnegative integer");
        }else if(cell.contains("attachments"))require(cell["attachments"].isObject(),"Cell attachments must be an object");
    }
    return result;
}
}
void validateNotebookEditArguments(const QJsonObject& args) {
    for(auto it=args.begin();it!=args.end();++it)require(QStringList{"notebook_path","new_source","cell_id","cell_type","edit_mode"}.contains(it.key()),"Unknown notebook edit argument: "+it.key());
    require(args["notebook_path"].isString()&&!args["notebook_path"].toString().isEmpty()&&!args["notebook_path"].toString().contains(QChar::Null)
        &&args["notebook_path"].toString().size()<=4096,"A notebook path is required");
    require(args["notebook_path"].toString().endsWith(".ipynb"),"Notebook path must end in .ipynb");
    require(args["new_source"].isString(),"new_source must be a string, including for delete");
    require(args["new_source"].toString().toUtf8().size()<=maximumBytes,"Notebook source exceeds 1 MiB",ErrorCode::ResourceLimit);
    const auto mode=args["edit_mode"].toString("replace");
    require(!args.contains("edit_mode")||(args["edit_mode"].isString()&&QStringList{"replace","insert","delete"}.contains(mode)),"Invalid notebook edit mode");
    require(!args.contains("cell_type")||(args["cell_type"].isString()&&QStringList{"code","markdown"}.contains(args["cell_type"].toString())),"cell_type must be code or markdown");
    require(mode!="insert"||args.contains("cell_type"),"Inserting a cell requires cell_type");
    require(!args.contains("cell_id")||(args["cell_id"].isString()&&!args["cell_id"].toString().isEmpty()&&args["cell_id"].toString().size()<=1024),"Invalid cell_id");
    require(mode=="insert"||args.contains("cell_id"),"Replacing or deleting a cell requires cell_id");
}
NotebookEditResult editNotebook(const QByteArray& before,const QJsonObject& args,const CancellationToken& token) {
    token.throwIfCancelled();validateNotebookEditArguments(args);auto value=notebook(before,token);auto cells=value["cells"].toArray();
    const auto mode=args["edit_mode"].toString("replace"),selector=args["cell_id"].toString();int index=0;
    if(args.contains("cell_id")) {
        index=-1;for(qsizetype i=0;i<cells.size();++i)if(cells[i].toObject()["id"]==selector){index=int(i);break;}
        if(index<0) {
            static const QRegularExpression alias("\\Acell-([0-9]+)\\z");const auto match=alias.match(selector);bool ok=false;
            const auto number=match.hasMatch()?match.captured(1).toULongLong(&ok):0;
            require(ok&&number<quint64(cells.size()),"Notebook cell ID or index was not found");index=int(number);
        }
        if(mode=="insert")++index;
    }
    QString id,type;
    if(mode=="insert") {
        require(cells.size()<10000,"Notebook exceeds 10000 cells",ErrorCode::ResourceLimit);
        type=args["cell_type"].toString();QJsonObject cell{{"cell_type",type},{"source",args["new_source"]},{"metadata",QJsonObject{}}};
        if(value["nbformat_minor"].toInteger()>=5) {
            QSet<QString> ids;for(const auto& item:cells)ids.insert(item.toObject()["id"].toString());
            do{id=QUuid::createUuid().toString(QUuid::WithoutBraces);}while(ids.contains(id));cell["id"]=id;
        }
        if(type=="code"){cell["execution_count"]=QJsonValue::Null;cell["outputs"]=QJsonArray{};}
        cells.insert(index,cell);
    }else {
        auto cell=cells[index].toObject();const auto originalType=cell["cell_type"].toString();
        require(knownType(originalType),"Cannot edit an unsupported future notebook cell type");id=cell.value("id").toString();
        type=mode=="delete"?originalType:args["cell_type"].toString(originalType);
        if(mode=="delete")cells.removeAt(index);
        else {
            cell["source"]=args["new_source"];cell["cell_type"]=type;
            if(type=="code") {cell["outputs"]=QJsonArray{};cell["execution_count"]=QJsonValue::Null;cell.remove("attachments");}
            else {cell.remove("outputs");cell.remove("execution_count");}
            cells[index]=cell;
        }
    }
    if(id.isEmpty())id="cell-"+QString::number(index);
    value["cells"]=cells;token.throwIfCancelled();const auto content=QJsonDocument(value).toJson(QJsonDocument::Indented);
    require(content.size()<=maximumBytes,"Edited notebook exceeds 1 MiB",ErrorCode::ResourceLimit);
    auto language=value["metadata"].toObject()["language_info"].toObject()["name"].toString();if(language.isEmpty())language="python";
    return {content,{{"cell_id",id},{"cell_index",index},{"cell_type",type},{"new_source",args["new_source"]},{"language",language},
        {"edit_mode",mode},{"cell_count",cells.size()},{"nbformat",4},{"nbformat_minor",value["nbformat_minor"]}}};
}
}
