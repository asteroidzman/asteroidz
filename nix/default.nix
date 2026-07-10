{
  lib,
  libX11,
  libinput,
  libxcb,
  libdrm,
  libxkbcommon,
  pcre2,
  pango,
  cjson,
  pixman,
  gdk-pixbuf,
  systemd,
  vulkan-loader,
  pkg-config,
  stdenv,
  wayland,
  wayland-protocols,
  wayland-scanner,
  libxcb-wm,
  xwayland,
  meson,
  ninja,
  scenefx,
  wlroots_0_20,
  libGL,
  enableXWayland ? true,
  debug ? false,
}:
stdenv.mkDerivation {
  pname = "asteroidz";
  version = "0.15.3";

  src = builtins.path {
    path = ../.;
    name = "source";
  };

  mesonFlags = [
    (lib.mesonEnable "xwayland" enableXWayland)
    (lib.mesonBool "asan" debug)
  ];

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs =
    [
      libinput
      libxcb
      libxkbcommon
      pcre2
      pango
      cjson
      pixman
      gdk-pixbuf
      systemd
      vulkan-loader
      wayland
      wayland-protocols
      wlroots_0_20
      scenefx
      libGL
      libdrm
    ]
    ++ lib.optionals enableXWayland [
      libX11
      libxcb-wm
      xwayland
    ];

  passthru = {
    providedSessions = ["asteroidz" "asteroidz-gles"];
  };

  meta = {
    mainProgram = "asteroidz";
    description = "wlroots compositor with a Vulkan renderer, HDR10, and dwm-style tags";
    homepage = "https://github.com/asteroidzman/asteroidz";
    license = lib.licenses.gpl3Plus;
    maintainers = [];
    platforms = lib.platforms.unix;
  };
}
