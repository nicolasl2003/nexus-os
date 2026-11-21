#!/usr/bin/env python3
"""Adapte les sources de NetSurf (netsurf-all) à Nexus OS.

  1. libnsfb : la sortie Wayland « wld » (wl_shell, obsolète) est remplacée
     par la nôtre (nexus-wl.c, xdg-shell) ; elle a besoin de xkbcommon.
  2. Touches Unicode : NSFB_KEY_UCS4_FLAG | caractère, pour que les
     caractères du clavier AZERTY (é, è, à, ç, €...) arrivent tels quels.
  3. Barre d'adresse : saisie et effacement en UTF-8 (et non octet par octet).
  4. Édition de liens : seule libnsfb est liée en « whole-archive ».

Usage : patch-netsurf.py <dossier netsurf-all>
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])


def patch(rel, old, new, count=1):
    path = root / rel
    text = path.read_text()
    if text.count(old) != count:
        sys.exit(f"patch-netsurf : motif introuvable dans {rel} : {old[:60]!r}")
    path.write_text(text.replace(old, new))


# 1. libnsfb : notre sortie Wayland
patch("libnsfb/src/surface/Makefile",
      "SURFACE_HANDLER_$(NSFB_WLD_AVAILABLE) += wld.c",
      "SURFACE_HANDLER_$(NSFB_WLD_AVAILABLE) += nexus-wl.c xdg-shell-protocol.c "
      "xdg-decoration-unstable-v1-protocol.c")
patch("libnsfb/Makefile",
      "$(eval $(call pkg_config_package_available,NSFB_WLD_AVAILABLE,wayland-client))",
      "$(eval $(call pkg_config_package_available,NSFB_WLD_AVAILABLE,wayland-client xkbcommon))")
patch("libnsfb/Makefile",
      """  $(eval $(call pkg_config_package_add_flags,wayland-client,CFLAGS))
  $(eval $(call pkg_config_package_add_flags,wayland-client,TESTCFLAGS,TESTLDFLAGS))

  REQUIRED_PKGS := $(REQUIRED_PKGS) wayland-client""",
      """  $(eval $(call pkg_config_package_add_flags,wayland-client xkbcommon,CFLAGS))
  $(eval $(call pkg_config_package_add_flags,wayland-client xkbcommon,TESTCFLAGS,TESTLDFLAGS))

  REQUIRED_PKGS := $(REQUIRED_PKGS) wayland-client xkbcommon""")

# 2. Touches Unicode
patch("libnsfb/include/libnsfb_event.h",
      "#define _LIBNSFB_EVENT_H 1",
      "#define _LIBNSFB_EVENT_H 1\n\n"
      "/* Nexus OS : touche portant directement un caractère Unicode */\n"
      "#define NSFB_KEY_UCS4_FLAG 0x40000000")
patch("netsurf/frontends/framebuffer/fbtk/event.c",
      """fbtk_keycode_to_ucs4(int code, fbtk_modifier_type mods)
{
	int ucs4 = -1;
""",
      """fbtk_keycode_to_ucs4(int code, fbtk_modifier_type mods)
{
	int ucs4 = -1;

	if (code & NSFB_KEY_UCS4_FLAG)
		return code & 0x1fffff;
""")

# 3. Barre d'adresse en UTF-8
TEXT = "netsurf/frontends/framebuffer/fbtk/text.c"
patch(TEXT,
      """		memmove(widget->u.text.text + widget->u.text.idx - 1,
				widget->u.text.text + widget->u.text.idx,
				widget->u.text.len - widget->u.text.idx);
		widget->u.text.idx--;
		widget->u.text.len--;""",
      """		{
		int n = 1;
		while (widget->u.text.idx - n > 0 &&
		       (widget->u.text.text[widget->u.text.idx - n] & 0xC0) == 0x80)
			n++;
		memmove(widget->u.text.text + widget->u.text.idx - n,
				widget->u.text.text + widget->u.text.idx,
				widget->u.text.len - widget->u.text.idx);
		widget->u.text.idx -= n;
		widget->u.text.len -= n;
		}""")
patch(TEXT,
      """			if (modifier == FBTK_MOD_CLEAR)
				widget->u.text.idx++;""",
      """			if (modifier == FBTK_MOD_CLEAR) {
				widget->u.text.idx++;
				while (widget->u.text.idx < widget->u.text.len &&
				       (widget->u.text.text[widget->u.text.idx] & 0xC0) == 0x80)
					widget->u.text.idx++;
			}""")
patch(TEXT,
      """			if (modifier == FBTK_MOD_CLEAR)
				widget->u.text.idx--;""",
      """			if (modifier == FBTK_MOD_CLEAR) {
				widget->u.text.idx--;
				while (widget->u.text.idx > 0 &&
				       (widget->u.text.text[widget->u.text.idx] & 0xC0) == 0x80)
					widget->u.text.idx--;
			}""")
patch(TEXT,
      """		/* allow for new character and null */
		temp = realloc(widget->u.text.text, widget->u.text.len + 2);
		if (temp == NULL) {
			break;
		}

		widget->u.text.text = temp;
		memmove(widget->u.text.text + widget->u.text.idx + 1,
				widget->u.text.text + widget->u.text.idx,
				widget->u.text.len - widget->u.text.idx);
		widget->u.text.text[widget->u.text.idx] =
				fbtk_keycode_to_ucs4(value, modifier);
		widget->u.text.idx++;
		widget->u.text.len++;""",
      """		{
		int uc = fbtk_keycode_to_ucs4(value, modifier);
		char u8[4];
		int n;
		if (uc < 32 || uc == 127)
			break;
		if (uc < 0x80) {
			u8[0] = uc; n = 1;
		} else if (uc < 0x800) {
			u8[0] = 0xC0 | (uc >> 6); u8[1] = 0x80 | (uc & 0x3F); n = 2;
		} else if (uc < 0x10000) {
			u8[0] = 0xE0 | (uc >> 12); u8[1] = 0x80 | ((uc >> 6) & 0x3F);
			u8[2] = 0x80 | (uc & 0x3F); n = 3;
		} else {
			u8[0] = 0xF0 | (uc >> 18); u8[1] = 0x80 | ((uc >> 12) & 0x3F);
			u8[2] = 0x80 | ((uc >> 6) & 0x3F); u8[3] = 0x80 | (uc & 0x3F); n = 4;
		}
		/* allow for new character and null */
		temp = realloc(widget->u.text.text, widget->u.text.len + n + 1);
		if (temp == NULL) {
			break;
		}

		widget->u.text.text = temp;
		memmove(widget->u.text.text + widget->u.text.idx + n,
				widget->u.text.text + widget->u.text.idx,
				widget->u.text.len - widget->u.text.idx);
		memcpy(widget->u.text.text + widget->u.text.idx, u8, n);
		widget->u.text.idx += n;
		widget->u.text.len += n;
		}""")

# 4. Édition de liens statique
patch("netsurf/frontends/framebuffer/Makefile",
      """LDFLAGS += -Wl,--whole-archive
$(eval $(call pkg_config_find_and_add,libnsfb,libnsfb))
LDFLAGS += -Wl,--no-whole-archive""",
      """CFLAGS += $(shell $(PKG_CONFIG) --cflags libnsfb)
LDFLAGS += -Wl,--whole-archive -lnsfb -Wl,--no-whole-archive $(shell $(PKG_CONFIG) --libs libnsfb)""")

print("NetSurf adapté à Nexus OS")
