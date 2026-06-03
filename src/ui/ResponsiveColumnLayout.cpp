#include "ui/ResponsiveColumnLayout.h"

#include <QEvent>
#include <QHeaderView>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QTableView>
#include <QWidget>

#include <algorithm>

ResponsiveColumnLayout::ResponsiveColumnLayout(QTableView *view,
                                               QVector<ResponsiveColumnSpec> specs,
                                               QObject *parent)
    : QObject(parent == nullptr ? view : parent)
    , m_view(view)
    , m_specs(std::move(specs))
{
    resetToDefaults();
    if (m_view != nullptr && m_view->viewport() != nullptr) {
        m_view->viewport()->installEventFilter(this);
    }
}

void ResponsiveColumnLayout::setUserVisibleColumns(const QSet<QString> &keys)
{
    m_userVisibleKeys = keys;
    relayout();
}

QSet<QString> ResponsiveColumnLayout::userVisibleColumns() const
{
    return m_userVisibleKeys;
}

void ResponsiveColumnLayout::setColumnPriority(const QString &key, ResponsiveColumnPriority priority)
{
    if (specForKey(key) == nullptr || m_priorities.value(key, ResponsiveColumnPriority::Normal) == priority) {
        return;
    }
    m_priorities.insert(key, priority);
    relayout();
    emit layoutSettingsChanged();
}

ResponsiveColumnPriority ResponsiveColumnLayout::columnPriority(const QString &key) const
{
    if (const ResponsiveColumnSpec *spec = specForKey(key)) {
        return m_priorities.value(key, spec->defaultPriority);
    }
    return ResponsiveColumnPriority::Normal;
}

bool ResponsiveColumnLayout::isResponsiveAbsorber(const QString &key) const
{
    if (const ResponsiveColumnSpec *spec = specForKey(key)) {
        return spec->responsiveAbsorber;
    }
    return false;
}

void ResponsiveColumnLayout::applySavedWidthsJson(const QJsonObject &root)
{
    const QJsonObject widths = root.value(QStringLiteral("columnWidths")).toObject();
    if (widths.isEmpty()) {
        for (const ResponsiveColumnSpec &spec : m_specs) {
            const int current = m_view == nullptr ? 0 : m_view->columnWidth(spec.logicalIndex);
            m_baselineWidths.insert(spec.key, std::max(minForSpec(spec), current > 0 ? current : spec.preferredWidth));
        }
        return;
    }

    for (const ResponsiveColumnSpec &spec : m_specs) {
        const int saved = widths.value(spec.key).toInt(m_baselineWidths.value(spec.key, spec.preferredWidth));
        m_baselineWidths.insert(spec.key, std::max(minForSpec(spec), saved));
    }
}

void ResponsiveColumnLayout::writeSavedWidthsJson(QJsonObject *root) const
{
    if (root == nullptr) {
        return;
    }
    QJsonObject widths;
    for (const ResponsiveColumnSpec &spec : m_specs) {
        widths.insert(spec.key, baselineForSpec(spec));
    }
    root->insert(QStringLiteral("columnWidths"), widths);
}

void ResponsiveColumnLayout::applyPrioritiesJson(const QJsonObject &root)
{
    const QJsonObject priorities = root.value(QStringLiteral("responsivePriorities")).toObject();
    for (const ResponsiveColumnSpec &spec : m_specs) {
        m_priorities.insert(spec.key, priorityFromString(priorities.value(spec.key).toString(), spec.defaultPriority));
    }
}

void ResponsiveColumnLayout::writePrioritiesJson(QJsonObject *root) const
{
    if (root == nullptr) {
        return;
    }
    QJsonObject priorities;
    for (const ResponsiveColumnSpec &spec : m_specs) {
        priorities.insert(spec.key, priorityToString(columnPriority(spec.key)));
    }
    root->insert(QStringLiteral("responsivePriorities"), priorities);
}

void ResponsiveColumnLayout::updateBaselineWidthsForResize(int leftLogical, int rightLogical)
{
    if (m_view == nullptr) {
        return;
    }

    bool changed = false;
    for (const int logical : {leftLogical, rightLogical}) {
        if (const ResponsiveColumnSpec *spec = specForLogicalIndex(logical)) {
            const int width = std::max(minForSpec(*spec), m_view->columnWidth(logical));
            if (m_baselineWidths.value(spec->key, spec->preferredWidth) != width) {
                m_baselineWidths.insert(spec->key, width);
                changed = true;
            }
        }
    }
    if (changed) {
        emit layoutSettingsChanged();
    }
}

