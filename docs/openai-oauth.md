# OpenAI OAuth (codex models)

OpenAI's codex models (e.g. `gpt-5-codex-mini`) can be accessed via OAuth using your OpenAI subscription (Plus, Pro, or Team) or via a regular API key. Some newer codex models (e.g. `gpt-5.3-codex`) are only available through OAuth.

## How it works

PtrClaw runs a PKCE OAuth flow against `auth.openai.com`. You authorize in your browser, and PtrClaw exchanges the callback code for access and refresh tokens. Tokens are saved to `~/.ptrclaw/config.json` and refreshed automatically when they expire.

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

PtrClaw automatically selects the right auth mode based on the model:

- **Codex models** (`*codex*`): OAuth when tokens are available, falls back to API key
- **Non-codex models**: Always use the API key
- **API format**: Codex models use the Responses API; other models use Chat Completions

You can have both API key and OAuth tokens configured and switch freely between codex and non-codex models.

## Environment variables

These environment variables override the config file for OAuth:

| Variable | Description |
| --- | --- |
| `OPENAI_USE_OAUTH` | Use OAuth token path (`true`/`1`) |
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

You don't need to edit the OAuth fields manually — the `/auth` flow and automatic token refresh handle them. The `api_key` field is independent and used for non-codex OpenAI models. The `use_oauth` field is managed automatically.

## Token refresh

When the access token expires, PtrClaw automatically refreshes it using the `oauth_refresh_token` and saves the updated tokens to config. This happens transparently during API calls.
