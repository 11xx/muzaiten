#include "app/MuzaitenApplication.h"

#include "Version.h"
#include "core/Track.h"
#include "ui/MainWindow.h"

#include <QCommandLineParser>
#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QLoggingCategory>
#include <QProxyStyle>
#include <QScrollBar>
#include <QStyle>

#include <taglib/tdebuglistener.h>

namespace {

class OverlayScrollBarStyle final : public QProxyStyle {
public:
    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override
    {
        if (metric == PM_ScrollBarExtent) {
            return 0;
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
};

class TagLibQtDebugListener final : public TagLib::DebugListener {
public:
    explicit TagLibQtDebugListener(bool verbose)
        : m_verbose(verbose)
    {
    }

    void printMessage(const TagLib::String &msg) override
    {
        if (m_verbose) {
            qWarning().noquote() << "TagLib:" << QString::fromStdString(msg.to8Bit(true)).trimmed();
        }
    }

private:
    bool m_verbose = false;
};

} // namespace

MuzaitenApplication::MuzaitenApplication(int &argc, char **argv)
    : QApplication(argc, argv)
{
    QCoreApplication::setApplicationName(QStringLiteral(MUZAITEN_APP_NAME));
    QCoreApplication::setApplicationVersion(QStringLiteral(MUZAITEN_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("11xx"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("11xx.org"));
    setDesktopFileName(QStringLiteral(MUZAITEN_APP_ID));

    qRegisterMetaType<Track>("Track");
    qRegisterMetaType<QVector<Track>>("QVector<Track>");
    installEventFilter(this);
    configureUiStyle();
    configureCommandLine();
}

int MuzaitenApplication::run()
{
    MainWindow window;
    window.show();
    return exec();
}

bool MuzaitenApplication::event(QEvent *event)
{
    const bool result = QApplication::event(event);
    if (!m_applyingStyle && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange)) {
        configureUiStyle();
    }
    return result;
}

bool MuzaitenApplication::eventFilter(QObject *object, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Enter:
        setScrollAreaHover(object, true);
        break;
    case QEvent::Leave:
        setScrollAreaHover(object, false);
        break;
    default:
        break;
    }
    return QApplication::eventFilter(object, event);
}

void MuzaitenApplication::configureCommandLine()
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native Linux music player"));
    parser.addHelpOption();
    parser.addVersionOption();

    QStringList verboseNames;
    verboseNames << QStringLiteral("verbose");
    const QCommandLineOption verboseOption(verboseNames, QStringLiteral("Enable verbose debug logging."));
    const QCommandLineOption stateRootOption(QStringLiteral("state-root"), QStringLiteral("Store database, cache, and config under <path>."), QStringLiteral("path"));
    const QCommandLineOption xdgStateOption(QStringLiteral("xdg-state"), QStringLiteral("Use normal XDG state paths even when running from build/."));
    parser.addOption(verboseOption);
    parser.addOption(stateRootOption);
    parser.addOption(xdgStateOption);
    parser.process(*this);

    const bool verbose = parser.isSet(verboseOption) || qEnvironmentVariableIsSet("MUZAITEN_VERBOSE");
    configureLogging(verbose);
    setProperty("muzaiten.verbose", verbose);

    const QString envRoot = QString::fromLocal8Bit(qgetenv("MUZAITEN_STATE_ROOT")).trimmed();
    const QString cliRoot = parser.value(stateRootOption).trimmed();
    const QString stateRoot = !cliRoot.isEmpty() ? cliRoot : envRoot;
    if (!stateRoot.isEmpty()) {
        setProperty("muzaiten.stateRoot", QDir(stateRoot).absolutePath());
    }
    setProperty("muzaiten.devState", !parser.isSet(xdgStateOption));
}

void MuzaitenApplication::configureLogging(bool verbose)
{
    if (verbose) {
        QLoggingCategory::setFilterRules(QStringLiteral("muzaiten.*.debug=true\nmuzaiten.*.info=true"));
    } else {
        QLoggingCategory::setFilterRules(QStringLiteral("muzaiten.*.debug=false\nmuzaiten.*.info=false"));
    }

    static auto *tagLibListener = new TagLibQtDebugListener(verbose);
    TagLib::setDebugListener(tagLibListener);
}

void MuzaitenApplication::configureUiStyle()
{
    if (!m_overlayScrollbarStyleInstalled) {
        setStyle(new OverlayScrollBarStyle);
        m_overlayScrollbarStyleInstalled = true;
    }

    m_applyingStyle = true;
    setStyleSheet(QStringLiteral(R"(
        QScrollBar:vertical, QScrollBar:horizontal {
            background: transparent;
            border: none;
            margin: 0;
        }

        QScrollBar:vertical {
            width: 10px;
        }

        QScrollBar:horizontal {
            height: 10px;
        }

        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: transparent;
            border-radius: 4px;
            min-height: 28px;
            min-width: 28px;
        }

        QScrollBar::handle:vertical {
            margin-left: 3px;
            margin-right: 3px;
        }

        QScrollBar::handle:horizontal {
            margin-top: 3px;
            margin-bottom: 3px;
        }

        QScrollBar[muzaitenPaneHover="true"]::handle:vertical,
        QScrollBar[muzaitenPaneHover="true"]::handle:horizontal {
            background: palette(midlight);
        }

        QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover,
        QScrollBar::handle:vertical:pressed, QScrollBar::handle:horizontal:pressed {
            background: palette(highlight);
        }

        QScrollBar::handle:vertical:hover,
        QScrollBar::handle:vertical:pressed {
            margin-left: 0;
            margin-right: 0;
        }

        QScrollBar::handle:horizontal:hover,
        QScrollBar::handle:horizontal:pressed {
            margin-top: 0;
            margin-bottom: 0;
        }

        QScrollBar::add-line, QScrollBar::sub-line,
        QScrollBar::add-page, QScrollBar::sub-page {
            background: transparent;
            border: none;
            width: 0;
            height: 0;
        }
    )"));
    m_applyingStyle = false;
}

void MuzaitenApplication::setScrollAreaHover(QObject *object, bool hovered)
{
    auto *widget = qobject_cast<QWidget *>(object);
    if (widget == nullptr) {
        return;
    }

    auto *area = qobject_cast<QAbstractScrollArea *>(widget);
    if (area == nullptr) {
        area = qobject_cast<QAbstractScrollArea *>(widget->parentWidget());
    }
    if (area == nullptr) {
        return;
    }

    auto apply = [hovered](QScrollBar *scrollBar) {
        if (scrollBar == nullptr || scrollBar->property("muzaitenPaneHover").toBool() == hovered) {
            return;
        }
        scrollBar->setProperty("muzaitenPaneHover", hovered);
        scrollBar->style()->unpolish(scrollBar);
        scrollBar->style()->polish(scrollBar);
        scrollBar->update();
    };
    apply(area->verticalScrollBar());
    apply(area->horizontalScrollBar());
}
