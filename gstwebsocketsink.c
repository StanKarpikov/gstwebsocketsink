/******************************************************************************
 * INCLUDES
 ******************************************************************************/

#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <libwebsockets.h>
#include <string.h>

/******************************************************************************
 * DEFINES
 ******************************************************************************/

#define AUTHOR      "Stanislav Karpikov <stankarpikov@gmail.com>"
#define DESCRIPTION "Sends data over WebSocket"

#ifndef VERSION
#define VERSION "0.0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "websocketsink"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "Websocket Sink"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/StanKarpikov/gstwebsocketsink"
#endif

GST_DEBUG_CATEGORY_STATIC(gst_websocket_sink_debug);
#define GST_CAT_DEFAULT gst_websocket_sink_debug

#define DEFAULT_PORT 8080
#define DEFAULT_HOST "0.0.0.0"

enum
{
    PROP_0,
    PROP_HOST,
    PROP_PORT,
    PROP_LAST
};

/******************************************************************************
 * PROTOTYPES
 ******************************************************************************/

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len);

/******************************************************************************
 * PRIVATE DATA AND TYPES
 ******************************************************************************/

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

typedef struct _GstWebSocketSink
{
    GstBaseSink parent;

    gchar *host;
    guint port;

    struct lws_context *context;
    struct lws_vhost *vhost;
    GList *clients;
    GMutex clients_lock;
} GstWebSocketSink;

typedef struct _GstWebSocketSinkClient
{
    struct lws *wsi;
    GstWebSocketSink *sink;
} GstWebSocketSinkClient;

typedef struct _GstWebSocketSinkClass
{
    GstBaseSinkClass parent_class;
} GstWebSocketSinkClass;

#define GST_TYPE_WEBSOCKET_SINK (gst_websocket_sink_get_type())
#define GST_WEBSOCKET_SINK(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_WEBSOCKET_SINK, GstWebSocketSink))
#define GST_WEBSOCKET_SINK_CLASS(klass)                        \
    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_WEBSOCKET_SINK, \
                             GstWebSocketSinkClass))
#define GST_IS_WEBSOCKET_SINK(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_WEBSOCKET_SINK))
#define GST_IS_WEBSOCKET_SINK_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_WEBSOCKET_SINK))

/* clang-format off */

static struct lws_protocols protocols[] = 
{
  {
    "gst-websocket-sink",
    websocket_callback,
    sizeof(GstWebSocketSinkClient), 
    0
  },
  {NULL, NULL, 0, 0}
};

/* clang-format on */

G_DEFINE_TYPE(GstWebSocketSink, gst_websocket_sink, GST_TYPE_BASE_SINK);

/******************************************************************************
 * PRIVATE FUNCTIONS
 ******************************************************************************/

