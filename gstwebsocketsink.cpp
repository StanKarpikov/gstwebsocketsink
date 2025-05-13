/******************************************************************************
 * INCLUDES
 ******************************************************************************/

#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>
#include <list>
#include <memory>
#include "gst/gstelement.h"
#include "websocketpp/config/asio_no_tls.hpp"
#include "websocketpp/server.hpp"

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
 * PRIVATE DATA AND TYPES
 ******************************************************************************/

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

typedef websocketpp::server<websocketpp::config::asio> server;
typedef std::weak_ptr<void> connection_hdl;

struct WSContext
{
    std::list<connection_hdl> connections;
    std::mutex connections_mutex;
    server ws_endpoint;
    std::mutex stop_mutex;
    bool should_stop;
};

typedef struct _GstWebSocketSink
{
    GstBaseSink parent;

    gchar *host;
    guint port;

    GstTask *ws_task;
    GRecMutex ws_task_lock;
    std::shared_ptr<WSContext> ws_context;
} GstWebSocketSink;

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

G_DEFINE_TYPE(GstWebSocketSink, gst_websocket_sink, GST_TYPE_BASE_SINK);

/******************************************************************************
 * PRIVATE FUNCTIONS: WEBSOCKET THREAD
 ******************************************************************************/

static void ws_service_thread(GstWebSocketSink *sink)
{
    if (!sink)
    {
        GST_ERROR("NULL argument provided to Websocket thread");
    }

    GST_INFO("Starting Websocket thread");

    if(!sink->ws_context)
    {
        GST_ERROR("NULL context");
        GstElement *parent = GST_ELEMENT_PARENT(sink);
        if (parent)
        {
            GST_ELEMENT_ERROR(parent, STREAM, FAILED,
                              ("WebSocket requested shutdown"), (NULL));
        }
        return;
    }

    try
    {
        sink->ws_context->ws_endpoint.set_reuse_addr(true);
        sink->ws_context->ws_endpoint.listen(boost::asio::ip::tcp::endpoint(boost::asio::ip::address::from_string(sink->host), sink->port));
        sink->ws_context->ws_endpoint.start_accept();
    }
    catch (websocketpp::exception const &e)
    {
        GST_ERROR("Websocket initialisation error: %s", e.what());
        GstElement *parent = GST_ELEMENT_PARENT(sink);
        if (parent)
        {
            GST_ELEMENT_ERROR(parent, STREAM, FAILED,
                              ("WebSocket requested shutdown"), (NULL));
        }
        return;
    }

    while (gst_task_get_state(sink->ws_task) == GST_TASK_STARTED)
    {
        g_rec_mutex_lock(&sink->ws_task_lock);

        try
        {
            GST_INFO("Websocket run");
            sink->ws_context->ws_endpoint.run();
        }
        catch (websocketpp::exception const &e)
        {
            GST_ERROR("Websocket loop error: %s", e.what());
        }

        g_rec_mutex_unlock(&sink->ws_task_lock);
    }
    GST_INFO("Exiting Websocket thread");
}

static bool ws_initialise(std::shared_ptr<WSContext> ws_context, GstWebSocketSink *sink)
{
    GST_INFO("Websocket initialise");
    if (!ws_context)
    {
        return FALSE;
    }

    try
    {
        ws_context->ws_endpoint.init_asio();

        // Enable to see more logs
        // ws_context->ws_endpoint.clear_access_channels(websocketpp::log::alevel::all);
        // ws_context->ws_endpoint.set_access_channels(websocketpp::log::alevel::access_core);
        // ws_context->ws_endpoint.set_access_channels(websocketpp::log::alevel::app);

        ws_context->ws_endpoint.set_access_channels(websocketpp::log::alevel::none);  // No access logs
        ws_context->ws_endpoint.set_error_channels(websocketpp::log::elevel::none);   // No error logs

        ws_context->ws_endpoint.set_open_handler([ws_context](connection_hdl hdl) {
            GST_INFO("Client connected");
            std::lock_guard<std::mutex> lock(ws_context->connections_mutex);
            ws_context->connections.push_back(hdl);
        });

        ws_context->ws_endpoint.set_close_handler([ws_context](connection_hdl hdl) {
            GST_INFO("Client disconnected");
            std::lock_guard<std::mutex> lock(ws_context->connections_mutex);
            ws_context->connections.remove_if([&hdl](const connection_hdl &entry) {
                return !hdl.owner_before(entry) && !entry.owner_before(hdl);
            });
        });
    }
    catch (websocketpp::exception const &e)
    {
        GST_ERROR("Websocket initialisation error: %s", e.what());
        return FALSE;
    }
    return TRUE;
}

