project('ptrclaw', 'cpp',
  version: '0.0.10',
  default_options: ['cpp_std=c++17', 'buildtype=release', 'warning_level=2', 'b_lto=true'])

# ── HTTP / TLS backend selection ────────────────────────────────
#
# Default builds:
#   Linux  → POSIX sockets + OpenSSL (or mbedTLS with -Dwith_mbedtls=true)
#   macOS  → libcurl (system SDK)
#
# Embed/SDK builds (-Dwith_embed=true):
#   All platforms → POSIX sockets + mbedTLS (no libcurl dependency,
#   portable across macOS, iOS, Android, Linux).
host_system = host_machine.system()
opt_embed_early = get_option('with_embed')
opt_mbedtls = (get_option('with_mbedtls') and host_system == 'linux') or opt_embed_early

if opt_embed_early and host_system != 'linux'
  # Embed on non-Linux: use socket backend with mbedTLS
  mbedtls_dep = dependency('mbedtls', required: false)
  mbedx509_dep = dependency('mbedx509', required: false)
  mbedcrypto_dep = dependency('mbedcrypto', required: false)
  if mbedtls_dep.found() and mbedx509_dep.found() and mbedcrypto_dep.found()
    http_dep = declare_dependency(dependencies: [mbedtls_dep, mbedx509_dep, mbedcrypto_dep])
  else
    cc = meson.get_compiler('cpp')
    mbedtls_lib  = cc.find_library('mbedtls',    required: true)
    mbedx509_lib = cc.find_library('mbedx509',   required: true)
    mbedcrypto_lib = cc.find_library('mbedcrypto', required: true)
    http_dep = declare_dependency(dependencies: [mbedtls_lib, mbedx509_lib, mbedcrypto_lib])
  endif
  add_project_arguments('-DPTRCLAW_USE_MBEDTLS', language: 'cpp')
  add_project_arguments('-DPTRCLAW_USE_SOCKET_HTTP', language: 'cpp')
  http_impl_source = files('src/http_socket.cpp')
elif host_system == 'linux'
  if opt_mbedtls
    cc = meson.get_compiler('cpp')
    mbedtls_lib  = cc.find_library('mbedtls',    required: true)
    mbedx509_lib = cc.find_library('mbedx509',   required: true)
    mbedcrypto_lib = cc.find_library('mbedcrypto', required: true)
    http_dep = declare_dependency(dependencies: [mbedtls_lib, mbedx509_lib, mbedcrypto_lib])
    add_project_arguments('-DPTRCLAW_USE_MBEDTLS', language: 'cpp')
  else
    # Try pkg-config / cmake first; fall back to direct library search
    # (e.g. environments where libssl-dev is present but pkg-config is not).
    http_dep = dependency('openssl', required: false)
    if not http_dep.found()
      cc = meson.get_compiler('cpp')
      ssl_lib    = cc.find_library('ssl',    required: true)
      crypto_lib = cc.find_library('crypto', required: true)
      http_dep   = declare_dependency(dependencies: [ssl_lib, crypto_lib])
    endif
  endif
  http_impl_source = files('src/http_socket.cpp')
else
  http_dep = dependency('libcurl')
  http_impl_source = files('src/http.cpp')
endif

# SHA-256 for PKCE S256 code challenge.
# macOS: use CommonCrypto (system framework, no extra deps, no Keychain prompts).
# Linux with mbedTLS: uses mbedtls/sha256.h (PTRCLAW_USE_MBEDTLS already defined above).
# Linux with OpenSSL: OpenSSL is already linked via http_dep.
# Embed builds use mbedTLS SHA-256 on all platforms.
if host_system != 'linux' and not opt_embed_early
  add_project_arguments('-DPTRCLAW_USE_COMMONCRYPTO', language: 'cpp')
endif

inc = include_directories('include', 'src')

# ── Feature flag dependency resolution ─────────────────────────
# openrouter and compatible inherit from OpenAIProvider
opt_openai     = get_option('with_openai') or get_option('with_openrouter') or get_option('with_compatible')
if opt_openai
  add_project_arguments('-DPTRCLAW_HAS_OPENAI', language: 'cpp')
endif
opt_anthropic  = get_option('with_anthropic')
opt_openrouter = get_option('with_openrouter')
opt_compatible = get_option('with_compatible')
opt_ollama     = get_option('with_ollama')
opt_telegram   = get_option('with_telegram')
opt_whatsapp   = get_option('with_whatsapp')
opt_tools      = get_option('with_tools')

