#pragma once

#include <QApplication>

class MuzaitenApplication final : public QApplication {
    Q_OBJECT

public:
    MuzaitenApplication(int &argc, char **argv);

    int run();

private:
    void configureCommandLine();
    void configureLogging(bool verbose);
    void configureUiStyle();
};
