# Reverse-Proxy Setup for WhatsApp Webhooks

PtrClaw includes a minimal HTTP server for receiving WhatsApp Cloud API webhooks.
It is designed to run **on localhost only** and to sit behind a hardened reverse proxy
(nginx, Caddy, Traefik, …) that handles TLS, rate-limiting, and DDoS mitigation.

```
Internet ──► reverse proxy (TLS, rate-limit) ──► 127.0.0.1:8080 (ptrclaw)
                  ↑ Meta Cloud API webhooks
```

---

## Configuration

Add to `~/.ptrclaw/config.json`:

```json
{
  "channels": {
    "whatsapp": {
      "access_token":    "EAAx…",
      "phone_number_id": "123456789",
      "verify_token":    "your-meta-verify-token",
      "webhook_listen":  "127.0.0.1:8080",
      "webhook_secret":  "a-strong-random-secret"
    }
  }
}
```

Or via environment variables:

```
WHATSAPP_ACCESS_TOKEN=EAAx…
WHATSAPP_PHONE_ID=123456789
WHATSAPP_VERIFY_TOKEN=your-meta-verify-token
WHATSAPP_WEBHOOK_LISTEN=127.0.0.1:8080
WHATSAPP_WEBHOOK_SECRET=a-strong-random-secret
```

| Key | Purpose | Default |
|-----|---------|---------|
| `webhook_listen` | Bind address for the local HTTP server | *(disabled)* |
| `webhook_secret` | Value the proxy must send in `X-Webhook-Secret` | *(no check)* |
| `webhook_max_body` | Max POST body size in bytes | `65536` (64 KB) |

`webhook_secret` creates a shared-secret trust channel between the proxy and ptrclaw.
Use a long random string (≥ 32 bytes). The proxy appends this header; ptrclaw rejects
any POST that does not present the exact value.

---

## Webhook endpoints

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/webhook` | Meta webhook verification (`hub.mode=subscribe`) |
| `POST` | `/webhook` | Incoming message delivery |

All other methods receive `405 Method Not Allowed`.
Payloads exceeding `webhook_max_body` receive `413 Payload Too Large`.

---

## Running

```bash
./ptrclaw --channel whatsapp
```

PtrClaw starts the webhook server, logs the bind address, and enters its event loop.
Start ptrclaw before the proxy so the port is ready.

---

## nginx

```nginx
server {
    listen 443 ssl;
    server_name bot.example.com;

    ssl_certificate     /etc/letsencrypt/live/bot.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/bot.example.com/privkey.pem;

    # Meta only sends to your configured webhook URL path.
    location /webhook {
        # Drop connections faster than nginx can accept them (DDoS mitigation).
        limit_req zone=webhook burst=20 nodelay;
        limit_req_status 429;

        # Reject oversized bodies before they reach ptrclaw.
        client_max_body_size 64k;

        proxy_pass         http://127.0.0.1:8080;
        proxy_set_header   Host $host;
        proxy_set_header   X-Webhook-Secret "a-strong-random-secret";
        proxy_read_timeout 10s;
    }
}

# Rate-limit zone: 10 req/s per IP, shared 1 MB memory.
limit_req_zone $binary_remote_addr zone=webhook:1m rate=10r/s;
```

---

## Caddy

```caddy
bot.example.com {
    @webhook path /webhook
    handle @webhook {
        reverse_proxy 127.0.0.1:8080 {
            header_up X-Webhook-Secret "a-strong-random-secret"
        }
    }
}
```

---

## Docker Compose example

```yaml
services:
  ptrclaw:
    image: ptrclaw:latest
    restart: unless-stopped
    environment:
      WHATSAPP_ACCESS_TOKEN:    "${WHATSAPP_ACCESS_TOKEN}"
      WHATSAPP_PHONE_ID:        "${WHATSAPP_PHONE_ID}"
      WHATSAPP_VERIFY_TOKEN:    "${WHATSAPP_VERIFY_TOKEN}"
      WHATSAPP_WEBHOOK_LISTEN:  "0.0.0.0:8080"   # nginx in same compose net
      WHATSAPP_WEBHOOK_SECRET:  "${WEBHOOK_SECRET}"
      ANTHROPIC_API_KEY:        "${ANTHROPIC_API_KEY}"
    networks:
      - internal
    command: ["./ptrclaw", "--channel", "whatsapp"]

  nginx:
    image: nginx:alpine
    restart: unless-stopped
    ports:
      - "443:443"
    volumes:
      - ./nginx.conf:/etc/nginx/conf.d/default.conf:ro
      - /etc/letsencrypt:/etc/letsencrypt:ro
    networks:
      - internal
      - external
    depends_on:
      - ptrclaw

networks:
  internal:
  external:
```

> **Note**: When ptrclaw and nginx share a Docker network `ptrclaw` is not reachable
> from the internet directly. Set `WHATSAPP_WEBHOOK_LISTEN=0.0.0.0:8080` so nginx
> can reach it on the internal network. DDoS protection lives entirely in nginx.

---

## Security notes

- `webhook_listen` defaults to `127.0.0.1` (loopback). Do not bind `0.0.0.0` unless
  the port is firewalled or isolated to a trusted Docker network.
- `webhook_secret` should be a random string generated with e.g.
  `openssl rand -hex 32`. Rotate it by restarting both the proxy and ptrclaw.
- Meta's `hub.verify_token` is separate: it authenticates the GET verification
  handshake initiated by Meta. Use a different value from `webhook_secret`.
- Rate-limiting, connection throttling, and TLS are the proxy's responsibility.
  ptrclaw itself does not implement them.
- Telegram is unaffected by this feature; it continues to use long-polling and
  does not need a webhook server.
