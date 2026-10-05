#include "playback/PipeWireStreamWatch.h"

#include <QMetaObject>

#ifdef MUZAITEN_HAVE_PIPEWIRE

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/utils/string.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

void initializePipeWire()
{
    static std::once_flag once;
    std::call_once(once, []() { pw_init(nullptr, nullptr); });
}

uint32_t idProperty(const spa_dict *props, const char *key)
{
    const char *value = spa_dict_lookup(props, key);
    return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : SPA_ID_INVALID;
}

} // namespace

struct PipeWireStreamWatch::Impl {
    PipeWireStreamWatch *owner = nullptr;
    std::string streamName;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook registryListener{};

    // Graph state, touched only on the loop thread: sinks as {name, object
    // serial}, the node(s) carrying the stream's name, and links as {output
    // node, input node}.
    struct Sink {
        std::string name;
        std::string serial;
    };
    std::unordered_map<uint32_t, Sink> sinks;
    uint32_t defaultMetadata = SPA_ID_INVALID;
    pw_metadata *metadata = nullptr;
    std::unordered_set<uint32_t> streamNodes;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> links;

    mutable std::mutex mutex;
    QString linked;

    void publish()
    {
        QString route;
        for (const auto &[link, ends] : links) {
            if (streamNodes.contains(ends.first)) {
                if (const auto sink = sinks.find(ends.second); sink != sinks.end()) {
                    route = QString::fromStdString(sink->second.name);
                    break;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (route == linked) {
                return;
            }
            linked = route;
        }
        // Delivered on the owner's thread; dropped if the owner is gone.
        PipeWireStreamWatch *target = owner;
        QMetaObject::invokeMethod(target, [target, route]() {
            emit target->linkedSinkChanged(route);
        }, Qt::QueuedConnection);
    }

    static void onGlobal(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props)
    {
        auto *self = static_cast<Impl *>(data);
        if (props == nullptr) {
            return;
        }
        if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
            const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            if (name == nullptr) {
                return;
            }
            if (spa_streq(mediaClass, "Audio/Sink")) {
                const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
                self->sinks[id] = {name, serial != nullptr ? serial : ""};
            }
            if (self->streamName == name) {
                self->streamNodes.insert(id);
            }
        } else if (spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
            if (spa_streq(spa_dict_lookup(props, PW_KEY_METADATA_NAME), "default")) {
                self->defaultMetadata = id;
            }
            return;
        } else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
            const uint32_t output = idProperty(props, PW_KEY_LINK_OUTPUT_NODE);
            const uint32_t input = idProperty(props, PW_KEY_LINK_INPUT_NODE);
            if (output == SPA_ID_INVALID || input == SPA_ID_INVALID) {
                return;
            }
            self->links[id] = {output, input};
        } else {
            return;
        }
        self->publish();
    }

    static void onGlobalRemove(void *data, uint32_t id)
    {
        auto *self = static_cast<Impl *>(data);
        if (id == self->defaultMetadata) {
            self->defaultMetadata = SPA_ID_INVALID;
            if (self->metadata != nullptr) {
                pw_proxy_destroy(reinterpret_cast<pw_proxy *>(self->metadata));
                self->metadata = nullptr;
            }
        }
        const bool changed = self->sinks.erase(id) + self->streamNodes.erase(id) + self->links.erase(id) > 0;
        if (changed) {
            self->publish();
        }
    }
};

bool PipeWireStreamWatch::supported()
{
    return true;
}