static GstFlowReturn ws_send_data(GstWebSocketSink *sink, const uint8_t *data, size_t length)
{
    try
    {
        std::lock_guard<std::mutex> lock(sink->ws_context->connections_mutex);
        for (auto &hdl : sink->ws_context->connections)
        {
            try
            {
                sink->ws_context->ws_endpoint.send(hdl, data, length, websocketpp::frame::opcode::binary);
            }
            catch (websocketpp::exception const &e)
            {
                GST_ERROR("Websocket send error: %s", e.what());
            }
        }
    }
    catch (websocketpp::exception const &e)
    {
        GST_ERROR("Websocket send loop error: %s", e.what());
    }
    return GST_FLOW_OK;
}

static void ws_stop_server(std::shared_ptr<WSContext> ctx)
{
    try
    {
        std::lock_guard<std::mutex> lock(ctx->stop_mutex);
        if (!ctx->should_stop)
        {
            GST_INFO("Stopping task");
            ctx->should_stop = true;
            // Stop accepting new connections
            ctx->ws_endpoint.stop_listening();
            // Close all existing connections
            for (auto &hdl : ctx->connections)
            {
                ctx->ws_endpoint.close(hdl, websocketpp::close::status::going_away, "Server shutdown");
            }
            // Force-stop the ASIO event loop
            ctx->ws_endpoint.get_io_service().stop();
        }
    }
    catch (websocketpp::exception const &e)
    {
        GST_ERROR("Websocket stopping error: %s", e.what());
    }
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
    return ws_initialise(sink->ws_context, sink);
}

static gboolean gst_websocket_sink_stop(GstBaseSink *bsink)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(bsink);

    ws_stop_server(sink->ws_context);

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

    GstFlowReturn ret = ws_send_data(sink,
                                     (const uint8_t *)map.data,
                                     map.size);
    if (ret != GST_FLOW_OK)
    {
        GST_WARNING("Failed to send WebSocket data");
    }

    gst_buffer_unmap(buffer, &map);
    return GST_FLOW_OK;
}

static void gst_websocket_sink_finalize(GObject *object)
{
    GstWebSocketSink *sink = GST_WEBSOCKET_SINK(object);

    if (sink->ws_task)
    {
        ws_stop_server(sink->ws_context);
        gst_task_stop(sink->ws_task);
        gst_object_unref(sink->ws_task);
    }
    g_free(sink->host);
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
            sink->ws_context->should_stop = false;
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
            ws_stop_server(sink->ws_context);
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
            ws_stop_server(sink->ws_context);
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
                            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(
        gobject_class, PROP_PORT,
        g_param_spec_uint("port", "Port", "The port to listen on", 1, 65535,
                          DEFAULT_PORT,
                          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

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

    try
    {
        if(!sink->ws_context)
        {
            sink->ws_context = std::make_shared<WSContext>();
        }
        sink->ws_context->should_stop = false;
    }
    catch (websocketpp::exception const &e)
    {
        GST_ERROR("Could not init Websocket: %s", e.what());
    }

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
                  "WebSocket Sink", plugin_init, VERSION, "MIT/X11", PACKAGE_NAME,
                  GST_PACKAGE_ORIGIN)