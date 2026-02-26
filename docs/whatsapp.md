# WhatsApp Channel Setup

> **Note:** WhatsApp is not included in the default build. Enable it with `-Dwith_whatsapp=true` at configure time (see [Feature flags](../README.md#feature-flags)).

## Getting Cloud API credentials

Use Meta's WhatsApp Business Platform (Cloud API).

1. Go to [Meta for Developers](https://developers.facebook.com/), create/select an app.
2. Add the **WhatsApp** product to the app.
3. In **WhatsApp > API Setup**, copy:
   - **Temporary access token** (for testing) or create a **system user permanent token**
   - **Phone number ID**
4. Configure webhook in **WhatsApp > Configuration**:
   - Callback URL: your public webhook endpoint
   - Verify token: choose a secret string (you define this)
   - Subscribe to `messages` (and other events you need)
5. Add values to env/config:

```sh
export WHATSAPP_ACCESS_TOKEN="EAA..."
export WHATSAPP_PHONE_ID="123456789"
export WHATSAPP_VERIFY_TOKEN="your-verify-secret"
```

Or set them in `~/.ptrclaw/config.json` under `channels.whatsapp`.

## Starting the webhook server

WhatsApp delivers messages via webhooks, so PtrClaw needs a local HTTP server:

```sh
export WHATSAPP_WEBHOOK_LISTEN="127.0.0.1:8080"
export WHATSAPP_WEBHOOK_SECRET="$(openssl rand -hex 32)"
./builddir/ptrclaw --channel whatsapp
```

PtrClaw's built-in HTTP server binds to localhost and must sit behind a reverse proxy (nginx, Caddy) that handles TLS and rate-limiting. See [reverse-proxy.md](reverse-proxy.md) for full proxy configuration.

## Environment variables

| Variable | Description |
| --- | --- |
| `WHATSAPP_ACCESS_TOKEN` | WhatsApp Business API access token |
| `WHATSAPP_PHONE_ID` | WhatsApp Business phone number ID |
| `WHATSAPP_VERIFY_TOKEN` | WhatsApp webhook verification token |
| `WHATSAPP_WEBHOOK_LISTEN` | Bind address for built-in webhook server (e.g. `127.0.0.1:8080`) |
| `WHATSAPP_WEBHOOK_SECRET` | Shared secret for proxy-to-local trust (`X-Webhook-Secret` header) |

## Notes

- Temporary tokens expire; use a long-lived token for production.
- Point Meta's webhook callback URL at your reverse proxy's public HTTPS endpoint.
- The `app_secret` config field is optional and reserved for future payload signature verification.
