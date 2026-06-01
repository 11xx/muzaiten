#include <QJsonArray>
#include <QTest>

#include "ui/MainPanelKeybindings.h"

class TestMainPanelKeybindings : public QObject {
    Q_OBJECT

private slots:
    void defaultProfileContainsRequiredBindings()
    {
        const KeyBindingMap bindings = mainPanelBindingMapForProfile(QStringLiteral("dired_hjkl"));

        QCOMPARE(bindings.value(QKeySequence(Qt::Key_H)), QString::fromLatin1(MainPanelAction::FocusPrevious));
        QCOMPARE(bindings.value(QKeySequence(Qt::Key_L)), QString::fromLatin1(MainPanelAction::FocusNext));
        QCOMPARE(bindings.value(QKeySequence(Qt::Key_Q)), QString::fromLatin1(MainPanelAction::FocusQueue));
        QCOMPARE(bindings.value(QKeySequence(Qt::Key_Slash)), QString::fromLatin1(MainPanelAction::Search));
        QCOMPARE(bindings.value(QKeySequence(Qt::ControlModifier | Qt::Key_V)), QString::fromLatin1(MainPanelAction::PageDown));
        QCOMPARE(bindings.value(QKeySequence(Qt::AltModifier | Qt::Key_V)), QString::fromLatin1(MainPanelAction::PageUp));
        QCOMPARE(bindings.value(QKeySequence(Qt::AltModifier | Qt::Key_N)), QString::fromLatin1(MainPanelAction::SearchNext));
        QCOMPARE(bindings.value(QKeySequence(Qt::AltModifier | Qt::Key_P)), QString::fromLatin1(MainPanelAction::SearchPrevious));
        QCOMPARE(bindings.value(QKeySequence(Qt::Key_Return)), QString::fromLatin1(MainPanelAction::PlayNow));
        QCOMPARE(bindings.value(QKeySequence(Qt::AltModifier | Qt::Key_Return)), QString::fromLatin1(MainPanelAction::PlayNow));
        QCOMPARE(bindings.value(QKeySequence(Qt::Key_Space)), QString::fromLatin1(MainPanelAction::AddToQueue));
        QCOMPARE(bindings.value(QKeySequence(Qt::AltModifier | Qt::Key_Space)), QString::fromLatin1(MainPanelAction::PlayNext));
        QCOMPARE(bindings.value(QKeySequence(Qt::Key_Escape)), QString::fromLatin1(MainPanelAction::Escape));
        QCOMPARE(bindings.value(QKeySequence(Qt::ControlModifier | Qt::Key_G)), QString::fromLatin1(MainPanelAction::Escape));
    }

    void invalidProfileFallsBackToDefault()
    {
        QCOMPARE(mainPanelBindingMapForProfile(QStringLiteral("missing")),
                 mainPanelBindingMapForProfile(defaultMainPanelKeyBindingProfileName()));
    }

    void focusOrderRoundTrips()
    {
        const QVector<MainPanelId> order = {
            MainPanelId::Artists,
            MainPanelId::Albums,
            MainPanelId::Tracks,
            MainPanelId::Queue,
        };
        QCOMPARE(mainPanelFocusOrderFromJson(mainPanelFocusOrderToJson(order)), order);
    }

    void focusOrderRejectsInvalidValues()
    {
        QCOMPARE(mainPanelFocusOrderFromJson(QJsonArray{QStringLiteral("queue"),
                                                        QStringLiteral("artists"),
                                                        QStringLiteral("albums"),
                                                        QStringLiteral("tracks")}),
                 defaultMainPanelFocusOrder());
        QCOMPARE(mainPanelFocusOrderFromJson(QJsonArray{QStringLiteral("queue"),
                                                        QStringLiteral("queue"),
                                                        QStringLiteral("albums"),
                                                        QStringLiteral("tracks")}),
                 defaultMainPanelFocusOrder());
        QCOMPARE(mainPanelFocusOrderFromJson(QJsonArray{QStringLiteral("queue"),
                                                        QStringLiteral("artists"),
                                                        QStringLiteral("bogus"),
                                                        QStringLiteral("tracks")}),
                 defaultMainPanelFocusOrder());
    }
};

QTEST_MAIN(TestMainPanelKeybindings)
#include "test_main_panel_keybindings.moc"