static void gst_websocket_sink_set_property(GObject *object, guint prop_id,
                                            const GValue *value,
                                            GParamSpec *pspec)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(object);

    switch (prop_id)
    {
        case PROP_HOST:
            if(sink->host)
            {
              g_free(sink->host);
            }
            sink->host = g_value_dup_string(value);
            break;
        case PROP_PORT:
            sink->port = g_value_get_uint(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void gst_websocket_sink_get_property(GObject *object, guint prop_id,
                                            GValue *value, GParamSpec *pspec)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(object);

    switch (prop_id)
    {
        case PROP_HOST:
            g_value_set_string(value, sink->host);
            break;
        case PROP_PORT:
            g_value_set_uint(value, sink->port);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static gboolean gst_websocket_sink_start(GstBaseSink *bsink)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(bsink);
    struct lws_context_creation_info info;

    memset(&info, 0, sizeof(info));
    info.port = sink->port;
    info.iface = sink->host;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    if(sink->context)
    {
      lws_context_destroy(sink->context);
      sink->context = NULL;
    }
    sink->context = lws_create_context(&info);
    if (!sink->context)
    {
        GST_ERROR("Failed to create WebSocket context");
        return FALSE;
    }

    if(sink->vhost)
    {
       lws_vhost_destroy(sink->vhost);
       sink->vhost = NULL;
    }
    sink->vhost = lws_create_vhost(sink->context, &info);
    if (!sink->vhost)
    {
        GST_ERROR("Failed to create WebSocket vhost");
        lws_context_destroy(sink->context);
        sink->context = NULL;
        return FALSE;
    }

    GST_INFO("WebSocket server started on %s:%d", sink->host, sink->port);
    return TRUE;
}

static gboolean gst_websocket_sink_stop(GstBaseSink *bsink)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(bsink);

    if (sink->context)
    {
        lws_context_destroy(sink->context);
        sink->context = NULL;
    }

    g_mutex_lock(&sink->clients_lock);
    g_list_free(sink->clients);
    sink->clients = NULL;
    g_mutex_unlock(&sink->clients_lock);

    return TRUE;
}

static GstFlowReturn gst_websocket_sink_render(GstBaseSink *bsink,
                                               GstBuffer *buffer)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(bsink);
    GstMapInfo map;
    GList *iter;

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    {
        return GST_FLOW_ERROR;
    }

    GST_INFO("Render");

    g_mutex_lock(&sink->clients_lock);

    for (iter = sink->clients; iter; iter = iter->next)
    {
        GstWebSocketSinkClient *client = iter->data;

        // TODO: Consider moving this to heap
        unsigned char buf[LWS_PRE + map.size];

        memcpy(buf + LWS_PRE, map.data, map.size);
        lws_write(client->wsi, buf + LWS_PRE, map.size, LWS_WRITE_BINARY);
    }

    g_mutex_unlock(&sink->clients_lock);

    gst_buffer_unmap(buffer, &map);
    return GST_FLOW_OK;
}

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len)
{
    GstWebSocketSinkClient *client = user;

    switch (reason)
    {
        case LWS_CALLBACK_ESTABLISHED: {
            GstWebSocketSink *sink = lws_context_user(lws_get_context(wsi));
            client = user;
            client->wsi = wsi;
            client->sink = sink;

            g_mutex_lock(&sink->clients_lock);
            sink->clients = g_list_append(sink->clients, client);
            g_mutex_unlock(&sink->clients_lock);

            GST_INFO("New client connected");
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            if (!client)
            {
                GST_ERROR("Error getting disconnected client");
                break;
            }

            GstWebSocketSink *sink = client->sink;
            g_mutex_lock(&sink->clients_lock);
            sink->clients = g_list_remove(sink->clients, client);
            g_mutex_unlock(&sink->clients_lock);

            GST_INFO("Client disconnected");
            break;
        }

        default:
            break;
    }

    return 0;
}

static void gst_websocket_sink_finalize(GObject *object)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(object);

    g_free(sink->host);
    g_mutex_clear(&sink->clients_lock);

    G_OBJECT_CLASS(gst_websocket_sink_parent_class)->finalize(object);
}

static void gst_websocket_sink_class_init(GstWebSocketSinkClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);
    GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS(klass);

    gobject_class->set_property = gst_websocket_sink_set_property;
    gobject_class->get_property = gst_websocket_sink_get_property;
    gobject_class->finalize = gst_websocket_sink_finalize;

    g_object_class_install_property(
        gobject_class, PROP_HOST,
        g_param_spec_string("host", "Host", "The host interface to bind to",
                            DEFAULT_HOST,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(
        gobject_class, PROP_PORT,
        g_param_spec_uint("port", "Port", "The port to listen on", 1, 65535,
                          DEFAULT_PORT,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    gst_element_class_set_static_metadata(gstelement_class, "WebSocket Sink",
                                          "Sink/Network",
                                          DESCRIPTION,
                                          AUTHOR);

    gst_element_class_add_static_pad_template(gstelement_class, &sink_template);

    gstbasesink_class->start = GST_DEBUG_FUNCPTR(gst_websocket_sink_start);
    gstbasesink_class->stop = GST_DEBUG_FUNCPTR(gst_websocket_sink_stop);
    gstbasesink_class->render = GST_DEBUG_FUNCPTR(gst_websocket_sink_render);

    GST_INFO("Init done");
}

static void gst_websocket_sink_init(GstWebSocketSink *sink)
{
    sink->host = g_strdup(DEFAULT_HOST);
    sink->port = DEFAULT_PORT;
    sink->context = NULL;
    sink->vhost = NULL;
    sink->clients = NULL;
    g_mutex_init(&sink->clients_lock);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(gst_websocket_sink_debug, "websocketsink", 0,
                            "WebSocket Sink");

    return gst_element_register(plugin, "websocketsink", GST_RANK_NONE,
                                GST_TYPE_WEBSOCKET_SINK);
}

/******************************************************************************
 * INTERFACE
 ******************************************************************************/

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, websocketsink,
                  "WebSocket Sink", plugin_init, VERSION, "MIT", PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)