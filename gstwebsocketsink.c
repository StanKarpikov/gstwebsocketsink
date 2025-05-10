/******************************************************************************
 * INCLUDES
 ******************************************************************************/

#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <libwebsockets.h>
#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>
#include "gst/gstelement.h"

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

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len);

/******************************************************************************
 * PRIVATE DATA AND TYPES
 ******************************************************************************/

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

typedef struct {
    uint8_t *data;
    size_t length;
} QueuedMessage;

typedef struct
{
    struct lws_context *context;
    struct lws_vhost *vhost;
    GList *clients;
    GMutex clients_lock;

    GQueue *send_queue;
    GMutex send_mutex;
    GCond send_cond;
    gboolean waiting_for_send;
    gboolean connection_closed;
} WSContext;

typedef struct _GstWebSocketSink
{
    GstBaseSink parent;

    gchar *host;
    guint port;

    GstTask *ws_task;
    GRecMutex ws_task_lock;
    WSContext ws_context;
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
    "ws",
    ws_callback,
    sizeof(GstWebSocketSinkClient), 
    0
  },
  {NULL, NULL, 0, 0}
};

/* clang-format on */

G_DEFINE_TYPE(GstWebSocketSink, gst_websocket_sink, GST_TYPE_BASE_SINK);

/******************************************************************************
 * PRIVATE FUNCTIONS: WEBSOCKET THREAD
 ******************************************************************************/

static void ws_service_thread(GstWebSocketSink *element)
{
    if (!element)
    {
        GST_ERROR("NULL argument provided to Websocket thread");
    }

    GST_INFO("Starting Websocket thread");
    while (gst_task_get_state(element->ws_task) == GST_TASK_STARTED)
    {
        g_rec_mutex_lock(&element->ws_task_lock);
        if (element->ws_context.context)
        {
            lws_service(element->ws_context.context, 100);
        }
        g_rec_mutex_unlock(&element->ws_task_lock);
    }
    GST_INFO("Exiting Websocket thread");
}

static void ws_context_cleanup(WSContext *ws_context)
{
    GST_INFO("Websocket cleanup");
    if (!ws_context)
    {
        return;
    }

    if (ws_context->vhost)
    {
        lws_vhost_destroy(ws_context->vhost);
        ws_context->vhost = NULL;
    }

    g_mutex_lock(&ws_context->clients_lock);
    g_list_free(ws_context->clients);
    ws_context->clients = NULL;
    g_mutex_unlock(&ws_context->clients_lock);

    if (ws_context->context)
    {
        lws_context_destroy(ws_context->context);
        ws_context->context = NULL;
    }
}

static bool ws_initialise(WSContext *ws_context, struct lws_context_creation_info *info)
{
    GST_INFO("Websocket initialise");
    if (!ws_context || !info)
    {
        return FALSE;
    }

    ws_context_cleanup(ws_context);

    lws_set_log_level(LLL_ERR | LLL_WARN /* | LLL_NOTICE | LLL_DEBUG */, NULL);

    ws_context->context = lws_create_context(info);
    if (!ws_context->context)
    {
        ws_context_cleanup(ws_context);
        GST_ERROR("Failed to create WebSocket context");
        return FALSE;
    }

    ws_context->vhost = lws_create_vhost(ws_context->context, info);
    if (!ws_context->vhost)
    {
        GST_ERROR("Failed to create WebSocket vhost");
        ws_context_cleanup(ws_context);
        return FALSE;
    }

    GST_INFO("WebSocket server started on %s:%d", info->iface, info->port);
    return TRUE;
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    GstWebSocketSinkClient *client = user;
    switch (reason)
    {
      case LWS_CALLBACK_SERVER_WRITEABLE: {
            g_mutex_lock(&client->sink->ws_context.send_mutex);

            if (!g_queue_is_empty(client->sink->ws_context.send_queue)) {
                QueuedMessage *msg = g_queue_pop_head(client->sink->ws_context.send_queue);

                // Allocate with LWS_PRE padding
                unsigned char *buf = g_malloc(LWS_PRE + msg->length);
                memcpy(buf + LWS_PRE, msg->data, msg->length);

                int written = lws_write(wsi, buf + LWS_PRE, msg->length, LWS_WRITE_BINARY);
                g_free(buf);
                g_free(msg->data);
                g_free(msg);

                client->sink->ws_context.waiting_for_send = FALSE;
                g_cond_signal(&client->sink->ws_context.send_cond);
            }

            g_mutex_unlock(&client->sink->ws_context.send_mutex);
            break;
        }
        case LWS_CALLBACK_ESTABLISHED: {
            struct lws_context *context = lws_get_context(wsi);
            if (!context)
            {
                GST_ERROR("Failed to get context, lws_get_context returned NULL");
                break;
            }
            GstWebSocketSink *sink = lws_context_user(context);
            if (!sink)
            {
                GST_ERROR("Failed to get sink, lws_context_user returned NULL");
                break;
            }

            client = user;
            client->wsi = wsi;
            client->sink = sink;

            g_mutex_lock(&sink->ws_context.clients_lock);
            sink->ws_context.clients = g_list_append(sink->ws_context.clients, client);
            g_mutex_unlock(&sink->ws_context.clients_lock);

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
            if (!sink)
            {
                GST_ERROR("Failed to get sink, client->sink is NULL");
                break;
            }
            g_mutex_lock(&sink->ws_context.clients_lock);
            sink->ws_context.clients = g_list_remove(sink->ws_context.clients, client);
            g_mutex_unlock(&sink->ws_context.clients_lock);

            GST_INFO("Client disconnected");
            break;
        }

        default:
            break;
    }

    return 0;
}

