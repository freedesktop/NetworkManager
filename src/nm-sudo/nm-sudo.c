/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "libnm-glib-aux/nm-default-glib-i18n-prog.h"

#include "libnm-glib-aux/nm-logging-fwd.h"
#include "libnm-glib-aux/nm-logging-base.h"
#include "libnm-glib-aux/nm-shared-utils.h"
#include "libnm-glib-aux/nm-time-utils.h"
#include "libnm-glib-aux/nm-dbus-aux.h"

/* nm-sudo doesn't link with libnm-core, but nm-dbus-interface.h header
 * can be used independently. */
#include "libnm-core-public/nm-dbus-interface.h"

/*****************************************************************************/

#define IDLE_TIMEOUT_SEC 10

#define NM_SUDO_DBUS_BUS_NAME    "org.freedesktop.nm.sudo"
#define NM_SUDO_DBUS_OBJECT_PATH "/org/freedesktop/nm/sudo"
#define NM_SUDO_DBUS_IFACE_NAME  "org.freedesktop.nm.sudo"

/*****************************************************************************/

/* Serves only the purpose to mark environment variables that are honored by
 * the application. You can search for this macro, and find what options are supported. */
#define _ENV(var) ("" var "")

/*****************************************************************************/

extern volatile NMLogLevel _nm_logging_configured_level;

static inline gboolean
nm_logging_enabled(NMLogLevel level)
{
    return level >= _nm_logging_configured_level;
}

void _nm_logging_enabled_init(const char *level_str);

void _nm_log_impl_sudo(NMLogLevel level, const char *fmt, ...) _nm_printf(2, 3);

#define _nm_log(level, ...) _nm_log_impl_sudo((level), __VA_ARGS__);

#define _NMLOG(level, ...)                 \
    G_STMT_START                           \
    {                                      \
        const NMLogLevel _level = (level); \
                                           \
        if (nm_logging_enabled(_level)) {  \
            _nm_log(_level, __VA_ARGS__);  \
        }                                  \
    }                                      \
    G_STMT_END

/*****************************************************************************/

volatile NMLogLevel _nm_logging_configured_level = LOGL_TRACE;

void
_nm_logging_enabled_init(const char *level_str)
{
    NMLogLevel level;

    if (!_nm_log_parse_level(level_str, &level))
        level = LOGL_WARN;
    else if (level == _LOGL_KEEP)
        level = LOGL_WARN;

    _nm_logging_configured_level = level;
}

void
_nm_log_impl_sudo(NMLogLevel level, const char *fmt, ...)
{
    gs_free char *msg = NULL;
    va_list       ap;
    const char *  level_str;
    gint64        ts;

    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    switch (level) {
    case LOGL_TRACE:
        level_str = "<trace>";
        break;
    case LOGL_DEBUG:
        level_str = "<debug>";
        break;
    case LOGL_INFO:
        level_str = "<info> ";
        break;
    case LOGL_WARN:
        level_str = "<warn> ";
        break;
    default:
        nm_assert(level == LOGL_ERR);
        level_str = "<error>";
        break;
    }

    ts = nm_utils_clock_gettime_nsec(CLOCK_BOOTTIME);

    g_print("[%" G_GINT64_FORMAT ".%05" G_GINT64_FORMAT "] %s %s\n",
            ts / NM_UTILS_NSEC_PER_SEC,
            (ts / (NM_UTILS_NSEC_PER_SEC / 10000)) % 10000,
            level_str,
            msg);
}

/*****************************************************************************/

typedef struct {
    GCancellable *   quit_cancellable;
    GDBusConnection *dbus_connection;
    GMainLoop *      main_loop;
    GSource *        source_sigterm;
    GSource *        source_idle_timeout;
    char *           name_owner;
    guint            name_owner_changed_id;
    guint            service_regist_id;
    int              timeout_sec;
    bool             name_owner_initialized;
    bool             service_registered;
    bool             is_debug;
} GlobalData;

/*****************************************************************************/

static void
_handle_ping(GlobalData *gl, GDBusMethodInvocation *invocation, const char *arg)
{
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", arg));
}

/*****************************************************************************/

