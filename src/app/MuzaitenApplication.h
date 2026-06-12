#pragma once

#include <QApplication>

class MuzaitenApplication final : public QApplication {
    Q_OBJECT

public:
    MuzaitenApplication(int &argc, char **argv);

    int run();

private:
    bool raiseRunningInstance();
    void configureCommandLine();
    void configureLogging(bool verbose);
};
