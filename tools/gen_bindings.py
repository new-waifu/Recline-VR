#!/usr/bin/env python3
# Writes the overlay's default controller bindings
# (packaging/resources/overlay/bindings_*.json) and their list in actions.json.
# Every controller gets the same single chord, so one description fits all:
#
#   hold both grips + click both sticks: show the panel
#
# "Stick" is the thumbstick, or the trackpad on controllers without one. Recline
# and reset have no default binding: SteamVR's own recenter does both (the
# overlay's systemRecenter option, on by default), and no extra chord can then
# clash with a game. Users can still bind them in SteamVR's binding editor.
#
# Every source is in "button" mode and every chord input is "single", as
# SteamVR's binding editor writes them: a chord input must be one its source's
# mode provides ("click" is the trigger mode's), or SteamVR logs "Unbound
# input ... for chord, ignoring chord" and drops the chord. "held" and "long"
# don't delay a chord (tested on the Frame), so there is no long-press variant.
# Run from the source root after changing CONTROLLERS.
import json
import os
import re

# controller type: (grip, stick) input names, as in each type's
# SteamVR input profile (Oasis for holographic_controller and hpmotioncontroller).
CONTROLLERS = {
    "frame_controller": ("grip", "thumbstick"),
    "knuckles": ("grip", "thumbstick"),
    "oculus_touch": ("grip", "joystick"),
    "hpmotioncontroller": ("grip", "joystick"),
    "holographic_controller": ("grip", "joystick"),
    "vive_controller": ("grip", "trackpad"),
    "generic": ("grip", "trackpad"),
}

DESCRIPTION = ("Recline VR defaults: hold both grips and click both sticks (trackpad where there is no "
               "stick) to show the panel. Recline and reset with SteamVR's own recenter.")

OUT = "packaging/resources/overlay"


def binding(grip, stick):
    def p(hand, name):
        return "/user/hand/%s/input/%s" % (hand, name)

    hands = ("left", "right")
    sources = [{"path": p(h, n), "mode": "button", "inputs": {}, "parameters": {}}
               for n in (grip, stick) for h in hands]
    return {
        "/actions/recline": {
            "sources": sources,
            "chords": [
                {"output": "/actions/recline/in/show_panel",
                 "inputs": [[p(h, n), "single"] for n in (grip, stick) for h in hands]},
            ],
            "haptics": [{"output": "/actions/recline/out/haptic", "path": "/user/hand/%s/output/haptic" % h}
                        for h in hands],
            "poses": [],
            "skeleton": [],
        }
    }


def write(path, data):
    text = json.dumps(data, indent="\t")
    # [ "path", "click" ] on one line, as in the hand-written files.
    text = re.sub(r'\[\s*("[^"]*"),\s*("[^"]*")\s*\]', r"[ \1, \2 ]", text)
    with open(path, "w", newline="\n") as f:
        f.write(text + "\n")


def main():
    for ctype, names in CONTROLLERS.items():
        write(os.path.join(OUT, "bindings_%s.json" % ctype),
              {"controller_type": ctype, "description": DESCRIPTION, "name": "Recline VR defaults",
               "bindings": binding(*names)})
    # Only the default_bindings list of actions.json, keeping the rest as written.
    path = os.path.join(OUT, "actions.json")
    with open(path) as f:
        text = f.read()
    rows = ",\n".join('\t\t{ "controller_type" : "%s", "binding_url" : "bindings_%s.json" }' % (c, c)
                      for c in CONTROLLERS)
    text, n = re.subn(r'"default_bindings" : \[.*?\n\t\],', lambda m: '"default_bindings" : [\n' + rows + "\n\t],",
                      text, flags=re.S)
    assert n == 1, "default_bindings not found in actions.json"
    with open(path, "w", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
