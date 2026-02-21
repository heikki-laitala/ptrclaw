project('sqlite3', 'c', version: '3.48.0')

cc = meson.get_compiler('c')
thread_dep = dependency('threads', required: false)

sqlite3_lib = static_library('sqlite3',
  'sqlite3.c',
  c_args: ['-DSQLITE_ENABLE_FTS5', '-DSQLITE_THREADSAFE=1'],
  dependencies: [thread_dep])

sqlite3_dep = declare_dependency(
  link_with: sqlite3_lib,
  include_directories: include_directories('.'))
