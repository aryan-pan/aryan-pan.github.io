project('realmd', 'c', version: '0.1.0', default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=release'])

add_project_arguments('-DWLR_USE_UNSTABLE', '-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')

wayland_protos = dependency('wayland-protocols')
wayland_scanner = find_program('wayland-scanner')
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
protocol_headers = []
foreach p : [
  ['stable/xdg-shell/xdg-shell.xml', 'xdg-shell-protocol.h'],
  ['staging/cursor-shape/cursor-shape-v1.xml', 'cursor-shape-v1-protocol.h'],
  ['unstable/tablet/tablet-unstable-v2.xml', 'tablet-unstable-v2-protocol.h'],
]
  protocol_headers += custom_target(p[1],
    input: wl_protocol_dir / p[0], output: p[1],
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
endforeach

deps = [
  dependency('wlroots', version: ['>=0.17', '<0.18']),
  dependency('wayland-server'),
  dependency('xkbcommon'),
  dependency('cairo'),
  dependency('libdrm'),
  meson.get_compiler('c').find_library('m'),
]

executable('realmd',
  ['src/main.c', 'src/game.c', 'src/character.c', 'src/procs.c', 'src/net.c', protocol_headers],
  dependencies: deps, install: true)