int ResponsiveColumnLayout::baselineWidth(const QString &key) const
{
    if (const ResponsiveColumnSpec *spec = specForKey(key)) {
        return baselineForSpec(*spec);
    }
    return 0;
}

void ResponsiveColumnLayout::resetToDefaults()
{
    m_userVisibleKeys.clear();
    m_baselineWidths.clear();
    m_priorities.clear();
    for (const ResponsiveColumnSpec &spec : m_specs) {
        m_userVisibleKeys.insert(spec.key);
        m_baselineWidths.insert(spec.key, std::max(minForSpec(spec), spec.preferredWidth));
        m_priorities.insert(spec.key, spec.defaultPriority);
    }
    relayout();
}

void ResponsiveColumnLayout::relayout()
{
    if (m_view == nullptr || m_view->horizontalHeader() == nullptr || m_applyingLayout) {
        return;
    }

    QVector<int> visible = userVisibleSpecIndexes();
    int availableWidth = m_view->viewport() == nullptr ? m_view->width() : m_view->viewport()->width();
    if (!m_view->isVisible() || availableWidth <= 0) {
        availableWidth = 0;
        for (int specIndex : visible) {
            availableWidth += baselineForSpec(m_specs.at(specIndex));
        }
    }
    QVector<int> droppable;
    for (const ResponsiveColumnPriority priority : {ResponsiveColumnPriority::HideEarly, ResponsiveColumnPriority::Normal}) {
        for (int specIndex : visible) {
            const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
            if (columnPriority(spec.key) == priority) {
                droppable.push_back(specIndex);
            }
        }
    }

    while (!visible.isEmpty() && !fitsWithoutResponsiveHiding(visible, availableWidth)) {
        auto it = std::find_if(droppable.begin(), droppable.end(), [&visible](int specIndex) {
            return visible.contains(specIndex);
        });
        if (it == droppable.end()) {
            break;
        }
        visible.removeOne(*it);
    }

    const QVector<LayoutColumn> widths = computeWidths(visible, availableWidth);
    QHash<int, int> widthBySpecIndex;
    for (const LayoutColumn &column : widths) {
        widthBySpecIndex.insert(column.specIndex, column.width);
    }

    m_applyingLayout = true;
    const QSignalBlocker headerBlocker(m_view->horizontalHeader());
    int visibleMinTotal = 0;
    for (int specIndex : visible) {
        visibleMinTotal += minForSpec(m_specs.at(specIndex));
    }
    m_view->setHorizontalScrollBarPolicy(visibleMinTotal > availableWidth ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);

    for (int specIndex = 0; specIndex < m_specs.size(); ++specIndex) {
        const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
        const bool shouldShow = visible.contains(specIndex);
        m_view->setColumnHidden(spec.logicalIndex, !shouldShow);
        if (shouldShow) {
            m_view->setColumnWidth(spec.logicalIndex, widthBySpecIndex.value(specIndex, minForSpec(spec)));
        }
    }
    m_applyingLayout = false;
}

bool ResponsiveColumnLayout::eventFilter(QObject *watched, QEvent *event)
{
    if (m_view != nullptr && watched == m_view->viewport()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        relayout();
    }
    return QObject::eventFilter(watched, event);
}

const ResponsiveColumnSpec *ResponsiveColumnLayout::specForKey(const QString &key) const
{
    for (const ResponsiveColumnSpec &spec : m_specs) {
        if (spec.key == key) {
            return &spec;
        }
    }
    return nullptr;
}

const ResponsiveColumnSpec *ResponsiveColumnLayout::specForLogicalIndex(int logicalIndex) const
{
    for (const ResponsiveColumnSpec &spec : m_specs) {
        if (spec.logicalIndex == logicalIndex) {
            return &spec;
        }
    }
    return nullptr;
}

int ResponsiveColumnLayout::baselineForSpec(const ResponsiveColumnSpec &spec) const
{
    return std::max(minForSpec(spec), m_baselineWidths.value(spec.key, spec.preferredWidth));
}

int ResponsiveColumnLayout::minForSpec(const ResponsiveColumnSpec &spec) const
{
    return std::max(1, spec.minWidth);
}

QVector<int> ResponsiveColumnLayout::userVisibleSpecIndexes() const
{
    QVector<int> result;
    result.reserve(m_specs.size());
    for (int i = 0; i < m_specs.size(); ++i) {
        if (m_userVisibleKeys.contains(m_specs.at(i).key)) {
            result.push_back(i);
        }
    }
    return result;
}

