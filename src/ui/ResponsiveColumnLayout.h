#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

class QJsonObject;
class QTableView;

enum class ResponsiveColumnPriority {
    Keep,
    Normal,
    HideEarly,
};

struct ResponsiveColumnSpec {
    int logicalIndex;
    QString key;
    int preferredWidth;
    int minWidth;
    ResponsiveColumnPriority defaultPriority;
    bool responsiveAbsorber = false;
};

class ResponsiveColumnLayout final : public QObject {
    Q_OBJECT

public:
    explicit ResponsiveColumnLayout(QTableView *view,
                                    QVector<ResponsiveColumnSpec> specs,
                                    QObject *parent = nullptr);

    void setUserVisibleColumns(const QSet<QString> &keys);
    QSet<QString> userVisibleColumns() const;

    void setColumnPriority(const QString &key, ResponsiveColumnPriority priority);
    ResponsiveColumnPriority columnPriority(const QString &key) const;
    bool isResponsiveAbsorber(const QString &key) const;
    void setColumnMinimumWidth(const QString &key, int width);
    int columnMinimumWidth(const QString &key) const;
    int defaultColumnMinimumWidth(const QString &key) const;
    void setDropOrderKeys(const QStringList &keys);
    QStringList dropOrderKeys() const;

    void applySavedWidthsJson(const QJsonObject &root);
    void writeSavedWidthsJson(QJsonObject *root) const;

    void applyPrioritiesJson(const QJsonObject &root);
    void writePrioritiesJson(QJsonObject *root) const;
    void applyMinimumWidthsJson(const QJsonObject &root);
    void writeMinimumWidthsJson(QJsonObject *root) const;
    void applyDropOrderJson(const QJsonObject &root);
    void writeDropOrderJson(QJsonObject *root) const;

    void updateBaselineWidthsForResize(int leftLogical, int rightLogical);
    int baselineWidth(const QString &key) const;

    void resetToDefaults();
    void relayout();

signals:
    void layoutSettingsChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct LayoutColumn {
        int specIndex = -1;
        int width = 0;
    };

    const ResponsiveColumnSpec *specForKey(const QString &key) const;
    const ResponsiveColumnSpec *specForLogicalIndex(int logicalIndex) const;
    int baselineForSpec(const ResponsiveColumnSpec &spec) const;
    int minForSpec(const ResponsiveColumnSpec &spec) const;
    QVector<int> orderedSpecIndexesByDropOrder(ResponsiveColumnPriority priority,
                                               const QVector<int> &visible) const;
    QVector<int> userVisibleSpecIndexes() const;
    QVector<LayoutColumn> computeWidths(const QVector<int> &specIndexes, int availableWidth) const;
    bool fitsWithoutResponsiveHiding(const QVector<int> &specIndexes, int availableWidth) const;
    static QString priorityToString(ResponsiveColumnPriority priority);
    static ResponsiveColumnPriority priorityFromString(const QString &value,
                                                       ResponsiveColumnPriority fallback);

    QTableView *m_view = nullptr;
    QVector<ResponsiveColumnSpec> m_specs;
    QSet<QString> m_userVisibleKeys;
    QHash<QString, int> m_baselineWidths;
    QHash<QString, int> m_minimumWidths;
    QHash<QString, ResponsiveColumnPriority> m_priorities;
    QStringList m_dropOrderKeys;
    bool m_applyingLayout = false;
};
