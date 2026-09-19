# Build stage
FROM golang:1.24-alpine AS builder

WORKDIR /app

# Copy dependency manifests
COPY go.mod go.sum ./
RUN go mod download

# Copy source code
COPY server/ ./server/
COPY observer/ ./observer/

# Build static binaries
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /app/bin/photon-server ./server
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /app/bin/photon-observer ./observer

# Final runtime stage
FROM alpine:3.21

RUN apk --no-cache add ca-certificates tzdata

# Create non-root user
RUN addgroup -S photon && adduser -S photon -G photon

WORKDIR /app

COPY --from=builder /app/bin/photon-server /usr/local/bin/photon-server
COPY --from=builder /app/bin/photon-observer /usr/local/bin/photon-observer

# Create persistent data directory and set permissions
RUN mkdir -p /data/segments && chown -R photon:photon /data

USER photon

ENV PHOTON_HOST=0.0.0.0 \
    PHOTON_INGRESS_PORT=6100 \
    PHOTON_CLIENT_PORT=6500 \
    PHOTON_OBSERVER_PORT=5400 \
    PHOTON_VIDEO_INGRESS_PORT=6600 \
    PHOTON_VIDEO_CLIENT_PORT=6700 \
    PHOTON_DATA_DIR=/data

# 6100: CANP Ingress, 6500: Client Fan-out, 5400: Observer,
# 6600/udp: RTP/H.264 ingress, 6700: video live/replay
EXPOSE 6100 6500 5400 6600/udp 6700

VOLUME ["/data"]

ENTRYPOINT ["/usr/local/bin/photon-server"]
