# OpenAI OAuth (subscription models)

Models your OpenAI subscription (Plus, Pro, or Team) covers can be reached via OAuth instead of an API key — the codex family (e.g. `gpt-5-codex-mini`) and the wider gpt-5 family (e.g. `gpt-5`, `gpt-5.1`, `gpt-5-pro`). Some models are only available one way: newer codex models (e.g. `gpt-5.3-codex`) only through OAuth, and older models (e.g. `gpt-4o`, `o3`) only through an API key.

## How it works

PtrClaw runs a PKCE OAuth flow against `auth.openai.com`. You authorize in your browser, and PtrClaw exchanges the callback code for access and refresh tokens. Tokens are saved to `~/.ptrclaw/config.json` and refreshed automatically when they expire.

> **Build note.** The browser flow is behind the `with_openai_oauth` feature flag
> (default `true`), so everything below works in a standard build. A build
> configured with `-Dwith_openai_oauth=false` leaves out the flow and its default
> client id — intended for unattended deployments that cannot open a browser.
>
> Such a build is not "OAuth-free": if `use_oauth` and the tokens are present in
> `~/.ptrclaw/config.json` — placed there by provisioning, or copied from a machine
> that did run the flow — the provider uses them, refreshes them on expiry, and
> writes the rotated refresh token back exactly as described here. What it cannot
> do is *obtain* tokens interactively, and it has no built-in `oauth_client_id`, so
> set that explicitly alongside the tokens.

### Interactive setup

The easiest way to set up OAuth is via the `/auth openai` command and choosing "OAuth login":

```text
ptrclaw> /auth openai
Authentication method:
  1. API key
  2. OAuth login (ChatGPT subscription)
> 2

Open this URL to authorize:
https://auth.openai.com/oauth/authorize?...

Paste the callback URL or code: http://localhost:1455/auth/callback?code=...
OAuth connected. Model switched to gpt-5-codex-mini.
```

After approving in your browser, it redirects to `localhost:1455/auth/callback?code=...`. The page won't load (there's no local server) — copy the full URL from your browser's address bar and paste it back.

### Two-step flow (channels)

In Telegram and other channels where inline prompting isn't available, use the two-step flow:

1. Send `/auth openai start` — PtrClaw prints an authorization URL
2. Open the URL, sign in, copy the callback URL
3. Send `/auth openai finish <callback_url>` (or paste just the code)

You can also paste the callback URL directly without the `/auth openai finish` prefix while an auth flow is pending.

## Auth mode auto-detection

PtrClaw selects the auth mode from the model, because that is what decides which
credential can work: subscription tokens are only accepted by OpenAI's ChatGPT backend,
and `api.openai.com` does not accept them at all.

- **Models the subscription serves** (name contains `codex` or `gpt-5`): OAuth when tokens
  are available, falls back to the API key
- **Every other model**: always the API key
- **API format**: over OAuth, always the Responses API (the ChatGPT backend speaks nothing
  else); over an API key, codex models use the Responses API and the rest use Chat Completions

You can have both an API key and OAuth tokens configured and switch freely — `/model` rebuilds
the provider whenever the switch changes which credential applies.

### Choosing the models yourself

The set of models a subscription covers is not discoverable from PtrClaw and changes over
time, so `oauth_models` overrides the built-in list. It replaces it rather than adding to it,
so it can widen or narrow the set; entries match as substrings, and `"*"` matches any model:

```json
{
  "providers": {
    "openai": {
      "oauth_models": ["codex", "gpt-5", "gpt-4o"]
    }
  }
}
```

Set `["*"]` to send every OpenAI model over OAuth. A model the backend does not serve fails
with that backend's error rather than silently falling back to the API key.

### Connecting while on a model already covered

`/auth openai` keeps the current model when the subscription can serve it, so connecting
while on `gpt-5` leaves you on `gpt-5`. Otherwise it switches to `gpt-5-codex-mini`, which is
guaranteed to work with the credential you just added.

## Environment variables

These environment variables override the config file for OAuth:

| Variable | Description |
| --- | --- |
| `OPENAI_USE_OAUTH` | Use OAuth token path (`true`/`1`) |
| `OPENAI_OAUTH_MODELS` | Comma-separated `oauth_models` list (e.g. `codex,gpt-5`) |
| `OPENAI_OAUTH_ACCESS_TOKEN` | OAuth access token |
| `OPENAI_OAUTH_REFRESH_TOKEN` | OAuth refresh token |
| `OPENAI_OAUTH_EXPIRES_AT` | Access token expiry (epoch seconds) |
| `OPENAI_OAUTH_CLIENT_ID` | OAuth client id (default `app_EMoamEEZ73f0CkXaXp7hrann`) |
| `OPENAI_OAUTH_TOKEN_URL` | OAuth token endpoint (default `https://auth.openai.com/oauth/token`) |

## Config format

After OAuth setup, `~/.ptrclaw/config.json` looks like:

```json
{
  "provider": "openai",
  "model": "gpt-5-codex-mini",
  "providers": {
    "openai": {
      "api_key": "sk-...",
      "oauth_access_token": "<managed automatically>",
      "oauth_refresh_token": "<managed automatically>",
      "oauth_expires_at": 1767225600,
      "oauth_client_id": "app_EMoamEEZ73f0CkXaXp7hrann"
    }
  }
}
```

You don't need to edit the OAuth fields manually — the `/auth` flow and automatic token refresh handle them. The `api_key` field is independent and used for the models OAuth cannot serve. The `use_oauth` field is managed automatically: PtrClaw sets it to whatever the active model resolved to.

## Token refresh

When the access token expires, PtrClaw automatically refreshes it using the `oauth_refresh_token` and saves the updated tokens to config. This happens transparently during API calls.
