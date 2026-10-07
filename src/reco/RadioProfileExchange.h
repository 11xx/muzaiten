#pragma once

#include "reco/RadioProfile.h"

namespace RadioProfileExchange {

inline constexpr qsizetype MaximumBytes = 64 * 1024;
inline constexpr qsizetype MaximumNameLength = 200;

QString originName(RadioProfile::Origin origin);
QString contentsDescription();
bool encode(const RadioProfile &profile, bool allowLearned, QByteArray *bytes, QString *error);
bool parse(const QByteArray &bytes, RadioProfile *profile, QString *error);
bool read(const QString &path, RadioProfile *profile, QString *error);
bool write(const QString &path, const RadioProfile &profile, bool allowLearned, bool replace, QString *error);

} // namespace RadioProfileExchange
