#include "search/fold/Fold.h"

#include <QByteArray>
#include <QString>

#include <iostream>
#include <string>

int main()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto folded = Search::Fold::foldSegments(QString::fromUtf8(line.data(), line.size()));
        const auto bytes = folded.toUtf8();
        std::cout.write(bytes.constData(), bytes.size());
        std::cout.put('\n');
    }
    return std::cin.bad() || !std::cout ? 1 : 0;
}
