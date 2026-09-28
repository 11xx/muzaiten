#include "search/fold/Fold.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    const bool census = argc == 2 && std::string_view(argv[1]) == "--census";
    if (argc != 1 && !census) {
        std::cerr << "usage: fold-inspect [--census]\n";
        return 2;
    }
    qint64 values = 0, positions = 0, variants = 0, capHits = 0, alternateUnits = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const QString input = QString::fromUtf8(line.data(), line.size());
        if (census) {
            const auto count = Search::Fold::census(input);
            ++values;
            positions += count.positions;
            variants += count.variants;
            capHits += count.capped;
            alternateUnits += count.alternateUnits;
            const QJsonObject row{{"value", input}, {"positions", count.positions},
                                  {"product", QString::number(count.product)},
                                  {"variants", count.variants}, {"capped", count.capped},
                                  {"alternateUnits", double(count.alternateUnits)}};
            const auto bytes = QJsonDocument(row).toJson(QJsonDocument::Compact);
            std::cout.write(bytes.constData(), bytes.size());
            std::cout.put('\n');
            continue;
        }
        const auto folded = Search::Fold::foldSegments(input);
        const auto bytes = folded.toUtf8();
        std::cout.write(bytes.constData(), bytes.size());
        std::cout.put('\n');
    }
    if (census) {
        const QJsonObject totals{{"values", double(values)}, {"positions", double(positions)},
                                 {"variants", double(variants)}, {"capHits", double(capHits)},
                                 {"alternateUnits", double(alternateUnits)}};
        const auto bytes = QJsonDocument(totals).toJson(QJsonDocument::Compact);
        std::cerr.write(bytes.constData(), bytes.size());
        std::cerr.put('\n');
    }
    return std::cin.bad() || !std::cout ? 1 : 0;
}
