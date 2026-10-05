#include "playback/OutputEndpointMonitor.h"

#include <QMetaObject>

#include <algorithm>

#include <gst/gst.h>

namespace {

// The stable name a sink element accepts as its target: PulseAudio devices
// carry it as "internal-name", PipeWire devices as their node.name.
QString endpointName(GstDevice *device)
{
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(device), "internal-name") != nullptr) {
        gchar *name = nullptr;
        g_object_get(G_OBJECT(device), "internal-name", &name, nullptr);
        const QString result = QString::fromUtf8(name);
        g_free(name);
        if (!result.isEmpty()) {
            return result;
        }
    }
    QString result;
    if (GstStructure *props = gst_device_get_properties(device)) {
        result = QString::fromUtf8(gst_structure_get_string(props, "node.name"));
        gst_structure_free(props);
    }
    return result;
}

QString endpointLabel(GstDevice *device)
{
    gchar *display = gst_device_get_display_name(device);
    const QString label = QString::fromUtf8(display);
    g_free(display);
    return label;
}

// What a provider's bus handler needs: the monitor and the run it serves.
struct ProviderRun {
    OutputEndpointMonitor *monitor = nullptr;
    quint64 run = 0;
};

GstDeviceProvider *makeProvider(const QString &sinkFactory)
{
    const QString provider = OutputEndpointMonitor::providerFor(sinkFactory);
    if (provider.isEmpty()) {
        return nullptr;
    }
    return gst_device_provider_factory_get_by_name(provider.toLatin1().constData());
}

} // namespace

OutputEndpointMonitor::OutputEndpointMonitor(QObject *parent)
    : QObject(parent)
{
}

OutputEndpointMonitor::~OutputEndpointMonitor()
{
    stop();
}

QString OutputEndpointMonitor::providerFor(const QString &sinkFactory)
{
    if (sinkFactory == QStringLiteral("pulsesink")) {
        return QStringLiteral("pulsedeviceprovider");
    }
    if (sinkFactory == QStringLiteral("pipewiresink")) {
        return QStringLiteral("pipewiredeviceprovider");
    }
    return {};
}

QVector<OutputEndpoint> OutputEndpointMonitor::probe(const QString &sinkFactory)
{
    QVector<OutputEndpoint> result;
    GstDeviceProvider *provider = makeProvider(sinkFactory);
    if (provider == nullptr) {
        return result;
    }
    GList *devices = gst_device_provider_get_devices(provider);
    for (GList *item = devices; item != nullptr; item = item->next) {
        auto *device = GST_DEVICE(item->data);
        if (gst_device_has_classes(device, "Audio/Sink")) {
            const QString name = endpointName(device);
            if (!name.isEmpty()) {
                result.append({name, endpointLabel(device)});
            }
        }
    }
    g_list_free_full(devices, gst_object_unref);
    gst_object_unref(provider);
    return result;
}

bool OutputEndpointMonitor::start(const QString &sinkFactory)
{
    stop();
    m_provider = makeProvider(sinkFactory);
    if (m_provider == nullptr) {
        return false;
    }

    // Device messages are posted from the provider's own thread; copy what is
    // needed there and apply it on this object's thread.
    GstBus *bus = gst_device_provider_get_bus(m_provider);
    const auto handler = +[](GstBus *, GstMessage *message, gpointer userData) -> GstBusSyncReply {
        const bool added = GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_ADDED;
        const bool removed = GST_MESSAGE_TYPE(message) == GST_MESSAGE_DEVICE_REMOVED;
        if (added || removed) {
            GstDevice *device = nullptr;
            if (added) {
                gst_message_parse_device_added(message, &device);
            } else {
                gst_message_parse_device_removed(message, &device);
            }
            if (device != nullptr && gst_device_has_classes(device, "Audio/Sink")) {
                const QString name = endpointName(device);
                const QString label = endpointLabel(device);
                const auto *source = static_cast<const ProviderRun *>(userData);
                OutputEndpointMonitor *monitor = source->monitor;
                // Dropped if the monitor is gone or this run ended meanwhile.
                QMetaObject::invokeMethod(monitor, [monitor, run = source->run, added, name, label]() {
                    if (run != monitor->m_run) {
                        return;
                    }
                    if (added) {
                        monitor->add(name, label);
                    } else {
                        monitor->remove(name);
                    }
                }, Qt::QueuedConnection);
            }
            if (device != nullptr) {
                gst_object_unref(device);
            }
        }
        return GST_BUS_DROP;
    };
    gst_bus_set_sync_handler(bus, handler, new ProviderRun{this, ++m_run},
                             [](gpointer data) { delete static_cast<ProviderRun *>(data); });
    gst_object_unref(bus);

    if (!gst_device_provider_start(m_provider)) {
        stop();
        return false;
    }
    GList *devices = gst_device_provider_get_devices(m_provider);
    for (GList *item = devices; item != nullptr; item = item->next) {
        auto *device = GST_DEVICE(item->data);
        if (gst_device_has_classes(device, "Audio/Sink")) {
            const QString name = endpointName(device);
            if (!name.isEmpty()) {
                m_endpoints.insert(name, endpointLabel(device));
            }
        }
    }
    g_list_free_full(devices, gst_object_unref);
    return true;
}

void OutputEndpointMonitor::stop()
{
    ++m_run;
    if (m_provider == nullptr) {
        return;
    }
    gst_device_provider_stop(m_provider);
    GstBus *bus = gst_device_provider_get_bus(m_provider);
    gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
    gst_object_unref(bus);
    gst_object_unref(m_provider);
    m_provider = nullptr;
    m_endpoints.clear();
}

QVector<OutputEndpoint> OutputEndpointMonitor::endpoints() const
{
    QVector<OutputEndpoint> result;
    result.reserve(m_endpoints.size());
    for (auto it = m_endpoints.cbegin(); it != m_endpoints.cend(); ++it) {
        result.append({it.key(), it.value()});
    }
    std::sort(result.begin(), result.end(), [](const OutputEndpoint &a, const OutputEndpoint &b) {
        return a.label.localeAwareCompare(b.label) < 0;
    });
    return result;
}

void OutputEndpointMonitor::add(const QString &name, const QString &label)
{
    if (name.isEmpty()) {
        return;
    }
    const bool known = m_endpoints.contains(name);
    m_endpoints.insert(name, label);
    if (!known) {
        emit endpointAdded(name);
    }
}

void OutputEndpointMonitor::remove(const QString &name)
{
    if (m_endpoints.remove(name) > 0) {
        emit endpointRemoved(name);
    }
}
