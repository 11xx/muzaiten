#include "app/MuzaitenApplication.h"

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == "--check-storage") qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    MuzaitenApplication app(argc, argv);
    return app.run();
}
