# security-context.sh — the privileged-global deny list actually denies.
#
# The list in src/ext-protocol/modern.h is interface-name strings matched with
# strcmp. An entry naming no real global is not a build error and not a runtime
# error -- it is just a line that never fires, which is how
# "wlr_export_dmabuf_manager_v1" (no zwlr_ prefix) shipped as a hole handing
# screen capture to every sandboxed client. Enumerating the registry from inside
# a real security context is the only way to see it, so this asserts the whole
# list rather than the one entry that was wrong.

# Keep in sync with privileged_global_interfaces[] in modern.h.
HL_PRIVILEGED_GLOBALS="
wp_security_context_manager_v1
zwlr_screencopy_manager_v1
zwlr_export_dmabuf_manager_v1
ext_image_copy_capture_manager_v1
ext_output_image_capture_source_manager_v1
ext_foreign_toplevel_image_capture_source_manager_v1
zwlr_data_control_manager_v1
ext_data_control_manager_v1
zwlr_gamma_control_manager_v1
zwlr_output_manager_v1
zwlr_output_power_manager_v1
zwp_virtual_keyboard_manager_v1
zwlr_virtual_pointer_manager_v1
zwp_input_method_manager_v2
zwlr_foreign_toplevel_manager_v1
ext_foreign_toplevel_list_v1
ext_workspace_manager_v1
zwlr_layer_shell_v1
ext_session_lock_manager_v1
zdwl_ipc_manager_v2
ext_background_effect_manager_v1
"

test_sandboxed_client_sees_no_privileged_global() {
	local visible leaked=""
	visible="$(hl_sandbox_globals)"

	# premise: the sandboxed connection worked at all. Without this an empty
	# global list -- a failed connect, a missing binary -- reads as a pass.
	hl_assert_true "premise: sandboxed client sees the ordinary globals" \
		"$(echo "$visible" | grep -qx wl_compositor && echo true || echo false)"

	local iface
	for iface in $HL_PRIVILEGED_GLOBALS; do
		if echo "$visible" | grep -qx "$iface"; then
			leaked="$leaked $iface"
		fi
	done

	hl_assert_eq "no privileged global is visible to a sandboxed client" \
		"$leaked" ""
}

test_unsandboxed_client_still_sees_privileged_globals() {
	# The mirror image: proves the assertion above is measuring the filter and
	# not just a compositor that never advertised these in the first place.
	hl_assert_true "an ordinary client still sees zwlr_export_dmabuf_manager_v1" \
		"$(wayland-info 2>/dev/null | grep -q "zwlr_export_dmabuf_manager_v1" && echo true || echo false)"
}