bool PipeWireStreamWatch::start(const QString &nodeName)
{
    static const pw_registry_events registryEvents = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = Impl::onGlobal,
        .global_remove = Impl::onGlobalRemove,
    };
    stop();
    initializePipeWire();
    d = std::make_unique<Impl>();
    d->owner = this;
    d->streamName = nodeName.toStdString();
    d->loop = pw_thread_loop_new("muzaiten-route", nullptr);
    if (d->loop != nullptr) {
        d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    }
    if (d->context == nullptr || pw_thread_loop_start(d->loop) < 0) {
        stop();
        return false;
    }
    pw_thread_loop_lock(d->loop);
    d->core = pw_context_connect(d->context, nullptr, 0);
    if (d->core != nullptr) {
        d->registry = pw_core_get_registry(d->core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(d->registry, &d->registryListener, &registryEvents, d.get());
    }
    pw_thread_loop_unlock(d->loop);
    if (d->core == nullptr) {
        stop();
        return false;
    }
    return true;
}

void PipeWireStreamWatch::stop()
{
    if (!d) {
        return;
    }
    // Stop the loop thread first; nothing below runs concurrently with it.
    if (d->loop != nullptr) {
        pw_thread_loop_stop(d->loop);
    }
    if (d->metadata != nullptr) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(d->metadata));
    }
    if (d->registry != nullptr) {
        spa_hook_remove(&d->registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(d->registry));
    }
    if (d->core != nullptr) {
        pw_core_disconnect(d->core);
    }
    if (d->context != nullptr) {
        pw_context_destroy(d->context);
    }
    if (d->loop != nullptr) {
        pw_thread_loop_destroy(d->loop);
    }
    d.reset();
}

bool PipeWireStreamWatch::running() const
{
    return d != nullptr;
}

QString PipeWireStreamWatch::linkedSink() const
{
    if (!d) {
        return {};
    }
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->linked;
}

bool PipeWireStreamWatch::moveTo(const QString &sinkName)
{
    if (!d || d->loop == nullptr) {
        return false;
    }
    const std::string name = sinkName.toStdString();
    pw_thread_loop_lock(d->loop);
    uint32_t sinkId = SPA_ID_INVALID;
    std::string serial;
    for (const auto &[id, sink] : d->sinks) {
        if (sink.name == name) {
            sinkId = id;
            serial = sink.serial;
            break;
        }
    }
    const uint32_t node = d->streamNodes.empty() ? SPA_ID_INVALID : *d->streamNodes.begin();
    if (d->metadata == nullptr && d->defaultMetadata != SPA_ID_INVALID && d->registry != nullptr) {
        d->metadata = static_cast<pw_metadata *>(
            pw_registry_bind(d->registry, d->defaultMetadata, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    }
    const bool known = node != SPA_ID_INVALID && d->metadata != nullptr;
    if (known && sinkId != SPA_ID_INVALID) {
        // What a mixer writes: the session manager relinks the stream and
        // remembers the target for the application.
        const std::string id = std::to_string(sinkId);
        pw_metadata_set_property(d->metadata, node, "target.node", "Spa:Id", id.c_str());
        if (!serial.empty()) {
            pw_metadata_set_property(d->metadata, node, "target.object", "Spa:Id", serial.c_str());
        }
    } else if (known) {
        // A sink that has only just appeared may not be listed here yet; the
        // session manager also resolves a target given by node name.
        pw_metadata_set_property(d->metadata, node, "target.object", nullptr, name.c_str());
    }
    pw_thread_loop_unlock(d->loop);
    return known;
}

#else

struct PipeWireStreamWatch::Impl {};

bool PipeWireStreamWatch::supported()
{
    return false;
}

bool PipeWireStreamWatch::start(const QString &nodeName)
{
    Q_UNUSED(nodeName);
    return false;
}

void PipeWireStreamWatch::stop() {}

bool PipeWireStreamWatch::running() const
{
    return false;
}

QString PipeWireStreamWatch::linkedSink() const
{
    return {};
}

bool PipeWireStreamWatch::moveTo(const QString &sinkName)
{
    Q_UNUSED(sinkName);
    return false;
}

#endif

PipeWireStreamWatch::PipeWireStreamWatch(QObject *parent)
    : QObject(parent)
{
}

PipeWireStreamWatch::~PipeWireStreamWatch()
{
    stop();
}