static gboolean
_signal_callback_term(gpointer user_data)
{
    GCancellable *quit_cancellable = user_data;

    g_cancellable_cancel(quit_cancellable);
    return G_SOURCE_CONTINUE;
}

/*****************************************************************************/

typedef struct {
    GMainLoop *       main_loop;
    GDBusConnection **p_dbus_connection;
    GError **         p_error;
} BusGetData;

static void
_bus_get_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    BusGetData *data = user_data;

    *data->p_dbus_connection = g_bus_get_finish(result, data->p_error);
    g_main_loop_quit(data->main_loop);
}

static GDBusConnection *
_bus_get(GCancellable *cancellable, int *out_exit_code)
{
    nm_auto_unref_gmainloop GMainLoop *main_loop     = NULL;
    gs_free_error GError *error                      = NULL;
    gs_unref_object GDBusConnection *dbus_connection = NULL;
    BusGetData                       data;

    main_loop = g_main_loop_new(g_main_context_get_thread_default(), FALSE);

    data = (BusGetData){
        .main_loop         = main_loop,
        .p_dbus_connection = &dbus_connection,
        .p_error           = &error,
    };

    g_bus_get(G_BUS_TYPE_SYSTEM, cancellable, _bus_get_cb, &data);

    g_main_loop_run(main_loop);

    if (!dbus_connection) {
        gboolean was_cancelled = nm_utils_error_is_cancelled(error);

        NM_SET_OUT(out_exit_code, was_cancelled ? EXIT_SUCCESS : EXIT_FAILURE);
        if (!was_cancelled)
            _LOGE("dbus: failure to get D-Bus connection: %s", error->message);
        return NULL;
    }

    /* On bus-disconnect, GDBus will raise(SIGTERM), which we handle like a
     * regular request to quit. */
    g_dbus_connection_set_exit_on_close(dbus_connection, TRUE);

    _LOGD("dbus: unique name: %s", g_dbus_connection_get_unique_name(dbus_connection));

    return g_steal_pointer(&dbus_connection);
}

/*****************************************************************************/

static void
_name_owner_changed_cb(GDBusConnection *connection,
                       const char *     sender_name,
                       const char *     object_path,
                       const char *     interface_name,
                       const char *     signal_name,
                       GVariant *       parameters,
                       gpointer         user_data)
{
    GlobalData *gl = user_data;
    const char *new_owner;

    if (!gl->name_owner_initialized)
        return;

    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)")))
        return;

    g_variant_get(parameters, "(&s&s&s)", NULL, NULL, &new_owner);
    new_owner = nm_str_not_empty(new_owner);

    _LOGD("%s name-owner changed: %s -> %s",
          NM_DBUS_SERVICE,
          gl->name_owner ?: "(null)",
          new_owner ?: "(null)");

    nm_utils_strdup_reset(&gl->name_owner, new_owner);
}

typedef struct {
    GlobalData *gl;
    char **     p_name_owner;
    gboolean    is_cancelled;
} BusFindNMNameOwnerData;

static void
_bus_find_nm_nameowner_cb(const char *name_owner, GError *error, gpointer user_data)
{
    BusFindNMNameOwnerData *data = user_data;

    *data->p_name_owner              = nm_strdup_not_empty(name_owner);
    data->is_cancelled               = nm_utils_error_is_cancelled(error);
    data->gl->name_owner_initialized = TRUE;

    g_main_loop_quit(data->gl->main_loop);
}

static gboolean
_bus_find_nm_nameowner(GlobalData *gl)
{
    BusFindNMNameOwnerData data;
    guint                  name_owner_changed_id;
    gs_free char *         name_owner = NULL;

    name_owner_changed_id =
        nm_dbus_connection_signal_subscribe_name_owner_changed(gl->dbus_connection,
                                                               NM_DBUS_SERVICE,
                                                               _name_owner_changed_cb,
                                                               gl,
                                                               NULL);

    data = (BusFindNMNameOwnerData){
        .gl           = gl,
        .is_cancelled = FALSE,
        .p_name_owner = &name_owner,
    };
    nm_dbus_connection_call_get_name_owner(gl->dbus_connection,
                                           NM_DBUS_SERVICE,
                                           10000,
                                           gl->quit_cancellable,
                                           _bus_find_nm_nameowner_cb,
                                           &data);
    g_main_loop_run(gl->main_loop);

    if (data.is_cancelled) {
        g_dbus_connection_signal_unsubscribe(gl->dbus_connection, name_owner_changed_id);
        return FALSE;
    }

    gl->name_owner_changed_id = name_owner_changed_id;
    gl->name_owner            = g_steal_pointer(&name_owner);
    return TRUE;
}

