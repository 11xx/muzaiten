// Compile the CLI with rendezvous points only in this uninstalled fixture.
// SIGTERM/SIGINT still run the CLI's handler; the barrier only determines when
// the parent sends the signal, after an autocommit copy or native completion.
namespace {
void featuresTestCheckpoint(const char *point, int completed);
}

#define MUZAITEN_FEATURES_TEST_CHECKPOINTS
#define main featuresMain
#include "../src/indexer/FeaturesMain.cpp"
#undef main

namespace {

void featuresTestCheckpoint(const char *point, int completed)
{
    // Both checkpoints run on the sole live thread; a process stop signal
    // cannot be handled by another thread while this one waits.
    const QByteArray checkpoint = QByteArray(point) + ':' + QByteArray::number(completed);
    if (qgetenv("MUZAITEN_TEST_CHECKPOINT") != checkpoint) {
        return;
    }

    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGTERM);
    sigaddset(&stopSignals, SIGINT);
    sigset_t previous;
    if (sigprocmask(SIG_BLOCK, &stopSignals, &previous) != 0) {
        fail(QStringLiteral("could not block checkpoint signals"));
    }
    std::fprintf(stderr, "checkpoint %s\n", checkpoint.constData());
    std::fflush(stderr);
    // Atomic unmask-and-wait prevents a signal between the stop check and
    // suspension from leaving the child blocked. The parent bounds its wait.
    while (!stopRequested()) {
        sigsuspend(&previous);
    }
    if (sigprocmask(SIG_SETMASK, &previous, nullptr) != 0) {
        fail(QStringLiteral("could not restore checkpoint signals"));
    }
}

} // namespace

int main(int argc, char **argv)
{
    return featuresMain(argc, argv);
}
