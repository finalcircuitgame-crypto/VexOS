#ifndef VEXOS_E1000E_NETDEV_H
#define VEXOS_E1000E_NETDEV_H

/* Register the already-initialized PCI E1000e driver with the generic stack. */
int e1000e_netdev_attach(void);

#endif