/*****************************************************************************/

static void
_bus_method_call(GDBusConnection *      connection,
                 const gchar *          sender,
                 const gchar *          object_path,
                 const gchar *          interface_name,
                 const gchar *          method_name,
                 GVariant *             parameters,
                 GDBusMethodInvocation *invocation,
                 gpointer               user_data)
{
    GlobalData *gl = user_data;
    const char *arg;

    nm_assert(nm_streq(object_path, NM_SUDO_DBUS_OBJECT_PATH));
    nm_assert(nm_streq(interface_name, NM_SUDO_DBUS_IFACE_NAME));

    if (!gl->is_debug && !nm_streq0(sender, gl->name_owner)) {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR,
                                              G_DBUS_ERROR_ACCESS_DENIED,
                                              "Access denied");
        return;
    }

    _LOGT("dbus: request sender=%s, %s%s",
          sender,
          method_name,
          g_variant_get_type_string(parameters));

    if (nm_streq(method_name, "Ping")) {
        g_variant_get(parameters, "(&s)", &arg);
        _handle_ping(gl, invocation, arg);
        return;
    }

    nm_assert_not_reached();
}

static GDBusInterfaceInfo *const interface_info = NM_DEFINE_GDBUS_INTERFACE_INFO(
    NM_SUDO_DBUS_IFACE_NAME,
    .methods = NM_DEFINE_GDBUS_METHOD_INFOS(
        NM_DEFINE_GDBUS_METHOD_INFO(
            "Ping",
            .in_args  = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("arg", "s"), ),
            .out_args = NM_DEFINE_GDBUS_ARG_INFOS(NM_DEFINE_GDBUS_ARG_INFO("arg", "s"), ), ), ), );

typedef struct {
    GlobalData *gl;
    gboolean    success;
} BusRegisterServiceRequestNameData;

static void
_bus_register_service_request_name_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
    BusRegisterServiceRequestNameData *data = user_data;
    gs_free_error GError *error             = NULL;
    gs_unref_variant GVariant *ret          = NULL;
    gboolean                   success      = FALSE;

    ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

    if (nm_utils_error_is_cancelled(error))
        goto out;

    if (error)
        _LOGE("d-bus: failed to request name %s: %s", NM_SUDO_DBUS_BUS_NAME, error->message);
    else {
        guint32 ret_val;

        g_variant_get(ret, "(u)", &ret_val);
        if (ret_val != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
            _LOGW("dbus: request name for %s failed to take name (response %u)",
                  NM_SUDO_DBUS_BUS_NAME,
                  ret_val);
        } else {
            _LOGD("dbus: request name for %s succeeded", NM_SUDO_DBUS_BUS_NAME);
            success = TRUE;
        }
    }

out:
    data->success = success;
    g_main_loop_quit(data->gl->main_loop);
}

static void
_bus_register_service(GlobalData *gl)
{
    static const GDBusInterfaceVTable interface_vtable = {
        .method_call = _bus_method_call,
    };
    gs_free_error GError *            error = NULL;
    BusRegisterServiceRequestNameData data;

    gl->service_regist_id =
        g_dbus_connection_register_object(gl->dbus_connection,
                                          NM_SUDO_DBUS_OBJECT_PATH,
                                          interface_info,
                                          NM_UNCONST_PTR(GDBusInterfaceVTable, &interface_vtable),
                                          gl,
                                          NULL,
                                          &error);
    if (gl->service_regist_id == 0) {
        _LOGE("dbus: error registering object %s: %s", NM_SUDO_DBUS_OBJECT_PATH, error->message);
        return;
    }

    _LOGD("dbus: object %s registered", NM_SUDO_DBUS_OBJECT_PATH);

    data = (BusRegisterServiceRequestNameData){
        .gl = gl,
    };

    g_dbus_connection_call(
        gl->dbus_connection,
        DBUS_SERVICE_DBUS,
        DBUS_PATH_DBUS,
        DBUS_INTERFACE_DBUS,
        "RequestName",
        g_variant_new("(su)",
                      NM_SUDO_DBUS_BUS_NAME,
                      (guint) (DBUS_NAME_FLAG_ALLOW_REPLACEMENT | DBUS_NAME_FLAG_REPLACE_EXISTING)),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        gl->quit_cancellable,
        _bus_register_service_request_name_cb,
        &data);

    g_main_loop_run(gl->main_loop);

    gl->service_registered = data.success;
}

