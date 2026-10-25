#include "runtimes/ToolTurn.h"
#include <iostream>

int main() {
    using iiLocalLLM::detail::completeSingleToolTurn;
    const QString call=R"({"name":"Read","arguments":{"path":"a.txt"}})";
    const auto complete="<tool_call>"+call+"</tool_call>";
    if(completeSingleToolTurn({},complete,{"Read"})!=complete.size())return 1;
    const auto prefilled=call+"</tool_call>";
    if(completeSingleToolTurn("<|im_start|>assistant\n<tool_call>\n",prefilled,{"Read"})!=prefilled.size())return 2;
    if(completeSingleToolTurn({},prefilled,{"Read"})!=prefilled.size())return 9;
    if(completeSingleToolTurn({},"<tool_call>{\"name\":\"Read\",\"arguments\":{",{"Read"}))return 3;
    if(completeSingleToolTurn({},R"(<tool_call>{"name":"Read","arguments":{"path":"</tool_call>)",{"Read"}))return 4;
    const QString quoted=R"(<tool_call>{"name":"Read","arguments":{"path":"</tool_call>"}}</tool_call>)";
    if(completeSingleToolTurn({},quoted,{"Read"})!=quoted.size())return 5;
    if(completeSingleToolTurn({},complete+" unobserved speculation",{"Read"})!=complete.size())return 6;
    if(completeSingleToolTurn({},complete,{"Other"}))return 7;
    if(completeSingleToolTurn({},"Ordinary final answer",{"Read"}))return 8;
    std::cout<<"Complete single tool envelopes yield before speculative continuation; partial JSON never yields.\n";
}
