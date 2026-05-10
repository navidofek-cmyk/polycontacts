# ── Build stage ──────────────────────────────────────────────────────────────
FROM golang:1.22-alpine AS builder

WORKDIR /src

# Cache dependency downloads separately from the build.
COPY go.mod ./
RUN go mod download

COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /gateway-go .

# ── Final stage ───────────────────────────────────────────────────────────────
FROM alpine:3.19

# curl is needed for the Docker HEALTHCHECK.
RUN apk add --no-cache curl

COPY --from=builder /gateway-go /usr/local/bin/gateway-go

EXPOSE 9000

HEALTHCHECK --interval=15s --timeout=5s --start-period=5s --retries=3 \
    CMD curl -fs http://localhost:9000/health || exit 1

ENTRYPOINT ["/usr/local/bin/gateway-go"]
