#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

typedef struct {
  uint32_t clb;
  uint32_t clbu;
  uint32_t fb;
  uint32_t fbu;
  uint32_t is;
  uint32_t ie;
  uint32_t cmd;
  uint32_t res0;
  uint32_t tfd;
  uint32_t sig;
  uint32_t ssts;
  uint32_t sctl;
  uint32_t serr;
  uint32_t sact;
  uint32_t ci;
  uint32_t sntf;
  uint32_t fbs;
  uint32_t res1[11];
  uint32_t vendor[4];
} __attribute__((packed)) ahci_port_t;

#endif
