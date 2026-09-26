FROM golang:1.22-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY *.go ./
COPY cmd/grok-analyst ./cmd/grok-analyst
RUN CGO_ENABLED=0 go build -o /out/grok-analyst ./cmd/grok-analyst

FROM alpine:3.20
RUN adduser -D -H analyst
USER analyst
COPY --from=build /out/grok-analyst /usr/local/bin/grok-analyst
EXPOSE 18765
ENTRYPOINT ["/usr/local/bin/grok-analyst"]
