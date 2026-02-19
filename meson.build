project('ptrclaw', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17', 'buildtype=release', 'warning_level=2'])

curl_dep = dependency('libcurl')
inc = include_directories('include', 'src')

sources = files(
  'src/agent.cpp', 'src/config.cpp', 'src/dispatcher.cpp',
  'src/http.cpp', 'src/main.cpp', 'src/prompt.cpp',
  'src/provider.cpp', 'src/session.cpp', 'src/tool.cpp',
  'src/util.cpp',
  'src/providers/anthropic.cpp', 'src/providers/compatible.cpp',
  'src/providers/ollama.cpp', 'src/providers/openai.cpp',
  'src/providers/openrouter.cpp', 'src/providers/reliable.cpp',
  'src/providers/sse.cpp',
  'src/tools/file_edit.cpp', 'src/tools/file_read.cpp',
  'src/tools/file_write.cpp', 'src/tools/shell.cpp',
)

executable('ptrclaw', sources,
  include_directories: inc,
  dependencies: [curl_dep],
  install: true)
