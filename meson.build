project('ptrclaw', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17', 'buildtype=release', 'warning_level=2', 'b_lto=true'])

# Linux uses a lightweight POSIX-socket + OpenSSL HTTP backend to avoid
# the libcurl dependency and reduce static binary size.
# macOS keeps libcurl (available via the system SDK / Homebrew).
host_system = host_machine.system()
if host_system == 'linux'
  # Try pkg-config / cmake first; fall back to direct library search
  # (e.g. environments where libssl-dev is present but pkg-config is not).
  http_dep = dependency('openssl', required: false)
  if not http_dep.found()
    cc = meson.get_compiler('cpp')
    ssl_lib    = cc.find_library('ssl',    required: true)
    crypto_lib = cc.find_library('crypto', required: true)
    http_dep   = declare_dependency(dependencies: [ssl_lib, crypto_lib])
  endif
  http_impl_source = files('src/http_socket.cpp')
else
  http_dep = dependency('libcurl')
  http_impl_source = files('src/http.cpp')
endif

inc = include_directories('include', 'src')

# ── Feature flag dependency resolution ─────────────────────────
# openrouter and compatible inherit from OpenAIProvider
opt_openai     = get_option('with_openai') or get_option('with_openrouter') or get_option('with_compatible')
opt_anthropic  = get_option('with_anthropic')
opt_openrouter = get_option('with_openrouter')
opt_compatible = get_option('with_compatible')
opt_ollama     = get_option('with_ollama')
opt_telegram   = get_option('with_telegram')
opt_whatsapp   = get_option('with_whatsapp')
opt_tools      = get_option('with_tools')

# Memory features
opt_memory        = get_option('with_memory')
opt_sqlite_memory = get_option('with_sqlite_memory')
opt_memory_tools  = get_option('with_memory_tools') and opt_memory

# sse.cpp is shared by anthropic and openai streaming
opt_sse = opt_anthropic or opt_openai

# ── Optional dependencies ─────────────────────────────────────
lib_deps = [http_dep]

if opt_sqlite_memory
  sqlite3_dep = dependency('sqlite3', required: false)
  if not sqlite3_dep.found()
    sqlite3_proj = subproject('sqlite3')
    sqlite3_dep = sqlite3_proj.get_variable('sqlite3_dep')
  endif
  lib_deps += sqlite3_dep
  add_project_arguments('-DPTRCLAW_HAS_SQLITE_MEMORY', language: 'cpp')
endif

# ── Core sources (always compiled) ─────────────────────────────
core_sources = files(
  'src/agent.cpp', 'src/channel.cpp', 'src/config.cpp', 'src/dispatcher.cpp',
  'src/event_bus.cpp', 'src/plugin.cpp', 'src/prompt.cpp',
  'src/provider.cpp', 'src/session.cpp', 'src/stream_relay.cpp',
  'src/tool.cpp', 'src/util.cpp',
  'src/providers/reliable.cpp',
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
  optional_sources += files('src/providers/openai.cpp')
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
  optional_sources += files('src/channels/whatsapp.cpp')
endif
if opt_tools
  optional_sources += files(
    'src/tools/file_edit.cpp', 'src/tools/file_read.cpp',
    'src/tools/file_write.cpp', 'src/tools/shell.cpp',
  )
endif

# Memory system
if opt_memory
  optional_sources += files(
    'src/memory.cpp',
    'src/memory/json_memory.cpp',
    'src/memory/none_memory.cpp',
  )
endif
if opt_memory and get_option('with_memory')
  optional_sources += files('src/memory/response_cache.cpp')
endif
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

# ── Unit tests ─────────────────────────────────────────────────
catch2_proj = subproject('catch2')
catch2_dep = catch2_proj.get_variable('catch2_with_main_dep')

core_test_sources = files(
  'tests/test_util.cpp',
  'tests/test_dispatcher.cpp',
  'tests/test_config.cpp',
  'tests/test_agent.cpp',
  'tests/test_reliable.cpp',
  'tests/test_prompt.cpp',
  'tests/test_provider.cpp',
  'tests/test_session.cpp',
  'tests/test_channel.cpp',
  'tests/test_event_bus.cpp',
  'tests/test_plugin.cpp',
)

optional_test_sources = []

if opt_sse
  optional_test_sources += files('tests/test_sse.cpp')
endif
if opt_tools
  optional_test_sources += files('tests/test_tools.cpp')
endif
if opt_telegram
  optional_test_sources += files('tests/test_telegram.cpp')
endif
if opt_whatsapp
  optional_test_sources += files('tests/test_whatsapp.cpp')
endif
# test_providers.cpp tests all five providers in a single file
if opt_anthropic and opt_openai and opt_ollama and opt_openrouter and opt_compatible
  optional_test_sources += files('tests/test_providers.cpp')
endif
if opt_memory
  optional_test_sources += files(
    'tests/test_memory.cpp',
    'tests/test_json_memory.cpp',
    'tests/test_response_cache.cpp',
    'tests/test_hatch.cpp',
  )
endif
if opt_sqlite_memory
  optional_test_sources += files('tests/test_sqlite_memory.cpp')
endif
if opt_memory_tools
  optional_test_sources += files('tests/test_memory_tools.cpp')
endif
test_sources = core_test_sources + optional_test_sources

test_exe = executable('ptrclaw_tests', test_sources,
  dependencies: [ptrclaw_dep, catch2_dep])

test('unit_tests', test_exe)

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

memory_features = []
if opt_memory
  memory_features += 'json, none'
endif
if opt_sqlite_memory
  memory_features += 'sqlite'
endif
summary({
  'Providers': providers.length() > 0 ? ', '.join(providers) : 'none',
  'Channels':  channels.length() > 0  ? ', '.join(channels)  : 'none',
  'Tools':     opt_tools ? 'file_read, file_write, file_edit, shell' : 'none',
  'Memory':    memory_features.length() > 0 ? ', '.join(memory_features) : 'none',
  'Memory tools': opt_memory_tools ? 'memory_store, memory_recall, memory_forget, memory_link' : 'none',
}, section: 'Features')