GstFlowReturn ws_send_data(GstWebSocketSinkClient *client, struct lws *wsi, const uint8_t *data, size_t length) {
    QueuedMessage *msg = g_new(QueuedMessage, 1);
    msg->data = g_memdup2(data,   length);
    msg->length = length;

    g_mutex_lock(&client->sink->ws_context.send_mutex);
    g_queue_push_tail(client->sink->ws_context.send_queue, msg);
    client->sink->ws_context.waiting_for_send = TRUE;

    lws_callback_on_writable(wsi);

    while (client->sink->ws_context.waiting_for_send && !client->sink->ws_context.connection_closed) {
        g_cond_wait(&client->sink->ws_context.send_cond, &client->sink->ws_context.send_mutex);
    }

    gboolean success = !client->sink->ws_context.connection_closed;
    g_mutex_unlock(&client->sink->ws_context.send_mutex);

    return success ? GST_FLOW_OK : GST_FLOW_ERROR;
}

/******************************************************************************
 * PRIVATE FUNCTIONS: GSTREAMER
 ******************************************************************************/

static void gst_websocket_sink_set_property(GObject *object, guint prop_id,
                                            const GValue *value,
                                            GParamSpec *pspec)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(object);

    switch (prop_id)
    {
        case PROP_HOST:
            if (sink->host)
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
    info.user = sink;

    return ws_initialise(&sink->ws_context, &info);
}

static gboolean gst_websocket_sink_stop(GstBaseSink *bsink)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(bsink);

    ws_context_cleanup(&sink->ws_context);

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

    g_mutex_lock(&sink->ws_context.clients_lock);

    for (iter = sink->ws_context.clients; iter; iter = iter->next)
    {
        GstWebSocketSinkClient *client = iter->data;

        GstFlowReturn ret = ws_send_data(client, 
                                         client->wsi,
                                         (const uint8_t *)map.data,
                                         map.size);
        if (ret != GST_FLOW_OK)
        {
            GST_WARNING("Failed to send WebSocket data");
        }
        // // TODO: Consider moving this to heap
        // unsigned char buf[LWS_PRE + map.size];

        // memcpy(buf + LWS_PRE, map.data, map.size);

        // GST_INFO("Send data size: %lu", map.size);
        // int m = lws_write(client->wsi, buf + LWS_PRE, map.size, LWS_WRITE_BINARY);
        // if (m < map.size) {
        //   GST_ERROR("Error %d writing to ws socket", m);
        // }
    }

    g_mutex_unlock(&sink->ws_context.clients_lock);

    gst_buffer_unmap(buffer, &map);
    return GST_FLOW_OK;
}

static void gst_websocket_sink_finalize(GObject *object)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(object);

    g_free(sink->host);
    g_mutex_clear(&sink->ws_context.clients_lock);

    if (sink->ws_task)
    {
        gst_task_stop(sink->ws_task);
        gst_object_unref(sink->ws_task);
    }
    g_rec_mutex_clear(&sink->ws_task_lock);

    G_OBJECT_CLASS(gst_websocket_sink_parent_class)->finalize(object);
}

static GstStateChangeReturn
gst_websocket_sink_change_state(GstElement *element, GstStateChange transition)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(element);
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

    switch (transition)
    {
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            if (!gst_task_start(sink->ws_task))
            {
                GST_ERROR_OBJECT(sink, "Failed to start Websocket task");
                return GST_STATE_CHANGE_FAILURE;
            }
            break;

        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            /* Task keeps running in PLAYING */
            break;

        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            /* Task keeps running in PAUSED */
            break;

        case GST_STATE_CHANGE_PAUSED_TO_READY:
            GST_INFO("Attempting to stop the Websocket thread...");
            lws_cancel_service(sink->ws_context.context);
            gst_task_stop(sink->ws_task);
            gst_task_join(sink->ws_task);
            break;

        default:
            break;
    }

    ret = GST_ELEMENT_CLASS(gst_websocket_sink_parent_class)->change_state(element, transition);

    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        /* Ensure task is stopped if state change failed */
        if (transition == GST_STATE_CHANGE_READY_TO_PAUSED)
        {
            GST_INFO("Attempting to stop the Websocket thread...");
            lws_cancel_service(sink->ws_context.context);
            gst_task_stop(sink->ws_task);
            gst_task_join(sink->ws_task);
        }
    }

    return ret;
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
    gstelement_class->change_state = GST_DEBUG_FUNCPTR(gst_websocket_sink_change_state);

    GST_INFO("Init done");
}

static void gst_websocket_sink_init(GstWebSocketSink *sink)
{
    sink->host = g_strdup(DEFAULT_HOST);
    sink->port = DEFAULT_PORT;
    sink->ws_context.context = NULL;
    sink->ws_context.vhost = NULL;
    sink->ws_context.clients = NULL;
    g_mutex_init(&sink->ws_context.clients_lock);

    g_rec_mutex_init(&sink->ws_task_lock);
    sink->ws_task = gst_task_new((GstTaskFunction)ws_service_thread, sink, NULL);
    gst_task_set_lock(sink->ws_task, &sink->ws_task_lock);
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