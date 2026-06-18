#include "ui/ResponsiveColumnLayout.h"

#include <QEvent>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableView>
#include <QTimer>
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
    if (m_view != nullptr) {
        m_view->installEventFilter(this);
        if (m_view->viewport() != nullptr) {
            m_view->viewport()->installEventFilter(this);
        }
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

void ResponsiveColumnLayout::setColumnMinimumWidth(const QString &key, int width)
{
    if (const ResponsiveColumnSpec *spec = specForKey(key)) {
        const int clamped = std::clamp(width, std::max(1, spec->minWidth), 2000);
        if (minForSpec(*spec) == clamped) {
            return;
        }
        m_minimumWidths.insert(key, clamped);
        if (m_baselineWidths.value(key, spec->preferredWidth) < clamped) {
            m_baselineWidths.insert(key, clamped);
        }
        relayout();
        emit layoutSettingsChanged();
    }
}

int ResponsiveColumnLayout::columnMinimumWidth(const QString &key) const
{
    if (const ResponsiveColumnSpec *spec = specForKey(key)) {
        return minForSpec(*spec);
    }
    return 0;
}

int ResponsiveColumnLayout::defaultColumnMinimumWidth(const QString &key) const
{
    if (const ResponsiveColumnSpec *spec = specForKey(key)) {
        return std::max(1, spec->minWidth);
    }
    return 0;
}

void ResponsiveColumnLayout::setDropOrderKeys(const QStringList &keys)
{
    QStringList ordered;
    for (const QString &key : keys) {
        if (specForKey(key) != nullptr && !ordered.contains(key)) {
            ordered.push_back(key);
        }
    }
    for (const ResponsiveColumnSpec &spec : m_specs) {
        if (!ordered.contains(spec.key)) {
            ordered.push_back(spec.key);
        }
    }
    if (m_dropOrderKeys == ordered) {
        return;
    }
    m_dropOrderKeys = ordered;
    relayout();
    emit layoutSettingsChanged();
}

QStringList ResponsiveColumnLayout::dropOrderKeys() const
{
    return m_dropOrderKeys;
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

void ResponsiveColumnLayout::applyMinimumWidthsJson(const QJsonObject &root)
{
    const QJsonObject widths = root.value(QStringLiteral("responsiveMinWidths")).toObject();
    for (const ResponsiveColumnSpec &spec : m_specs) {
        const int saved = widths.value(spec.key).toInt(spec.minWidth);
        m_minimumWidths.insert(spec.key, std::clamp(saved, std::max(1, spec.minWidth), 2000));
        if (m_baselineWidths.value(spec.key, spec.preferredWidth) < minForSpec(spec)) {
            m_baselineWidths.insert(spec.key, minForSpec(spec));
        }
    }
}

void ResponsiveColumnLayout::writeMinimumWidthsJson(QJsonObject *root) const
{
    if (root == nullptr) {
        return;
    }
    QJsonObject widths;
    for (const ResponsiveColumnSpec &spec : m_specs) {
        widths.insert(spec.key, minForSpec(spec));
    }
    root->insert(QStringLiteral("responsiveMinWidths"), widths);
}

void ResponsiveColumnLayout::applyDropOrderJson(const QJsonObject &root)
{
    const QJsonArray order = root.value(QStringLiteral("responsiveDropOrder")).toArray();
    QStringList ordered;
    for (const QJsonValue &value : order) {
        const QString key = value.toString();
        if (specForKey(key) != nullptr && !ordered.contains(key)) {
            ordered.push_back(key);
        }
    }
    if (ordered.isEmpty()) {
        return;
    }
    for (const ResponsiveColumnSpec &spec : m_specs) {
        if (!ordered.contains(spec.key)) {
            ordered.push_back(spec.key);
        }
    }
    m_dropOrderKeys = ordered;
}

void ResponsiveColumnLayout::writeDropOrderJson(QJsonObject *root) const
{
    if (root == nullptr) {
        return;
    }
    QJsonArray order;
    for (const QString &key : m_dropOrderKeys) {
        order.append(key);
    }
    root->insert(QStringLiteral("responsiveDropOrder"), order);
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
    m_minimumWidths.clear();
    m_priorities.clear();
    m_dropOrderKeys.clear();
    for (const ResponsiveColumnSpec &spec : m_specs) {
        m_userVisibleKeys.insert(spec.key);
        m_minimumWidths.insert(spec.key, std::max(1, spec.minWidth));
        m_baselineWidths.insert(spec.key, std::max(minForSpec(spec), spec.preferredWidth));
        m_priorities.insert(spec.key, spec.defaultPriority);
        m_dropOrderKeys.push_back(spec.key);
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
        droppable.append(orderedSpecIndexesByDropOrder(priority, visible));
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
    const bool needsHorizontalScroll = visibleMinTotal > availableWidth;
    m_view->setHorizontalScrollBarPolicy(needsHorizontalScroll ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);

    for (int specIndex = 0; specIndex < m_specs.size(); ++specIndex) {
        const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
        const bool shouldShow = visible.contains(specIndex);
        m_view->setColumnHidden(spec.logicalIndex, !shouldShow);
        if (shouldShow) {
            m_view->setColumnWidth(spec.logicalIndex, widthBySpecIndex.value(specIndex, minForSpec(spec)));
        }
    }
    if (!needsHorizontalScroll && m_view->horizontalScrollBar() != nullptr) {
        m_view->horizontalScrollBar()->setValue(0);
    }
    m_applyingLayout = false;
}

void ResponsiveColumnLayout::scheduleDeferredRelayout()
{
    if (m_deferredRelayoutPending) {
        return;
    }
    m_deferredRelayoutPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_deferredRelayoutPending = false;
        relayout();
    });
}

bool ResponsiveColumnLayout::eventFilter(QObject *watched, QEvent *event)
{
    const bool viewportGeometryEvent = m_view != nullptr && watched == m_view->viewport()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show);
    const bool viewGeometryEvent = m_view != nullptr && watched == m_view
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest);
    if (viewportGeometryEvent || viewGeometryEvent) {
        relayout();
        if (event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest) {
            // On the viewport's Show (first show, or re-show when its QStackedWidget
            // page becomes current again) the final width may not be applied yet, so
            // the relayout above falls back to the baseline-sum width — leaving the
            // columns bunched at the left with empty space on the right and only the
            // absorber's edge near the viewport border. Re-run once the event loop
            // has flushed the real geometry so the absorber fills the viewport.
            scheduleDeferredRelayout();
        }
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
    return std::max(std::max(1, spec.minWidth), m_minimumWidths.value(spec.key, spec.minWidth));
}

QVector<int> ResponsiveColumnLayout::orderedSpecIndexesByDropOrder(ResponsiveColumnPriority priority,
                                                                  const QVector<int> &visible) const
{
    QVector<int> result;
    for (const QString &key : m_dropOrderKeys) {
        for (int specIndex : visible) {
            const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
            if (spec.key == key && columnPriority(spec.key) == priority && !result.contains(specIndex)) {
                result.push_back(specIndex);
                break;
            }
        }
    }
    for (int specIndex : visible) {
        const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
        if (columnPriority(spec.key) == priority && !result.contains(specIndex)) {
            result.push_back(specIndex);
        }
    }
    return result;
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
        const int remainingExtra = target - baselineTotal;
        for (int i = 0; i < specIndexes.size(); ++i) {
            const int specIndex = specIndexes.at(i);
            const ResponsiveColumnSpec &spec = m_specs.at(specIndex);
            int width = baselineForSpec(spec);
            if (specIndex == absorberSpecIndex) {
                width += remainingExtra;
            } else if (absorberSpecIndex < 0 && remainingExtra > 0) {
                width += i == specIndexes.size() - 1 ? remainingExtra : 0;
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
