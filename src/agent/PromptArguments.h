#pragma once
#include "Types.h"
namespace iiLocalLLM::agent::detail {
// Shared non-executing argument tokenizer. It never expands environment values.
inline QStringList splitPromptArguments(const QString& text) {
    QStringList result;QString part;QChar quote;bool escape=false,started=false;
    for(const auto c:text) {
        if(escape){part+=c;escape=false;started=true;}
        else if(c=='\\'&&quote!='\''){escape=true;started=true;}
        else if(!quote.isNull()){if(c==quote)quote={};else part+=c;}
        else if(c=='\''||c=='"'){quote=c;started=true;}
        else if(c.isSpace()){if(started){result.append(part);part.clear();started=false;}}
        else{part+=c;started=true;}
    }
    if(!quote.isNull()||escape)throw Error(ErrorCode::InvalidArgument,"Unclosed quote or escape in prompt arguments");
    if(started)result.append(part);return result;
}
}
