/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __NM_FIREWALL_MANAGER_H__
#define __NM_FIREWALL_MANAGER_H__

#define NM_TYPE_FIREWALL_MANAGER (nm_firewall_manager_get_type())
#define NM_FIREWALL_MANAGER(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), NM_TYPE_FIREWALL_MANAGER, NMFirewallManager))
#define NM_FIREWALL_MANAGER_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), NM_TYPE_FIREWALL_MANAGER, NMFirewallManagerClass))
#define NM_IS_FIREWALL_MANAGER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), NM_TYPE_FIREWALL_MANAGER))
#define NM_IS_FIREWALL_MANAGER_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), NM_TYPE_FIREWALL_MANAGER))
#define NM_FIREWALL_MANAGER_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), NM_TYPE_FIREWALL_MANAGER, NMFirewallManagerClass))

typedef struct _NMFirewallManager      NMFirewallManager;
typedef struct _NMFirewallManagerClass NMFirewallManagerClass;

GType nm_firewall_manager_get_type(void);

NMFirewallManager *nm_firewall_manager_get(void);

#endif /* __NM_FIREWALL_MANAGER_H__ */
