#include <agent/Plugins.h>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <iostream>
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);app.setApplicationName("iillm-plugins");
    app.setApplicationVersion(IILOCALLLM_APP_VERSION);
    QCommandLineParser parser;parser.setApplicationDescription("Manage explicitly selected local iiLocalLLM plugins. Host restart applies a new snapshot.");
    parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"store","Private plugin store directory.","directory"});
    parser.addOption({"disabled","Install without enabling the plugin."});
    parser.addPositionalArgument("action","install, enable, disable, uninstall, list, or inspect");
    parser.addPositionalArgument("target","Package directory for install; plugin name for enable/disable/uninstall.","[target]");
    parser.process(app);
    try{
        const auto args=parser.positionalArguments();
        if(!parser.isSet("store")||parser.value("store").isEmpty()||args.isEmpty()||args.size()>2)
            throw std::runtime_error("--store and an action are required");
        const auto action=args[0];
        if(QStringList{"list","inspect"}.contains(action)?args.size()!=1:args.size()!=2)
            throw std::runtime_error("Wrong number of action arguments");
        if(parser.isSet("disabled")&&action!="install")throw std::runtime_error("--disabled requires install");
        iiLocalLLM::agent::PluginStore store({QDir().absoluteFilePath(parser.value("store"))});QJsonObject result;
        if(action=="install")result=store.install(QDir().absoluteFilePath(args[1]),!parser.isSet("disabled")).toJson(true);
        else if(action=="list")result={{"plugins",store.list()}};
        else if(action=="inspect")result=store.snapshot().toJson();
        else if(action=="enable"||action=="disable"){store.setEnabled(args[1],action=="enable");result={{"changed",true},{"reload_requires_restart",true}};}
        else if(action=="uninstall"){store.uninstall(args[1]);result={{"removed",true},{"cache_and_data_retained",true},{"reload_requires_restart",true}};}
        else throw std::runtime_error("Unknown plugin action");
        std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<std::endl;return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
