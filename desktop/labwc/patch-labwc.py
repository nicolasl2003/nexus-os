#!/usr/bin/env python3
"""Adapte labwc 0.8.2 au bureau Nexus.

  1. Aimantation dans les coins : glisser une fenêtre dans un coin de
     l'écran (ou près d'un coin, le long d'un bord) la range dans le quart
     d'écran correspondant. Les quarts sont les régions « nexus-haut-gauche »,
     « nexus-haut-droite », « nexus-bas-gauche », « nexus-bas-droite » définies
     dans rc.xml. Le reste ne change pas : haut au centre = plein écran,
     bords gauche/droit = moitié d'écran.
  2. Zone invisible de redimensionnement autour des fenêtres élargie
     (8 -> 14 px) : plus facile à attraper à la souris.

Usage : patch-labwc.py <sources de labwc>
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])


def patch(rel, old, new):
    p = root / rel
    t = p.read_text()
    if t.count(old) != 1:
        sys.exit(f"patch-labwc : motif introuvable dans {rel} : {old[:50]!r}")
    p.write_text(t.replace(old, new))


patch("include/labwc.h",
      "enum view_edge edge_from_cursor(struct seat *seat, struct output **dest_output);",
      "enum view_edge edge_from_cursor(struct seat *seat, struct output **dest_output);\n"
      "struct region *corner_region_from_cursor(struct seat *seat);")

patch("src/interactive.c", """static bool
snap_to_region(struct view *view)
{""", """/* Nexus : région « quart d'écran » si le curseur est près d'un coin */
struct region *
corner_region_from_cursor(struct seat *seat)
{
	struct server *server = seat->server;
	if (server->input_mode != LAB_INPUT_STATE_MOVE
			|| !view_is_floating(server->grabbed_view)
			|| !rc.snap_edge_range) {
		return NULL;
	}
	struct output *output = output_nearest_to_cursor(server);
	if (!output_is_usable(output)) {
		return NULL;
	}
	double x = seat->cursor->x, y = seat->cursor->y;
	wlr_output_layout_output_coords(server->output_layout,
		output->wlr_output, &x, &y);
	struct wlr_box *a = &output->usable_area;
	if (a->width <= 0 || a->height <= 0) {
		return NULL;
	}
	int r = rc.snap_edge_range;
	bool left = x <= a->x + r, right = x >= a->x + a->width - r;
	bool top = y <= a->y + r, bottom = y >= a->y + a->height - r;
	double fx = (x - a->x) / a->width, fy = (y - a->y) / a->height;
	const char *name = NULL;
	if ((top && fx < 0.25) || (left && fy < 0.25)) {
		name = "nexus-haut-gauche";
	} else if ((top && fx > 0.75) || (right && fy < 0.25)) {
		name = "nexus-haut-droite";
	} else if ((bottom && fx < 0.25) || (left && fy > 0.75)) {
		name = "nexus-bas-gauche";
	} else if ((bottom && fx > 0.75) || (right && fy > 0.75)) {
		name = "nexus-bas-droite";
	}
	return name ? regions_from_name(name, output) : NULL;
}

static bool
snap_to_region(struct view *view)
{""")

patch("src/interactive.c", """		if (!snap_to_region(view)) {
			snap_to_edge(view);
		}""", """		if (!snap_to_region(view)) {
			struct region *corner =
				corner_region_from_cursor(&view->server->seat);
			if (corner) {
				view_snap_to_region(view, corner,
					/*store_natural_geometry*/ false);
			} else {
				snap_to_edge(view);
			}
		}""")

patch("src/overlay.c", """	/* Edge-snapping overlay */""", """	/* Nexus : aperçu du quart d'écran (coins) */
	struct region *corner = corner_region_from_cursor(seat);
	if (corner) {
		show_region_overlay(seat, corner);
		return;
	}

	/* Edge-snapping overlay */""")

patch("include/ssd.h", "#define SSD_EXTENDED_AREA 8", "#define SSD_EXTENDED_AREA 14")
print("labwc adapté à Nexus OS")