# Memory features (core memory is always included — Agent requires it)
opt_sqlite_memory = get_option('with_sqlite_memory')
opt_memory_tools  = get_option('with_memory_tools')

opt_embeddings = get_option('with_embeddings')

opt_pipe  = get_option('with_pipe')
opt_embed = get_option('with_embed')

if opt_pipe
  add_project_arguments('-DPTRCLAW_HAS_PIPE', language: 'cpp')
endif

if opt_embeddings
  add_project_arguments('-DPTRCLAW_HAS_EMBEDDINGS', language: 'cpp')
endif

# Expose project version to C/C++ sources (used by the embed API).
add_project_arguments(
  '-DPTRCLAW_VERSION_STRING="' + meson.project_version() + '"',
  language: 'cpp')

# sse.cpp is shared by anthropic and openai streaming
opt_sse = opt_anthropic or opt_openai

# ── Optional dependencies ─────────────────────────────────────
lib_deps = [http_dep]

if opt_sqlite_memory
  sqlite3_dep = dependency('sqlite3', required: false)
  if sqlite3_dep.found()
    lib_deps += sqlite3_dep
    add_project_arguments('-DPTRCLAW_HAS_SQLITE_MEMORY', language: 'cpp')
  else
    warning('sqlite3 not found — SQLite memory backend disabled')
    opt_sqlite_memory = false
  endif
endif

# ── Core sources (always compiled) ─────────────────────────────
core_sources = files(
  'src/agent.cpp', 'src/channel.cpp', 'src/commands.cpp', 'src/config.cpp',
  'src/dispatcher.cpp', 'src/event_bus.cpp', 'src/oauth.cpp',
  'src/onboard.cpp', 'src/output_filter.cpp', 'src/plugin.cpp',
  'src/prompt.cpp', 'src/provider.cpp', 'src/repl.cpp',
  'src/session.cpp', 'src/skill.cpp', 'src/stream_relay.cpp', 'src/tool.cpp',
  'src/tool_manager.cpp', 'src/util.cpp',
) + http_impl_source

# ── Optional sources (gated by feature flags) ──────────────────
optional_sources = []

if opt_sse
  optional_sources += files('src/providers/sse.cpp')
endif
if opt_anthropic
  optional_sources += files('src/providers/anthropic.cpp')
endif
if opt_openai
  optional_sources += files('src/providers/openai.cpp', 'src/providers/oauth_openai.cpp')
endif
if opt_openrouter
  optional_sources += files('src/providers/openrouter.cpp')
endif
if opt_compatible
  optional_sources += files('src/providers/compatible.cpp')
endif
if opt_ollama
  optional_sources += files('src/providers/ollama.cpp')
endif
if opt_telegram
  optional_sources += files('src/channels/telegram.cpp')
endif
if opt_whatsapp
  optional_sources += files(
    'src/channels/whatsapp.cpp',
    'src/channels/webhook_server.cpp',
  )
endif
if opt_tools
  optional_sources += files(
    'src/tools/file_edit.cpp', 'src/tools/file_read.cpp',
    'src/tools/file_write.cpp', 'src/tools/shell.cpp',
    'src/tools/skill_activate.cpp', 'src/tools/cron.cpp',
  )
endif

# Memory system (always included — Agent requires it)
optional_sources += files(
  'src/memory.cpp',
  'src/memory/json_memory.cpp',
  'src/memory/none_memory.cpp',
  'src/memory/response_cache.cpp',
)
if opt_sqlite_memory
  optional_sources += files('src/memory/sqlite_memory.cpp')
endif
if opt_memory_tools
  optional_sources += files(
    'src/tools/memory_store.cpp',
    'src/tools/memory_recall.cpp',
    'src/tools/memory_forget.cpp',
    'src/tools/memory_link.cpp',
  )
endif
if opt_embeddings
  optional_sources += files(
    'src/embedder.cpp',
    'src/embedders/http_embedder.cpp',
  )
endif
if opt_embed
  optional_sources += files(
    'src/channels/embed.cpp',
    'src/embed/embed_api.cpp',
  )
endif

lib_sources = core_sources + optional_sources

ptrclaw_lib = static_library('ptrclaw_lib', lib_sources,
  include_directories: inc,
  dependencies: lib_deps)

ptrclaw_dep = declare_dependency(
  link_whole: ptrclaw_lib,
  include_directories: inc)

executable('ptrclaw', 'src/main.cpp',
  dependencies: [ptrclaw_dep],
  install: true)

