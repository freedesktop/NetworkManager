/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "src/core/nm-default-daemon.h"

#include "nm-firewall-manager.h"

#include "nm-core-utils.h"

/*****************************************************************************/

typedef struct {
    int i;
} NMFirewallManagerPrivate;

struct _NMFirewallManager {
    GObject                  parent;
    NMFirewallManagerPrivate _priv;
};

struct _NMFirewallManagerClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(NMFirewallManager, nm_firewall_manager, G_TYPE_OBJECT);

#define NM_FIREWALL_MANAGER_GET_PRIVATE(self) \
    _NM_GET_PRIVATE(self, NMFirewallManager, NM_IS_FIREWALL_MANAGER)

/*****************************************************************************/

NM_DEFINE_SINGLETON_GETTER(NMFirewallManager, nm_firewall_manager_get, NM_TYPE_FIREWALL_MANAGER);

/*****************************************************************************/

#define _NMLOG_DOMAIN      LOGD_FIREWALL
#define _NMLOG_PREFIX_NAME "firewall"
#define _NMLOG(level, ...)                                                \
    G_STMT_START                                                          \
    {                                                                     \
        if (nm_logging_enabled((level), (_NMLOG_DOMAIN))) {               \
            char _prefix_name[30];                                        \
                                                                          \
            _nm_log((level),                                              \
                    (_NMLOG_DOMAIN),                                      \
                    0,                                                    \
                    NULL,                                                 \
                    NULL,                                                 \
                    "%s: " _NM_UTILS_MACRO_FIRST(__VA_ARGS__),            \
                    (((self) != singleton_instance) ? ({                  \
                        g_snprintf(_prefix_name,                          \
                                   sizeof(_prefix_name),                  \
                                   "%s[" NM_HASH_OBFUSCATE_PTR_FMT "]",   \
                                   ""_NMLOG_PREFIX_NAME,                  \
                                   NM_HASH_OBFUSCATE_PTR(self));          \
                        _prefix_name;                                     \
                    })                                                    \
                                                    : _NMLOG_PREFIX_NAME) \
                        _NM_UTILS_MACRO_REST(__VA_ARGS__));               \
        }                                                                 \
    }                                                                     \
    G_STMT_END

/*****************************************************************************/

static void
nm_firewall_manager_init(NMFirewallManager *self)
{
    NMFirewallManagerPrivate *priv = NM_FIREWALL_MANAGER_GET_PRIVATE(self);

    (void) priv;  //XXX
}

static void
dispose(GObject *object)
{
    NMFirewallManager *       self = NM_FIREWALL_MANAGER(object);
    NMFirewallManagerPrivate *priv = NM_FIREWALL_MANAGER_GET_PRIVATE(self);

    (void) priv;  //XXX

    G_OBJECT_CLASS(nm_firewall_manager_parent_class)->dispose(object);
}

static void
nm_firewall_manager_class_init(NMFirewallManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = dispose;
}
