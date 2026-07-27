exclude_files = ['meson.build']

# Built inside asteroidz these headers are a private build detail: the library
# is linked statically and nothing off-tree compiles against it, so installing
# them would only collide with a system asteroidz-scenefx.
if not meson.is_subproject()
	install_subdir('scenefx',
		install_dir: get_option('includedir') / versioned_name,
		exclude_files: exclude_files,
	)
endif
