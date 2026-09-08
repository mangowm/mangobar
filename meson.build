project('mangobar', 'c',
  version : '0.2.1',
  default_options: ['warning_level=3', 'buildtype=release']
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments('-Wno-unused-parameter'),
                      language: 'c')

sdbus_dep = dependency('libsystemd', 'libelogind', 'basu')
add_project_arguments('-D' + 'SDBUS_' + sdbus_dep.name().to_upper(), language: 'c')

deps = [
  dependency('threads'),
  dependency('wayland-client'),
  dependency('wayland-cursor'),
  dependency('fcft'),
  dependency('pixman-1'),
  dependency('cairo'),
  dependency('pango'),
  dependency('pangocairo'),
  dependency('libpulse'),
  sdbus_dep,
  dependency('libudev'),
  dependency('gdk-pixbuf-2.0'),
  cc.find_library('rt'),
  cc.find_library('cjson'),
]

scanner = find_program('wayland-scanner')
proto_dep = dependency('wayland-protocols')
proto_dir = proto_dep.get_variable(pkgconfig: 'pkgdatadir')

gen_header = generator(scanner,
  output: '@BASENAME@-protocol.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@']
)
gen_code = generator(scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@']
)

xdg_shell_xml = proto_dir / 'stable/xdg-shell/xdg-shell.xml'
xdg_output_xml = proto_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml'
wlr_layer_xml = 'protocols/wlr-layer-shell-unstable-v1.xml'

xdg_shell_h = gen_header.process(xdg_shell_xml)
xdg_shell_c = gen_code.process(xdg_shell_xml)

xdg_output_h = gen_header.process(xdg_output_xml)
xdg_output_c = gen_code.process(xdg_output_xml)

wlr_layer_h = gen_header.process(wlr_layer_xml)
wlr_layer_c = gen_code.process(wlr_layer_xml)

defaults_h = custom_target('defaults-h',
  output: 'defaults.h',
  input: ['config.jsonc', 'style.css'],
  command: [
    find_program('python3'),
    files('scripts/gen_defaults.py'),
    '@INPUT0@',
    '@INPUT1@',
    '@OUTPUT@',
  ],
)

executable('mangobar',
  sources: [
    defaults_h,
    xdg_shell_h, xdg_shell_c,
    xdg_output_h, xdg_output_c,
    wlr_layer_h, wlr_layer_c,
    'mangobar.c',
    'rtconfig.c',
    'style.c',
    'tray.c',
    'menu.c'
  ],
  dependencies: deps,
  install: true
)
