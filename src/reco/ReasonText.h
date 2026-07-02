#pragma once

#include "reco/TrackScorer.h"

#include <QList>
#include <QString>

namespace ReasonText {

QString sentence(const QList<TrackScorer::Component> &components);
QString breakdown(const QList<TrackScorer::Component> &components);

} // namespace ReasonText
