#pragma once
#include "Tools.h"
#include <QtCore/QFileInfo>

namespace iiLocalLLM::agent::detail {
// Child engines have no PlanMode transitions. Their workspace adapters must
// nevertheless retain the parent's private plan directory before preparation
// and inside Read/Glob/Grep execution. A verifier may read its owner's plan;
// ordinary subagents receive no plan-file exception.
inline Tool protectPlanningFiles(Tool tool,QString directory,QString ownedFile={}) {
    if(directory.isEmpty()||tool.definition.metadata["source"]!="builtin.workspace")return tool;
    const auto canonical=QFileInfo(directory).canonicalFilePath();if(!canonical.isEmpty())directory=canonical;
    const auto bind=[directory=std::move(directory),ownedFile=std::move(ownedFile)](ToolContext c) {
        c.plansDirectory=directory;c.planFilePath=ownedFile;c.planModeActive=false;return c;
    };
    if(tool.validate)tool.validate=[validate=tool.validate,bind](const auto& args,const auto& c){validate(args,bind(c));};
    if(tool.prepare)tool.prepare=[prepare=tool.prepare,bind](const auto& args,const auto& c){return prepare(args,bind(c));};
    if(tool.execute)tool.execute=[execute=tool.execute,bind](const auto& args,const auto& c){return execute(args,bind(c));};
    return tool;
}
}
