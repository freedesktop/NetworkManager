# Shared method, Massquerading and IPForwarding

https://bugzilla.redhat.com/show_bug.cgi?id=1548825

## Current Situation

When NetworkManager activates a profile with `ipv4.method=shared`, then the following happens:

- NM uses IP tables to configure masquerading:

```
modprobe: '/sbin/modprobe --use-blacklist ip_tables'
modprobe: '/sbin/modprobe --use-blacklist iptable_nat'
modprobe: '/sbin/modprobe --use-blacklist nf_nat_ftp'
modprobe: '/sbin/modprobe --use-blacklist nf_nat_irc'
modprobe: '/sbin/modprobe --use-blacklist nf_nat_sip'
modprobe: '/sbin/modprobe --use-blacklist nf_nat_tftp'
modprobe: '/sbin/modprobe --use-blacklist nf_nat_pptp'
modprobe: '/sbin/modprobe --use-blacklist nf_nat_h323'
Executing: /usr/sbin/iptables --table filter --insert INPUT --in-interface eth0 --protocol tcp --destination-port 53 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert INPUT --in-interface eth0 --protocol udp --destination-port 53 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert INPUT --in-interface eth0 --protocol tcp --destination-port 67 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert INPUT --in-interface eth0 --protocol udp --destination-port 67 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --in-interface eth0 --jump REJECT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --out-interface eth0 --jump REJECT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --in-interface eth0 --out-interface eth0 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --source 192.168.42.0/255.255.255.0 --in-interface eth0 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --destination 192.168.42.0/255.255.255.0 --out-interface eth0 --match state --state ESTABLISHED,RELATED --jump ACCEPT
Executing: /usr/sbin/iptables --table nat --insert POSTROUTING --source 192.168.42.0/255.255.255.0 ! --destination 192.168.42.0/255.255.255.0 --jump MASQUERADE
Executing: /usr/sbin/iptables --table filter --insert INPUT --in-interface eth0 --protocol tcp --destination-port 53 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert INPUT --in-interface eth0 --protocol udp --destination-port 53 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert INPUT --in-interface eth0 --protocol tcp --destination-port 67 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert INPUT --in-interface eth0 --protocol udp --destination-port 67 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --in-interface eth0 --jump REJECT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --out-interface eth0 --jump REJECT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --in-interface eth0 --out-interface eth0 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --source 192.168.42.0/255.255.255.0 --in-interface eth0 --jump ACCEPT
Executing: /usr/sbin/iptables --table filter --insert FORWARD --destination 192.168.42.0/255.255.255.0 --out-interface eth0 --match state --state ESTABLISHED,RELATED --jump ACCEPT
Executing: /usr/sbin/iptables --table nat --insert POSTROUTING --source 192.168.42.0/255.255.255.0 ! --destination 192.168.42.0/255.255.255.0 --jump MASQUERADE
```

- set `/proc/sys/net/ipv4/ip_forward` to 1

- configure firewalld zone. Usually, when a profile leaves `connection.zone` empty, NetworkManager configures `""` as zone, which is a shortcut for the default in firewalld. When activating a shared profiles (either `ipv4.method=shared` or `ipv6.method=shared`), then instead the pre-installed zone [nm-shared](https://gitlab.freedesktop.org/NetworkManager/NetworkManager/-/blob/caea7514cb74714077fd372c7264c9582d884c1f/data/nm-shared.xml) is used.


## Problems

- This mechanism isn't very flexible, compared to systemd-networkd's `IPForward=` and `IPMasquerade=` settings.

- it happens to mostly work, because on newer systems `iptables` is a wrapper around `nftables`. Still, it's ugly. We should support more backends, in particular `firewalld`.


## Firewalld and Policy Objects

Firewalld 0.9.0 [introduced](https://firewalld.org/2020/09/policy-objects-introduction) policy objects. These are necessary to natively configure masquerading. However, masquerading is based on IP ranges and not interfaces. That means, a policy object with `<maquerading/>` cannot refer to a zone which has interfaces (`addInterface()`). Note that currently NetworkManager always uses `changeZoneOfInterface()`. That does not work. Firewalld will reject this configuration, because a masquerding policy requires addresses (`addSource()`). That means, the user cannot use such policies/zones from NetworkManager.

A solution is to add rich rules to the policy, like

```
firewall-cmd --permanent --policy nm-masquerade --add-rich-rule='rule family=ipv4 source address=10.0.0.0/24 masquerade'
```


## Way Forward

- Optimally, we would only use firewalld. In practice, that seems not a feasible solution. So we need to allow the user to configure the firewall backend, possibly with smart autodetection. Possible backends are `iptables`, `nftables`, and `firewalld`. `systemd` does that.

- the `ipvx.method=shared` should be more flexible, and support new options like `IPForward=` and `IPMasquerade=` (see `man systemd.network`).

- when using firewalld backend, we can pre-deply an XML with a policy object. NetworkManager then adds/removes rich rules with the address range according to the range on the shared interface. Note that handling of this policy object is entirely independent of the `nm-shared` zone that we pre-deploy. The `nm-shared` zone is just a pre-installed, default zone that we use for shared profile, the user use any other zone instead.


### Steps

1) support detecting version of firewalld. Since policy objects are only a recent addition, we will need that for autodetecting the backend.

1) add a firewall-backend configuration option that initially supports iptables and firewalld. The autodetec mechanism choses firewalld if firewalld is running and recent enough. Otherwise, use iptables.

1) rename current `NMFirewallManager` class to `NMFirewalldManager`. The a name is confusing and we will need it.

1) rework `NMUtilsShareRules` to become a proper `NMFirewallManager`. Depending on the configured/detected firewall-backend, it will act.

1) make it configurable whether to enable masquerading (aking to systemd-networkd's `IPMasquerade`).

1) make it configurable whether to enable forwarding (aking to systemd-networkd's `IPForward`). Note that the sysctls are global, handle that well.

1) see what to do about the `modprobe` calls and the additional `iptables` rules that we currently have.