/*****************************************************************************/

static gboolean
_idle_timeout_cb(gpointer user_data)
{
    GlobalData *gl = user_data;

    _LOGT("idle-timeout: expired");
    g_main_loop_quit(gl->main_loop);
    return G_SOURCE_CONTINUE;
}

static void
_idle_timeout_restart(GlobalData *gl)
{
    nm_clear_g_source_inst(&gl->source_idle_timeout);

    _LOGT("idle-timeout: start (%d seconds)", gl->timeout_sec);
    gl->source_idle_timeout =
        nm_g_timeout_add_source_seconds(gl->timeout_sec, _idle_timeout_cb, gl);
}

/*****************************************************************************/

static void
_setup(GlobalData *gl)
{
    gl->is_debug    = _nm_utils_ascii_str_to_int64(g_getenv(_ENV("NM_SUDO_DEBUG")), 0, 0, 1, 0);
    gl->timeout_sec = _nm_utils_ascii_str_to_int64(g_getenv(_ENV("NM_SUDO_IDLE_TIMEOUT")),
                                                   0,
                                                   1,
                                                   G_MAXINT32,
                                                   IDLE_TIMEOUT_SEC);

    gl->quit_cancellable = g_cancellable_new();

    gl->main_loop = g_main_loop_new(NULL, FALSE);

    signal(SIGPIPE, SIG_IGN);
    gl->source_sigterm =
        nm_g_unix_signal_add_source(SIGTERM, _signal_callback_term, gl->quit_cancellable);
}

int
main(int argc, char **argv)
{
    GlobalData _gl = {
        .quit_cancellable = NULL,
    };
    GlobalData *const gl = &_gl;
    int               exit_code;
    int               r = 0;

    _nm_logging_enabled_init(g_getenv(_ENV("NM_SUDO_LOG")));

    _LOGD("starting nm-sudo (%s)", NM_DIST_VERSION);

    _setup(gl);

    if (gl->is_debug)
        _LOGW("WARNING: running in debug mode without authentication (NM_SUDO_DEBUG). ");

    gl->dbus_connection = _bus_get(gl->quit_cancellable, &r);
    if (!gl->dbus_connection) {
        exit_code = r;
        goto done;
    }

    if (!_bus_find_nm_nameowner(gl)) {
        /* abort due to cancellation. That is success. */
        exit_code = EXIT_SUCCESS;
        goto done;
    }
    _LOGD("%s name-owner: %s", NM_DBUS_SERVICE, gl->name_owner ?: "(null)");

    _bus_register_service(gl);
    if (!gl->service_registered) {
        exit_code = EXIT_FAILURE;
        goto done;
    }

    _idle_timeout_restart(gl);

    g_main_loop_run(gl->main_loop);

    exit_code = EXIT_SUCCESS;

done:
    if (gl->service_regist_id != 0) {
        g_dbus_connection_unregister_object(gl->dbus_connection,
                                            nm_steal_int(&gl->service_regist_id));
    }
    if (gl->name_owner_changed_id != 0) {
        g_dbus_connection_signal_unsubscribe(gl->dbus_connection,
                                             nm_steal_int(&gl->name_owner_changed_id));
    }
    nm_clear_g_cancellable(&gl->quit_cancellable);
    g_clear_object(&gl->dbus_connection);
    nm_clear_pointer(&gl->main_loop, g_main_loop_unref);
    nm_clear_g_source_inst(&gl->source_sigterm);
    nm_clear_g_source_inst(&gl->source_idle_timeout);
    nm_clear_g_free(&gl->name_owner);
    _LOGD("exit (%d)", exit_code);
    return exit_code;
}
