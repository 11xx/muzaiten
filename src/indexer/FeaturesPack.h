#pragma once

#include <QJsonObject>
#include <QStringList>

#include <stdexcept>

// Local scalar exchange belongs to the feature-store writer, never app readers.
namespace FeaturesPack {

inline constexpr qint64 MaximumBytes = 64 * 1024 * 1024;
inline constexpr int MaximumRecords = 100'000;
inline constexpr qint64 MaximumLineBytes = 16 * 1024;

class Error final : public std::runtime_error {
public:
    Error(int code, const QString &message);
    int code() const { return m_code; }

private:
    int m_code;
};

QString contentsDescription();
QJsonObject run(QStringList arguments);

} // namespace FeaturesPack
