wayland_protos = dependency('wayland-protocols',
	version: '>=1.41',
	fallback: 'wayland-protocols',
	default_options: ['tests=false'],
)
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner',
	kwargs: wayland_kwargs,
	native: true,
)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

protocols = {
	'xdg-shell': wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	# For ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT, used when the
	# scene builds feedback from compositor-supplied capabilities. wlroots
	# generates this header too but does not install it, and the alternative
	# -- hardcoding the flag's numeric value -- is a constant that would go
	# unnoticed if the protocol ever added another.
	'linux-dmabuf-v1': wl_protocol_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
}

protocols_code = {}
protocols_server_header = {}
protocols_client_header = {}
foreach name, path : protocols
	code = custom_target(
		name.underscorify() + '_c',
		input: path,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
	scenefx_files += code

	server_header = custom_target(
		name.underscorify() + '_server_h',
		input: path,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
	scenefx_files += server_header

	client_header = custom_target(
		name.underscorify() + '_client_h',
		input: path,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
		build_by_default: false,
	)

	protocols_code += { name: code }
	protocols_server_header += { name: server_header }
	protocols_client_header += { name: client_header }
endforeach
