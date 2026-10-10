let
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/e7439b6b14ad3cc35d05608ebca9bce01a25f5f8.tar.gz";
    sha256 = "19y4py973w62z4qb7gzn0chjs67ihxy8rbjb7g71iyk8yz7z5k8z";
  }) { system = "x86_64-linux"; };
  gst = pkgs.gst_all_1;
in
(pkgs.mkShell.override { stdenv = pkgs.gcc16Stdenv; }) {
  nativeBuildInputs = with pkgs; [
    cmake ninja pkg-config git python3 gnumake binutils mold coreutils bash
  ];
  buildInputs = with pkgs; [
    qt6.qtbase qt6.qtmultimedia qt6.qtsvg taglib zstd chromaprint glib
    gst.gstreamer gst.gst-plugins-base gst.gst-plugins-good
    gst.gst-plugins-bad gst.gst-libav pipewire pulseaudio wireplumber
    dbus ffmpeg fontconfig dejavu_fonts
  ];
  QT_QPA_PLATFORM = "offscreen";
  QT_PLUGIN_PATH = pkgs.lib.makeSearchPath "lib/qt-6/plugins" [
    pkgs.qt6.qtbase pkgs.qt6.qtmultimedia pkgs.qt6.qtsvg
  ];
  FONTCONFIG_FILE = "${pkgs.fontconfig.out}/etc/fonts/fonts.conf";
  FONTCONFIG_PATH = "${pkgs.fontconfig.out}/etc/fonts";
  GST_PLUGIN_SYSTEM_PATH_1_0 = pkgs.lib.makeSearchPath "lib/gstreamer-1.0" [
    gst.gst-plugins-base gst.gst-plugins-good gst.gst-plugins-bad
    gst.gst-libav pkgs.pipewire
  ];
  shellHook = ''
    export GST_REGISTRY="''${MUZAITEN_STATE_ROOT:-$PWD/agent-state/cloud}/cache/gstreamer-registry.bin"
    export CMAKE_BUILD_PARALLEL_LEVEL="''${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
  '';
}
