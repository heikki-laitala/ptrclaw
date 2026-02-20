project('ptrclaw', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17', 'buildtype=release', 'warning_level=2', 'b_lto=true'])

curl_dep = dependency('libcurl')
inc = include_directories('include', 'src')

lib_sources = files(
  'src/agent.cpp', 'src/channel.cpp', 'src/config.cpp', 'src/dispatcher.cpp',
  'src/http.cpp', 'src/prompt.cpp',
  'src/provider.cpp', 'src/session.cpp', 'src/tool.cpp',
  'src/util.cpp',
  'src/channels/telegram.cpp', 'src/channels/whatsapp.cpp',
  'src/providers/anthropic.cpp', 'src/providers/compatible.cpp',
  'src/providers/ollama.cpp', 'src/providers/openai.cpp',
  'src/providers/openrouter.cpp', 'src/providers/reliable.cpp',
  'src/providers/sse.cpp',
  'src/tools/file_edit.cpp', 'src/tools/file_read.cpp',
  'src/tools/file_write.cpp', 'src/tools/shell.cpp',
)

ptrclaw_lib = static_library('ptrclaw_lib', lib_sources,
  include_directories: inc,
  dependencies: [curl_dep])

ptrclaw_dep = declare_dependency(
  link_with: ptrclaw_lib,
  include_directories: inc)

executable('ptrclaw', 'src/main.cpp',
  dependencies: [ptrclaw_dep],
  install: true)

# Unit tests
catch2_proj = subproject('catch2')
catch2_dep = catch2_proj.get_variable('catch2_with_main_dep')

test_sources = files(
  'tests/test_util.cpp',
  'tests/test_dispatcher.cpp',
  'tests/test_sse.cpp',
  'tests/test_config.cpp',
  'tests/test_tools.cpp',
  'tests/test_agent.cpp',
  'tests/test_reliable.cpp',
  'tests/test_prompt.cpp',
  'tests/test_provider.cpp',
  'tests/test_providers.cpp',
  'tests/test_session.cpp',
  'tests/test_channel.cpp',
  'tests/test_telegram.cpp',
  'tests/test_whatsapp.cpp',
)

test_exe = executable('ptrclaw_tests', test_sources,
  dependencies: [ptrclaw_dep, catch2_dep])

test('unit_tests', test_exe)
