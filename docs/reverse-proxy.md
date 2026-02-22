# WhatsApp Webhook Setup

WhatsApp Cloud API delivers messages via webhooks — Meta's servers POST to your
URL. PtrClaw includes a built-in HTTP server to receive these calls. It binds to
localhost and must sit behind a reverse proxy that handles TLS and rate-limiting.

```
Meta Cloud API ──► reverse proxy (TLS, rate-limit) ──► 127.0.0.1:8080 (ptrclaw)
```

## Prerequisites

- A server with a public IP and a domain name (e.g. `bot.example.com`)
- A TLS certificate (e.g. [Let's Encrypt](https://letsencrypt.org/))
- A reverse proxy installed (nginx or Caddy)
- Meta WhatsApp Business credentials (see [README](../README.md#how-to-get-whatsapp-cloud-api-credentials))

## Step-by-step setup

### 1. Generate a shared secret

This secret authenticates requests from the proxy to ptrclaw. Generate one:

```bash
openssl rand -hex 32
```

Save the output — you'll use it in both the ptrclaw config and the proxy config.

### 2. Configure ptrclaw

Add to `~/.ptrclaw/config.json`:

```json
{
  "channels": {
    "whatsapp": {
      "access_token":    "EAAx…",
      "phone_number_id": "123456789",
      "verify_token":    "your-meta-verify-token",
      "webhook_listen":  "127.0.0.1:8080",
      "webhook_secret":  "your-generated-secret"
    }
  }
}
```

Or via environment variables:

```bash
export WHATSAPP_ACCESS_TOKEN="EAAx…"
export WHATSAPP_PHONE_ID="123456789"
export WHATSAPP_VERIFY_TOKEN="your-meta-verify-token"
export WHATSAPP_WEBHOOK_LISTEN="127.0.0.1:8080"
export WHATSAPP_WEBHOOK_SECRET="your-generated-secret"
```

| Key | Purpose | Default |
|-----|---------|---------|
| `webhook_listen` | Bind address for the local HTTP server | *(required)* |
| `webhook_secret` | Value the proxy must send in `X-Webhook-Secret` | *(no check)* |
| `webhook_max_body` | Max POST body size in bytes | `65536` (64 KB) |

### 3. Configure the reverse proxy

The proxy terminates TLS, rate-limits, and injects the `X-Webhook-Secret` header.

#### nginx

```nginx
# Rate-limit zone: 10 req/s per IP, shared 1 MB memory.
limit_req_zone $binary_remote_addr zone=webhook:1m rate=10r/s;

server {
    listen 443 ssl;
    server_name bot.example.com;

    ssl_certificate     /etc/letsencrypt/live/bot.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/bot.example.com/privkey.pem;

    location /webhook {
        limit_req zone=webhook burst=20 nodelay;
        limit_req_status 429;

        client_max_body_size 64k;

        proxy_pass         http://127.0.0.1:8080;
        proxy_set_header   Host $host;
        proxy_set_header   X-Webhook-Secret "your-generated-secret";
        proxy_read_timeout 10s;
    }
}
```

#### Caddy

```caddy
bot.example.com {
    @webhook path /webhook
    handle @webhook {
        reverse_proxy 127.0.0.1:8080 {
            header_up X-Webhook-Secret "your-generated-secret"
        }
    }
}
```

### 4. Start ptrclaw

```bash
./ptrclaw --channel whatsapp
```

PtrClaw starts the webhook server, logs the bind address, and enters its event
loop. Start ptrclaw before registering the webhook with Meta (next step), because
Meta immediately sends a verification request.

### 5. Register the webhook with Meta

1. Go to [Meta for Developers](https://developers.facebook.com/) → your app → **WhatsApp** → **Configuration**.
2. Set **Callback URL** to `https://bot.example.com/webhook`.
3. Set **Verify token** to the same `verify_token` you configured in step 2.
4. Click **Verify and save**. Meta sends a GET request to your endpoint — ptrclaw
   responds with the challenge token and Meta confirms the webhook is active.
5. Subscribe to the **messages** webhook field.

You should now receive WhatsApp messages in ptrclaw.

## Webhook endpoints

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/webhook` | Meta verification handshake (`hub.mode=subscribe`) |
| `POST` | `/webhook` | Incoming message delivery |

All other methods return `405 Method Not Allowed`.
Payloads exceeding `webhook_max_body` return `413 Payload Too Large`.

## Security notes

- **Do not expose ptrclaw directly to the internet.** Always use a reverse proxy
  for TLS and rate-limiting.
- **`webhook_secret`** creates a trust channel between the proxy and ptrclaw. The
  proxy injects the header; ptrclaw rejects POSTs without it. Use a long random
  string (≥ 32 bytes). Rotate by restarting both the proxy and ptrclaw.
- **`verify_token`** is separate: it authenticates the initial GET verification
  handshake from Meta. Use a different value from `webhook_secret`.
- **Rate-limiting and TLS** are the proxy's responsibility. PtrClaw does not
  implement them.
- **Telegram** is unaffected — it uses long-polling and does not need a webhook
  server.
