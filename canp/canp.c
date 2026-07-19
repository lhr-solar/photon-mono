#include "canp.h"

#include <asm-generic/errno-base.h>
#include <netinet/in.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <inttypes.h>

static uint64_t canpHton64(uint64_t value){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t high = htonl((uint32_t)(value >> 32));
    uint32_t low = htonl((uint32_t)value);
    return ((uint64_t)low << 32) | high;
#else
    return value;
#endif
}

static uint64_t canpNtoh64(uint64_t value){
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t high = ntohl((uint32_t)value);
    uint32_t low = ntohl((uint32_t)(value >> 32));
    return ((uint64_t)high << 32) | low;
#else
    return value;
#endif
}

canpPacket_t canpMakePacket(uint32_t canId, uint8_t dlc,
                            const uint8_t data[8], const uint16_t δt[8]){
    canpPacket_t p;
    p.can_id = htonl(canId);
    p.dlc = dlc;
    for(int i = 0; i < 8; i++) p.data[i] = data[i];
    for(int i = 0; i < 8; i++) p.δt[i] = htons(δt[i]);
    return p;
};

uint32_t canpGetId(const canpPacket_t* p){ return ntohl(p->can_id); }

int canpWrite(int fd, struct iovec *iov, int iovcnt) {
    while(iovcnt > 0){
        ssize_t n = writev(fd, iov, iovcnt);
        if(n < 0){
            if(errno == EINTR) continue;
            return -1;
        }
        if(n == 0) return -1;
        while(iovcnt > 0 && (size_t)n >= iov[0].iov_len){
            n -= iov[0].iov_len;
            iov++;
            iovcnt--;
        }
        if(iovcnt > 0){
            iov[0].iov_base = (char *)iov[0].iov_base + n;
            iov[0].iov_len -= n;
        }
    }
    return 1;
}

int canpWriteBatch(int fd, canpBatch_t* batch) {
    if(batch->count == 0 || batch->count > CANP_MAX_BATCH) return -1;
    canpHeader_t hdr = {
        .magic = htonl(CANP_MAGIC),
        .version = htons(CANP_VERSION),
        .count = htons(batch->count),
        .seq = htonl(batch->seq),
        .timestamp = canpHton64(batch->timestamp)
    };
    struct iovec iov[2] = {
        {.iov_base = &hdr, .iov_len = sizeof hdr},
        {.iov_base = (void*)batch->packets, .iov_len = batch->count * sizeof batch->packets[0]}
    };
    return canpWrite(fd, iov, 2);
}

int canpRead(int fd, void* buf, size_t len){
    char *p = (char*)buf;
    size_t accum = 0;
    while(accum < len){
        ssize_t n = read(fd, p + accum, len - accum);
        if(n == 0) return 0;
        if(n < 0) {
            if(errno == EINTR) continue;
            return -1;
        }
        accum += n;
    }
    return 1;
}

int canpReadBatch(int fd, canpBatch_t* batch){
    canpHeader_t hdr;
    int r = canpRead(fd, &hdr, sizeof hdr);
    if(r <= 0) return r;
    if(ntohl(hdr.magic) != CANP_MAGIC) return -1;
    if(ntohs(hdr.version) != CANP_VERSION) return -1;
    uint16_t n = ntohs(hdr.count);
    if(n == 0 || n > CANP_MAX_BATCH) return -1;
    r = canpRead(fd, batch->packets, n * sizeof batch->packets[0]);
    if(r <= 0) return r;
    batch->seq = ntohl(hdr.seq);
    batch->timestamp = canpNtoh64(hdr.timestamp);
    batch->count = n;
    return 1;
}

int canpRelayBatch(int in_fd, int out_fd) {
    canpBatch_t batch;
    int r = canpReadBatch(in_fd, &batch);
    if (r <= 0) return r;
    return canpWriteBatch(out_fd, &batch);
}

static inline void dataToString(const uint8_t data[8], uint8_t dlc, char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (uint8_t i = 0; i < dlc; i++) {
        s[i * 2 + 0] = hex[data[i] >> 4];
        s[i * 2 + 1] = hex[data[i] & 0x0F];
    }
    s[dlc * 2] = '\0';
}

void canpPrintBatch(canpBatch_t* batch){
    printf("[Ts|%" PRIu64 "][Seq|%" PRIu32 "]\n",
           batch->timestamp, batch->seq);
    for(uint16_t i = 0; i < batch->count; i++){
        const canpPacket_t *p = &batch->packets[i];
        char s[17];
        dataToString(p->data, p->dlc, s);
        printf("\t[0x%03" PRIX32 "][%u][%s]\n", canpGetId(p), p->dlc, s);
    }
};
