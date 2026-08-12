#pragma once

#include "scrobble/ScrobbleDestination.h"

#include <QString>

#include <functional>

namespace ScrobbleUploadDispatcher {

// Routes a user-requested history upload only to a configured destination. The
// compatible callback receives the selected id unchanged so its backlog stays
// scoped to that destination.
void dispatch(const ScrobbleDestinationSet &destinations, const QString &destinationId,
              const std::function<void()> &uploadLastFm,
              const std::function<void(const QString &destinationId)> &uploadCompatible);

}   // namespace ScrobbleUploadDispatcher
