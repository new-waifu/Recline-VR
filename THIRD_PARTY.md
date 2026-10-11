# Third-party material

The project itself is MIT-licensed (see `LICENSE`). These parts keep their own
licenses:

- `overlay/font_data.h` is rasterized from **DejaVu Sans Mono** (Bitstream Vera
  derived license; DejaVu changes are public domain). See
  https://dejavu-fonts.github.io/License.html.
- `tools/gen_font.c` uses **stb_truetype.h** (public domain / MIT, Sean Barrett),
  fetched at build-tool time; it is not part of the shipped binaries.
- `third_party/openvr/` is a subset of the OpenVR SDK tag `v2.15.6`
  (https://github.com/ValveSoftware/openvr), copied unmodified in upstream's
  layout: `CMakeLists.txt`, `LICENSE`, `headers/` (`openvr.h`, `openvr_driver.h`,
  `openvr_capi.h`) and `src/` (the `openvr_api` loader, including its bundled
  jsoncpp); BSD-3-Clause, Valve Corporation. To update, replace those files from
  the new tag. Every interface is requested by the version string compiled in from
  these headers and the runtime keeps serving old versions, so they must not be
  newer than the SteamVR on the device. Update them only when a newer API is
  needed.
