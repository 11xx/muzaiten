#include "ui/ScrobbleUploadDispatcher.h"

namespace ScrobbleUploadDispatcher {

void dispatch(const ScrobbleDestinationSet &destinations, const QString &destinationId,
              const std::function<void()> &uploadLastFm,
              const std::function<void(const QString &)> &uploadCompatible)
{
    const ScrobbleDestination *destination = destinations.find(destinationId);
    if (destination == nullptr) {
        return;
    }
    if (destination->type == ScrobbleDestination::Type::LastFm) {
        uploadLastFm();
    } else if (destination->type == ScrobbleDestination::Type::ListenBrainzCompatible) {
        uploadCompatible(destinationId);
    }
}

}   // namespace ScrobbleUploadDispatcher
