#include "ModelDigest.h"
#include "ModelReadAhead.h"
#include <QtCore/QTemporaryFile>
#include <QtCore/QCryptographicHash>
#include <algorithm>
#include <iostream>
using iiLocalLLM::detail::ModelDigest;
int main() {
    struct Vector {QByteArray text;QByteArray expected;};
    const Vector vectors[]{
        {{},"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {QByteArray(1000000,'a'),"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
    for(const auto& vector:vectors) {
        ModelDigest whole;whole.addData(vector.text);
        if(whole.result().toHex()!=vector.expected||whole.result().toHex()!=vector.expected)return 1;
        for(const qsizetype block:{qsizetype(1),qsizetype(31),qsizetype(65536)}) {
            ModelDigest chunked;
            for(qsizetype offset=0;offset<vector.text.size();offset+=block)
                chunked.addData(QByteArrayView(vector.text).sliced(offset,std::min(block,vector.text.size()-offset)));
            if(chunked.result().toHex()!=vector.expected)return 2;
        }
    }
    ModelDigest continued;continued.addData("a");(void)continued.result();continued.addData("bc");
    if(continued.result().toHex()!=vectors[1].expected)return 3;
    QTemporaryFile file;if(!file.open())return 4;
    const QByteArray payload(2*iiLocalLLM::detail::ModelReadAhead::windowBytes+17,'x');
    if(file.write(payload)!=payload.size()||!file.flush()||!file.seek(0))return 5;
    iiLocalLLM::detail::ModelReadAhead readAhead(file);ModelDigest hinted;
    for(qint64 offset=0;offset<payload.size();) {
        const auto before=file.pos();readAhead.prepare(offset);if(file.pos()!=before)return 6;
        const auto bytes=file.read(1024*1024);if(bytes.isEmpty())return 7;
        hinted.addData(bytes);offset+=bytes.size();
    }
    if(hinted.result()!=QCryptographicHash::hash(payload,QCryptographicHash::Sha256))return 8;
    readAhead.prepare(-1);readAhead.prepare(payload.size());file.close();readAhead.prepare(0);
    std::cout<<"Complete SHA-256 vectors and streaming boundaries verified\n";
}