# ── Embeddable shared library (opt-in) ─────────────────────────
if opt_embed
  shared_library('ptrclaw_shared', lib_sources,
    include_directories: inc,
    dependencies: lib_deps,
    install: true)

  install_headers('include/ptrclaw/ptrclaw.h', subdir: 'ptrclaw')
endif

# ── Unit tests ─────────────────────────────────────────────────
catch2_proj = subproject('catch2')
catch2_dep = catch2_proj.get_variable('catch2_with_main_dep')

core_test_sources = files(
  'tests/test_util.cpp',
  'tests/test_dispatcher.cpp',
  'tests/test_config.cpp',
  'tests/test_agent.cpp',
  'tests/test_prompt.cpp',
  'tests/test_provider.cpp',
  'tests/test_session.cpp',
  'tests/test_channel.cpp',
  'tests/test_event_bus.cpp',
  'tests/test_plugin.cpp',
  'tests/test_oauth.cpp',
  'tests/test_onboard.cpp',
  'tests/test_output_filter.cpp',
  'tests/test_skill.cpp',
  'tests/test_commands.cpp',
  'tests/test_tool_manager.cpp',
)

optional_test_sources = []

if opt_sse
  optional_test_sources += files('tests/test_sse.cpp')
endif
if opt_tools
  optional_test_sources += files('tests/test_tools.cpp', 'tests/test_cron.cpp',
                                 'tests/test_shell_filter.cpp',
                                 'tests/test_skill_activate.cpp')
endif
if opt_telegram
  optional_test_sources += files('tests/test_telegram.cpp')
endif
if opt_telegram
  optional_test_sources += files('tests/test_integration_telegram_memory.cpp')
endif
if opt_whatsapp
  optional_test_sources += files(
    'tests/test_whatsapp.cpp',
    'tests/test_whatsapp_webhook.cpp',
  )
endif
# test_providers.cpp tests all five providers in a single file
if opt_anthropic and opt_openai and opt_ollama and opt_openrouter and opt_compatible
  optional_test_sources += files('tests/test_providers.cpp')
endif
optional_test_sources += files(
  'tests/test_memory.cpp',
  'tests/test_json_memory.cpp',
  'tests/test_response_cache.cpp',
  'tests/test_hatch.cpp',
)
if opt_sqlite_memory
  optional_test_sources += files('tests/test_sqlite_memory.cpp')
endif
if opt_memory_tools
  optional_test_sources += files('tests/test_memory_tools.cpp')
endif
if opt_embeddings
  optional_test_sources += files(
    'tests/test_embedder.cpp',
    'tests/test_hybrid_search.cpp',
  )
endif
if opt_embed
  optional_test_sources += files('tests/test_embed.cpp')
endif
test_sources = core_test_sources + optional_test_sources

test_exe = executable('ptrclaw_tests', test_sources,
  dependencies: [ptrclaw_dep, catch2_dep])

test('unit_tests', test_exe, args: ['~[cron]'])

# ── Build summary ──────────────────────────────────────────────
providers = []
if opt_anthropic
  providers += 'anthropic'
endif
if opt_openai
  providers += 'openai'
endif
if opt_openrouter
  providers += 'openrouter'
endif
if opt_compatible
  providers += 'compatible'
endif
if opt_ollama
  providers += 'ollama'
endif

channels = []
if opt_telegram
  channels += 'telegram'
endif
if opt_whatsapp
  channels += 'whatsapp'
endif
if opt_embed
  channels += 'embed'
endif

memory_features = ['json, none']
if opt_sqlite_memory
  memory_features += 'sqlite'
endif
if opt_embeddings
  memory_features += 'embeddings'
endif
if opt_mbedtls
  tls_backend = 'mbedtls'
elif host_system == 'linux'
  tls_backend = 'openssl'
else
  tls_backend = 'libcurl (system)'
endif

if opt_tools
  tools_summary = 'file_read, file_write, file_edit, shell, cron'
elif opt_embed
  tools_summary = 'host-bridged (via ptrclaw_register_tool)'
else
  tools_summary = 'none'
endif

summary({
  'Providers': providers.length() > 0 ? ', '.join(providers) : 'none',
  'Channels':  channels.length() > 0  ? ', '.join(channels)  : 'none',
  'Tools':     tools_summary,
  'Memory':    memory_features.length() > 0 ? ', '.join(memory_features) : 'none',
  'Memory tools': opt_memory_tools ? 'memory_store, memory_recall, memory_forget, memory_link' : 'none',
  'Embed SDK': opt_embed ? 'libptrclaw_shared + ptrclaw.h' : 'off',
  'TLS backend': tls_backend,
}, section: 'Features')