QVector<ResponsiveColumnLayout::LayoutColumn> ResponsiveColumnLayout::computeWidths(const QVector<int> &specIndexes,
                                                                                   int availableWidth) const
{
    QVector<LayoutColumn> result;
    result.reserve(specIndexes.size());
    if (specIndexes.isEmpty()) {
        return result;
    }

    int baselineTotal = 0;
    int minTotal = 0;
    int absorberSpecIndex = -1;
    for (int specIndex : specIndexes) {
        const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
        baselineTotal += baselineForSpec(spec);
        minTotal += minForSpec(spec);
        if (spec.responsiveAbsorber) {
            absorberSpecIndex = specIndex;
        }
    }

    const int target = std::max(availableWidth, minTotal);
    if (baselineTotal <= target) {
        int remainingExtra = target - baselineTotal;
        const bool absorberGetsSmallExtra = absorberSpecIndex >= 0 && remainingExtra <= 120;
        int assigned = 0;
        for (int i = 0; i < specIndexes.size(); ++i) {
            const int specIndex = specIndexes.at(i);
            const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
            int width = baselineForSpec(spec);
            if (absorberGetsSmallExtra) {
                if (specIndex == absorberSpecIndex) {
                    width += remainingExtra;
                }
            } else if (remainingExtra > 0) {
                const int extra = i == specIndexes.size() - 1
                    ? remainingExtra - assigned
                    : (baselineForSpec(spec) * remainingExtra) / std::max(1, baselineTotal);
                width += extra;
                assigned += extra;
            }
            result.push_back({specIndex, std::max(minForSpec(spec), width)});
        }
        return result;
    }

    QHash<int, int> widths;
    int currentTotal = baselineTotal;
    for (int specIndex : specIndexes) {
        const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
        widths.insert(specIndex, baselineForSpec(spec));
    }
    if (absorberSpecIndex >= 0) {
        const ResponsiveColumnSpec &absorber = m_specs.at(absorberSpecIndex);
        const int shrink = std::min(currentTotal - target,
                                    baselineForSpec(absorber) - minForSpec(absorber));
        if (shrink > 0) {
            widths.insert(absorberSpecIndex, baselineForSpec(absorber) - shrink);
            currentTotal -= shrink;
        }
    }
    if (currentTotal > target) {
        for (int specIndex : specIndexes) {
            const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
            widths.insert(specIndex, minForSpec(spec));
        }
    }

    for (int specIndex : specIndexes) {
        result.push_back({specIndex, widths.value(specIndex, minForSpec(m_specs.at(specIndex)))});
    }
    return result;
}

bool ResponsiveColumnLayout::fitsWithoutResponsiveHiding(const QVector<int> &specIndexes, int availableWidth) const
{
    if (specIndexes.isEmpty()) {
        return true;
    }
    int baselineTotal = 0;
    int shrinkableAbsorber = 0;
    int minTotal = 0;
    for (int specIndex : specIndexes) {
        const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
        baselineTotal += baselineForSpec(spec);
        minTotal += minForSpec(spec);
        if (spec.responsiveAbsorber) {
            shrinkableAbsorber += baselineForSpec(spec) - minForSpec(spec);
        }
    }
    if (baselineTotal - shrinkableAbsorber <= availableWidth) {
        return true;
    }
    for (int specIndex : specIndexes) {
        if (columnPriority(m_specs.at(specIndex).key) != ResponsiveColumnPriority::Keep) {
            return false;
        }
    }
    return minTotal <= availableWidth;
}

QString ResponsiveColumnLayout::priorityToString(ResponsiveColumnPriority priority)
{
    switch (priority) {
    case ResponsiveColumnPriority::Keep:
        return QStringLiteral("keep");
    case ResponsiveColumnPriority::HideEarly:
        return QStringLiteral("hideEarly");
    case ResponsiveColumnPriority::Normal:
        return QStringLiteral("normal");
    }
    return QStringLiteral("normal");
}

ResponsiveColumnPriority ResponsiveColumnLayout::priorityFromString(const QString &value,
                                                                   ResponsiveColumnPriority fallback)
{
    if (value == QStringLiteral("keep")) {
        return ResponsiveColumnPriority::Keep;
    }
    if (value == QStringLiteral("hideEarly")) {
        return ResponsiveColumnPriority::HideEarly;
    }
    if (value == QStringLiteral("normal")) {
        return ResponsiveColumnPriority::Normal;
    }
    return fallback;
}
