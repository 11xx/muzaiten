#pragma once

#include <QApplication>

class MuzaitenApplication final : public QApplication {
    Q_OBJECT

public:
    MuzaitenApplication(int &argc, char **argv);

    int run();

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void configureCommandLine();
    void configureLogging(bool verbose);
    void configureUiStyle();
    void setScrollAreaHover(QObject *object, bool hovered);

    bool m_applyingStyle = false;
    bool m_overlayScrollbarStyleInstalled = false;
};
