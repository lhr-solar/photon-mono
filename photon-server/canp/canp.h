#ifndef CAN_PROTO_H
#define CAN_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <bits/types/struct_iovec.h>

#define CANP_MAGIC     0x43414E31u  /* "CAN1" */
#define CANP_VERSION   3u
#define CANP_MAX_BATCH 64u

/* we assume single threaded            */
/* these functions are not thread safe  */
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t seq;
  uint64_t timestamp;
} canpHeader_t;

typedef struct __attribute__((packed)) {
  uint32_t can_id;
  uint8_t dlc;
  uint8_t data[8];
  uint16_t δt[8];
} canpPacket_t;

typedef struct {
  uint32_t seq;
  uint64_t timestamp;
  uint16_t count;
  canpPacket_t packets[CANP_MAX_BATCH];
} canpBatch_t;

int canpWrite(int fd, struct iovec* iov, int iovcnt);
int canpRead(int fd, void *buf, size_t n);

int canpWriteBatch(int fd, canpBatch_t* batch);
int canpReadBatch(int fd, canpBatch_t* batch);

int canpRelayBatch(int in_fd, int out_fd);
void canpPrintBatch(canpBatch_t* batch);

uint32_t canpGetId(const canpPacket_t* p);
canpPacket_t canpMakePacket(uint32_t canId, uint8_t dlc,
                            const uint8_t data[8], const uint16_t δt[8]);

#endif
