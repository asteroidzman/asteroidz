wayland_scanner = find_program('wayland-scanner')
wayland_protos_dep = dependency('wayland-protocols')
wl_protocol_dir = wayland_protos_dep.get_variable(pkgconfig:'pkgdatadir')
wayland_scanner_code = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
wayland_scanner_client = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.h',
	arguments: ['server-header', '@INPUT@', '@OUTPUT@'])

wayland_xmls = [
	wl_protocol_dir + '/stable/xdg-shell/xdg-shell.xml',
	# For the scene graph in src/scene/, which needs
	# ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT when it builds dmabuf
	# feedback tranches. scenefx generated this itself; the graph is ours now.
	wl_protocol_dir + '/stable/linux-dmabuf/linux-dmabuf-v1.xml',
	# Xwayland is given its own xdg-output, in device pixels, so its X screen
	# comes out the same size as the windows this compositor puts in it. See
	# xdg_output_visible_to() in src/ext-protocol/modern.h.
	wl_protocol_dir + '/unstable/xdg-output/xdg-output-unstable-v1.xml',
	wl_protocol_dir + '/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
	wl_protocol_dir + '/staging/cursor-shape/cursor-shape-v1.xml',
	wl_protocol_dir + '/stable/tablet/tablet-v2.xml',
	wl_protocol_dir + '/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
	wl_protocol_dir + '/staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
	wl_protocol_dir + '/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
	wl_protocol_dir + '/staging/ext-workspace/ext-workspace-v1.xml',
	wl_protocol_dir + '/staging/tearing-control/tearing-control-v1.xml',
	wl_protocol_dir + '/staging/ext-background-effect/ext-background-effect-v1.xml',
	# Phase 1 of native wp-color-management ownership. Scanned but not yet
	# authoritative: wlroots still creates the global unless AZ_NATIVE_CM=1.
	wl_protocol_dir + '/staging/color-management/color-management-v1.xml',
	'wlr-foreign-toplevel-management-unstable-v1.xml',
	'frog-color-management-v1.xml',
	'dwl-ipc-unstable-v2.xml',
	'wlr-layer-shell-unstable-v1.xml',
	'wlr-output-power-management-unstable-v1.xml',
]
wayland_sources = [
	wayland_scanner_code.process(wayland_xmls),
	wayland_scanner_client.process(wayland_xmls),
]

# ── CLIENT headers, for the programs in tools/ ────────────────────────────
#
# The generator above is named `client` and emits a SERVER header, which is
# what the compositor needs. A Wayland client needs the other one, so azview
# gets its own pair rather than the misnamed one being changed underneath the
# compositor.
wayland_scanner_client_header = generator(
	wayland_scanner,
	output: '@BASENAME@-client-protocol.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'])

azview_xmls = [
	wl_protocol_dir + '/stable/xdg-shell/xdg-shell.xml',
	wl_protocol_dir + '/staging/color-management/color-management-v1.xml',
	wl_protocol_dir + '/stable/viewporter/viewporter.xml',
]
azview_protocol_sources = [
	wayland_scanner_code.process(azview_xmls),
	wayland_scanner_client_header.process(azview_xmls),
]
