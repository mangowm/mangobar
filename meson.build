project('mangobar', 'c',
  default_options: ['warning_level=3', 'buildtype=release']
)

cc = meson.get_compiler('c')
deps = [
  dependency('wayland-client'),
  dependency('fcft'),
  dependency('pixman-1'),
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

executable('mangobar',
  sources: [
    xdg_shell_h, xdg_shell_c,
    xdg_output_h, xdg_output_c,
    wlr_layer_h, wlr_layer_c,
    'mangobar.c'
  ],
  dependencies: deps,
  install: true
)