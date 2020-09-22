/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2005 - 2017 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 */

#include "src/core/nm-default-daemon.h"

#include "nm-ip-config.h"

#include "nm-l3cfg.h"

/*****************************************************************************/

GType nm_ip4_config_get_type(void);
GType nm_ip6_config_get_type(void);

/*****************************************************************************/

typedef struct _NMIPConfigPrivate NMIPConfigPrivate;

NM_GOBJECT_PROPERTIES_DEFINE_FULL(_ip, NMIPConfig, PROP_IP_L3CFG, PROP_IP_IS_VPN, );

G_DEFINE_ABSTRACT_TYPE(NMIPConfig, nm_ip_config, NM_TYPE_DBUS_OBJECT)

#define NM_IP_CONFIG_GET_PRIVATE(self) _NM_GET_PRIVATE(self, NMIPConfig, NM_IS_IP_CONFIG)

/*****************************************************************************/

static void
get_property_ip(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NMIPConfig *       self = NM_IP_CONFIG(object);
    NMIPConfigPrivate *priv = NM_IP_CONFIG_GET_PRIVATE(self);

    (void) priv;  //XXX

    switch (prop_id) {
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    NMIPConfig *       self = NM_IP_CONFIG(object);
    NMIPConfigPrivate *priv = NM_IP_CONFIG_GET_PRIVATE(self);
    gpointer           ptr;

    switch (prop_id) {
    case PROP_IP_L3CFG:
        /* construct-only */
        ptr = g_value_get_pointer(value);
        nm_assert(NM_IS_L3CFG(ptr));
        priv->l3cfg = g_object_ref(ptr);
        break;
    case PROP_IP_IS_VPN:
        /* construct-only */
        priv->is_vpn = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

/*****************************************************************************/

static void
nm_ip_config_init(NMIPConfig *self)
{}

NMIPConfig *
nm_ip_config_new(int addr_family, NML3Cfg *l3cfg, gboolean is_vpn)
{
    nm_assert_addr_family(addr_family);
    nm_assert(NM_L3CFG(l3cfg));

    return g_object_new(NM_IS_IPv4(addr_family) ? nm_ip4_config_get_type()
                                                : nm_ip6_config_get_type(),
                        NM_IP_CONFIG_L3CFG,
                        l3cfg,
                        NM_IP_CONFIG_IS_VPN,
                        is_vpn,
                        NULL);
}

void
nm_ip_config_take_and_unexport_on_idle(NMIPConfig *self_take)
{
    if (!self_take)
        return;

    //XXX: freeze the instance so that no more signals are emitted while
    //     the instance gets unexported.

    nm_dbus_object_unexport_on_idle(g_steal_pointer(&self_take));
}

static void
finalize(GObject *object)
{
    NMIPConfig *       self = NM_IP_CONFIG(object);
    NMIPConfigPrivate *priv = NM_IP_CONFIG_GET_PRIVATE(self);

    g_object_unref(priv->l3cfg);

    G_OBJECT_CLASS(nm_ip_config_parent_class)->finalize(object);
}

static void
nm_ip_config_class_init(NMIPConfigClass *klass)
{
    GObjectClass *     object_class      = G_OBJECT_CLASS(klass);
    NMDBusObjectClass *dbus_object_class = NM_DBUS_OBJECT_CLASS(klass);

    object_class->get_property = get_property_ip;
    object_class->set_property = set_property;
    object_class->finalize     = finalize;

    dbus_object_class->export_on_construction = TRUE;

    obj_properties_ip[PROP_IP_L3CFG] =
        g_param_spec_pointer(NM_IP_CONFIG_L3CFG,
                             "",
                             "",
                             G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    obj_properties_ip[PROP_IP_IS_VPN] =
        g_param_spec_boolean(NM_IP_CONFIG_IS_VPN,
                             "",
                             "",
                             FALSE,
                             G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST_ip, obj_properties_ip);
}

/*****************************************************************************/

/* public*/
#define NM_IP4_CONFIG_ADDRESS_DATA     "address-data"
#define NM_IP4_CONFIG_DNS_OPTIONS      "dns-options"
#define NM_IP4_CONFIG_DNS_PRIORITY     "dns-priority"
#define NM_IP4_CONFIG_DOMAINS          "domains"
#define NM_IP4_CONFIG_GATEWAY          "gateway"
#define NM_IP4_CONFIG_NAMESERVER_DATA  "nameserver-data"
#define NM_IP4_CONFIG_ROUTE_DATA       "route-data"
#define NM_IP4_CONFIG_SEARCHES         "searches"
#define NM_IP4_CONFIG_WINS_SERVER_DATA "wins-server-data"

/* deprecated */
#define NM_IP4_CONFIG_ADDRESSES    "addresses"
#define NM_IP4_CONFIG_NAMESERVERS  "nameservers"
#define NM_IP4_CONFIG_ROUTES       "routes"
#define NM_IP4_CONFIG_WINS_SERVERS "wins-servers"

typedef struct _NMIP4Config      NMIP4Config;
typedef struct _NMIP4ConfigClass NMIP4ConfigClass;

NM_GOBJECT_PROPERTIES_DEFINE_FULL(_ip4,
                                  NMIP4Config,
                                  PROP_IP4_ADDRESSES,
                                  PROP_IP4_ADDRESS_DATA,
                                  PROP_IP4_DNS_OPTIONS,
                                  PROP_IP4_DNS_PRIORITY,
                                  PROP_IP4_DOMAINS,
                                  PROP_IP4_GATEWAY,
                                  PROP_IP4_NAMESERVERS,
                                  PROP_IP4_NAMESERVER_DATA,
                                  PROP_IP4_ROUTES,
                                  PROP_IP4_ROUTE_DATA,
                                  PROP_IP4_SEARCHES,
                                  PROP_IP4_WINS_SERVERS,
                                  PROP_IP4_WINS_SERVER_DATA, );

struct _NMIP4Config {
    NMIPConfig parent;
};

struct _NMIP4ConfigClass {
    NMIPConfigClass parent;
};

G_DEFINE_TYPE(NMIP4Config, nm_ip4_config, NM_TYPE_IP_CONFIG)

static void
get_property_ip4(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NMIPConfig *       self = NM_IP_CONFIG(object);
    NMIPConfigPrivate *priv = NM_IP_CONFIG_GET_PRIVATE(self);

    (void) priv;  //XXX

    switch (prop_id) {
    default:
        return;  //XXX
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static const NMDBusInterfaceInfoExtended interface_info_ip4_config = {
    .parent = NM_DEFINE_GDBUS_INTERFACE_INFO_INIT(
        NM_DBUS_INTERFACE_IP4_CONFIG,
        .properties = NM_DEFINE_GDBUS_PROPERTY_INFOS(
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Addresses",
                                                           "aau",
                                                           NM_IP4_CONFIG_ADDRESSES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("AddressData",
                                                           "aa{sv}",
                                                           NM_IP4_CONFIG_ADDRESS_DATA),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Gateway", "s", NM_IP4_CONFIG_GATEWAY),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Routes", "aau", NM_IP4_CONFIG_ROUTES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("RouteData",
                                                           "aa{sv}",
                                                           NM_IP4_CONFIG_ROUTE_DATA),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("NameserverData",
                                                           "aa{sv}",
                                                           NM_IP4_CONFIG_NAMESERVER_DATA),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Nameservers",
                                                           "au",
                                                           NM_IP4_CONFIG_NAMESERVERS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Domains", "as", NM_IP4_CONFIG_DOMAINS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Searches",
                                                           "as",
                                                           NM_IP4_CONFIG_SEARCHES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("DnsOptions",
                                                           "as",
                                                           NM_IP4_CONFIG_DNS_OPTIONS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("DnsPriority",
                                                           "i",
                                                           NM_IP4_CONFIG_DNS_PRIORITY),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("WinsServerData",
                                                           "as",
                                                           NM_IP4_CONFIG_WINS_SERVER_DATA),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("WinsServers",
                                                           "au",
                                                           NM_IP4_CONFIG_WINS_SERVERS), ), ),
};

static void
nm_ip4_config_init(NMIP4Config *self)
{}

static void
nm_ip4_config_class_init(NMIP4ConfigClass *klass)
{
    GObjectClass *     object_class      = G_OBJECT_CLASS(klass);
    NMDBusObjectClass *dbus_object_class = NM_DBUS_OBJECT_CLASS(klass);
    NMIPConfigClass *  ip_config_class   = NM_IP_CONFIG_CLASS(klass);

    ip_config_class->addr_family = AF_INET;

    dbus_object_class->export_path     = NM_DBUS_EXPORT_PATH_NUMBERED(NM_DBUS_PATH "/IP4Config");
    dbus_object_class->interface_infos = NM_DBUS_INTERFACE_INFOS(&interface_info_ip4_config);

    object_class->get_property = get_property_ip4;

    obj_properties_ip4[PROP_IP4_ADDRESS_DATA] =
        g_param_spec_variant(NM_IP4_CONFIG_ADDRESS_DATA,
                             "",
                             "",
                             G_VARIANT_TYPE("aa{sv}"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_ADDRESSES] =
        g_param_spec_variant(NM_IP4_CONFIG_ADDRESSES,
                             "",
                             "",
                             G_VARIANT_TYPE("aau"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_ROUTE_DATA] =
        g_param_spec_variant(NM_IP4_CONFIG_ROUTE_DATA,
                             "",
                             "",
                             G_VARIANT_TYPE("aa{sv}"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_ROUTES] =
        g_param_spec_variant(NM_IP4_CONFIG_ROUTES,
                             "",
                             "",
                             G_VARIANT_TYPE("aau"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_GATEWAY] =
        g_param_spec_string(NM_IP4_CONFIG_GATEWAY,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_NAMESERVER_DATA] =
        g_param_spec_variant(NM_IP4_CONFIG_NAMESERVER_DATA,
                             "",
                             "",
                             G_VARIANT_TYPE("aa{sv}"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_NAMESERVERS] =
        g_param_spec_variant(NM_IP4_CONFIG_NAMESERVERS,
                             "",
                             "",
                             G_VARIANT_TYPE("au"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_DOMAINS] =
        g_param_spec_boxed(NM_IP4_CONFIG_DOMAINS,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_SEARCHES] =
        g_param_spec_boxed(NM_IP4_CONFIG_SEARCHES,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_DNS_OPTIONS] =
        g_param_spec_boxed(NM_IP4_CONFIG_DNS_OPTIONS,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_DNS_PRIORITY] =
        g_param_spec_int(NM_IP4_CONFIG_DNS_PRIORITY,
                         "",
                         "",
                         G_MININT32,
                         G_MAXINT32,
                         0,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_WINS_SERVER_DATA] =
        g_param_spec_variant(NM_IP4_CONFIG_WINS_SERVER_DATA,
                             "",
                             "",
                             G_VARIANT_TYPE("as"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip4[PROP_IP4_WINS_SERVERS] =
        g_param_spec_variant(NM_IP4_CONFIG_WINS_SERVERS,
                             "",
                             "",
                             G_VARIANT_TYPE("au"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST_ip4, obj_properties_ip4);
}

/*****************************************************************************/

/* public */
#define NM_IP6_CONFIG_ADDRESS_DATA "address-data"
#define NM_IP6_CONFIG_ROUTE_DATA   "route-data"
#define NM_IP6_CONFIG_GATEWAY      "gateway"
#define NM_IP6_CONFIG_NAMESERVERS  "nameservers"
#define NM_IP6_CONFIG_DOMAINS      "domains"
#define NM_IP6_CONFIG_SEARCHES     "searches"
#define NM_IP6_CONFIG_DNS_OPTIONS  "dns-options"
#define NM_IP6_CONFIG_DNS_PRIORITY "dns-priority"

/* deprecated */
#define NM_IP6_CONFIG_ADDRESSES "addresses"
#define NM_IP6_CONFIG_ROUTES    "routes"

typedef struct _NMIP6Config      NMIP6Config;
typedef struct _NMIP6ConfigClass NMIP6ConfigClass;

NM_GOBJECT_PROPERTIES_DEFINE_FULL(_ip6,
                                  NMIP6Config,
                                  PROP_IP6_ADDRESS_DATA,
                                  PROP_IP6_ROUTE_DATA,
                                  PROP_IP6_GATEWAY,
                                  PROP_IP6_NAMESERVERS,
                                  PROP_IP6_DOMAINS,
                                  PROP_IP6_SEARCHES,
                                  PROP_IP6_DNS_OPTIONS,
                                  PROP_IP6_DNS_PRIORITY,
                                  PROP_IP6_ADDRESSES,
                                  PROP_IP6_ROUTES, );

struct _NMIP6Config {
    NMIPConfig parent;
};

struct _NMIP6ConfigClass {
    NMIPConfigClass parent;
};

G_DEFINE_TYPE(NMIP6Config, nm_ip6_config, NM_TYPE_IP_CONFIG)

static const NMDBusInterfaceInfoExtended interface_info_ip6_config = {
    .parent = NM_DEFINE_GDBUS_INTERFACE_INFO_INIT(
        NM_DBUS_INTERFACE_IP6_CONFIG,
        .properties = NM_DEFINE_GDBUS_PROPERTY_INFOS(
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Addresses",
                                                           "a(ayuay)",
                                                           NM_IP6_CONFIG_ADDRESSES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("AddressData",
                                                           "aa{sv}",
                                                           NM_IP6_CONFIG_ADDRESS_DATA),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Gateway", "s", NM_IP6_CONFIG_GATEWAY),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Routes",
                                                           "a(ayuayu)",
                                                           NM_IP6_CONFIG_ROUTES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("RouteData",
                                                           "aa{sv}",
                                                           NM_IP6_CONFIG_ROUTE_DATA),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Nameservers",
                                                           "aay",
                                                           NM_IP6_CONFIG_NAMESERVERS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Domains", "as", NM_IP6_CONFIG_DOMAINS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("Searches",
                                                           "as",
                                                           NM_IP6_CONFIG_SEARCHES),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("DnsOptions",
                                                           "as",
                                                           NM_IP6_CONFIG_DNS_OPTIONS),
            NM_DEFINE_DBUS_PROPERTY_INFO_EXTENDED_READABLE("DnsPriority",
                                                           "i",
                                                           NM_IP6_CONFIG_DNS_PRIORITY), ), ),
};

static void
get_property_ip6(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    NMIPConfig *       self = NM_IP_CONFIG(object);
    NMIPConfigPrivate *priv = NM_IP_CONFIG_GET_PRIVATE(self);

    (void) priv;  //XXX

    switch (prop_id) {
    //XXX
    default:
        return;  //XXX
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
nm_ip6_config_init(NMIP6Config *self)
{}

static void
nm_ip6_config_class_init(NMIP6ConfigClass *klass)
{
    GObjectClass *     object_class      = G_OBJECT_CLASS(klass);
    NMDBusObjectClass *dbus_object_class = NM_DBUS_OBJECT_CLASS(klass);
    NMIPConfigClass *  ip_config_class   = NM_IP_CONFIG_CLASS(klass);

    ip_config_class->addr_family = AF_INET6;

    dbus_object_class->export_path     = NM_DBUS_EXPORT_PATH_NUMBERED(NM_DBUS_PATH "/IP6Config");
    dbus_object_class->interface_infos = NM_DBUS_INTERFACE_INFOS(&interface_info_ip6_config);

    object_class->get_property = get_property_ip6;

    obj_properties_ip6[PROP_IP6_ADDRESS_DATA] =
        g_param_spec_variant(NM_IP6_CONFIG_ADDRESS_DATA,
                             "",
                             "",
                             G_VARIANT_TYPE("aa{sv}"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_ADDRESSES] =
        g_param_spec_variant(NM_IP6_CONFIG_ADDRESSES,
                             "",
                             "",
                             G_VARIANT_TYPE("a(ayuay)"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_ROUTE_DATA] =
        g_param_spec_variant(NM_IP6_CONFIG_ROUTE_DATA,
                             "",
                             "",
                             G_VARIANT_TYPE("aa{sv}"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_ROUTES] =
        g_param_spec_variant(NM_IP6_CONFIG_ROUTES,
                             "",
                             "",
                             G_VARIANT_TYPE("a(ayuayu)"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_GATEWAY] =
        g_param_spec_string(NM_IP6_CONFIG_GATEWAY,
                            "",
                            "",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_NAMESERVERS] =
        g_param_spec_variant(NM_IP6_CONFIG_NAMESERVERS,
                             "",
                             "",
                             G_VARIANT_TYPE("aay"),
                             NULL,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_DOMAINS] =
        g_param_spec_boxed(NM_IP6_CONFIG_DOMAINS,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_SEARCHES] =
        g_param_spec_boxed(NM_IP6_CONFIG_SEARCHES,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_DNS_OPTIONS] =
        g_param_spec_boxed(NM_IP6_CONFIG_DNS_OPTIONS,
                           "",
                           "",
                           G_TYPE_STRV,
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    obj_properties_ip6[PROP_IP6_DNS_PRIORITY] =
        g_param_spec_int(NM_IP6_CONFIG_DNS_PRIORITY,
                         "",
                         "",
                         G_MININT32,
                         G_MAXINT32,
                         0,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST_ip6, obj_properties_ip6);
}
