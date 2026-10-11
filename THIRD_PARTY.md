# Third-party material

- `overlay/font_data.h` is rasterized from **DejaVu Sans Mono** (Bitstream Vera
  derived license; DejaVu changes are public domain). See
  https://dejavu-fonts.github.io/License.html.
- `tools/gen_font.c` uses **stb_truetype.h** (public domain / MIT, Sean Barrett),
  fetched at build-tool time; it is not part of the shipped binaries.
- `third_party/openvr/` holds `openvr.h`, `openvr_driver.h` and `LICENSE`, copied
  unmodified from the OpenVR SDK tag `v2.15.6`
  (https://github.com/ValveSoftware/openvr); BSD-3-Clause, Valve Corporation. Every
  interface is requested by the version string compiled in from these headers and
  the runtime keeps serving old versions, so they must not be newer than the
  SteamVR on the device. Update them only when a newer API is needed.
