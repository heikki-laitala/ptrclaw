# OpenAI OAuth (subscription models)

Models your OpenAI subscription (Plus, Pro, or Team) covers can be reached via OAuth instead of an API key. The two endpoints do not serve the same catalog, so which credential works is a property of the model, not a preference.

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
OAuth connected. Model switched to gpt-5.6-sol.
```

After approving in your browser, it redirects to `localhost:1455/auth/callback?code=...`. The page won't load (there's no local server) — copy the full URL from your browser's address bar and paste it back.

### Two-step flow (channels)

In Telegram and other channels where inline prompting isn't available, use the two-step flow:

1. Send `/auth openai start` — PtrClaw prints an authorization URL
2. Open the URL, sign in, copy the callback URL
3. Send `/auth openai finish <callback_url>` (or paste just the code)

You can also paste the callback URL directly without the `/auth openai finish` prefix while an auth flow is pending.

## Model routes

Each model belongs to a route, matched on its exact id (case-insensitively):

| Route | Models | Credential |
| --- | --- | --- |
| Both | `gpt-5.6-sol`, `gpt-5.6-terra`, `gpt-5.6-luna`, `gpt-5.5`, `gpt-5.5-pro`, `gpt-5.4`, `gpt-5.4-pro`, `gpt-5.4-mini` | OAuth when tokens are present, otherwise the API key |
| API key only | `chat-latest`, `gpt-5.6` | API key, even when tokens are present |
| Subscription only | `gpt-5.3-codex-spark` | OAuth; refused outright without tokens |
| Unknown | everything else (`gpt-4o`, `o3`, …) | API key |

Two details worth knowing:

- Plain `gpt-5.6` is API-key only while `gpt-5.6-sol` is not — which is why the routes are
  exact ids and not a name pattern.
- An id containing `codex` that is not listed above (`gpt-5-codex-mini`, `gpt-5.3-codex`)
  is treated as *both*, so a model that worked before these lists existed keeps working.

`gpt-5.4-codex` is accepted as an alias of `gpt-5.4`.

A model whose only route rejects the credential you have is refused when you select it,
rather than failing later with an opaque error from OpenAI — and the refusal names what
would fix it, `oauth_models` included when that is what excluded the model.

### API format

Over OAuth it is always the Responses API — the ChatGPT backend speaks nothing else. Over an
API key the route table decides: every model it lists uses the Responses API, and anything it
does not recognise stays on Chat Completions.

You can have both an API key and OAuth tokens configured and switch freely; `/model` rebuilds
the provider whenever the switch changes which credential applies.

### Choosing the models yourself

The routes above need editing when OpenAI ships models, so `oauth_models` overrides them. It
replaces the built-in routes rather than adding to them, so it can widen or narrow the set;
entries match as case-insensitive substrings, and `"*"` matches any model:

```json
{
  "providers": {
    "openai": {
      "oauth_models": ["codex", "gpt-5.7", "gpt-4o"]
    }
  }
}
```

Set `["*"]` to send every OpenAI model over OAuth. A model the backend does not serve then
fails with that backend's error rather than being refused up front.

### Request identity

Requests to the subscription backend carry, besides the bearer token:

| Header | Value |
| --- | --- |
| `chatgpt-account-id` | the `chatgpt_account_id` claim of the current access token, omitted when absent |
| `originator` | `pi` — paired with the built-in client id, not a free label |
| `User-Agent` | `pi (<os> <release>; <arch>)` |

The account id is read from the live token rather than stored, so it follows the token across
a refresh. A subscription covering more than one workspace needs it to route the request.

### Endpoints

Requests over OAuth go to `https://chatgpt.com/backend-api/codex/responses`. Setting
`providers.openai.base_url` to `https://chatgpt.com/backend-api/codex` selects that endpoint
explicitly; any other `base_url` is treated as your own endpoint and is used verbatim, so a
proxy is never silently replaced by OpenAI's.

### Connecting while on a model already covered

`/auth openai` keeps the current model when the subscription can serve it, so connecting
while on `gpt-5.5` leaves you on `gpt-5.5`. Otherwise it switches to `gpt-5.6-sol`, which
works on the credential you just added.

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
  "model": "gpt-5.6-sol",
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

Both the refresh and the initial code exchange are constrained: the token endpoint must be
`https` (a plaintext one is refused before anything is sent), the request uses a 30-second
timeout rather than the chat timeout, and a response body over 1 MiB is rejected unparsed.
`oauth_token_url` stays configurable for a gateway that fronts `auth.openai.com`, but it
cannot be a plaintext URL.
