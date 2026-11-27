#!/usr/bin/env python3
"""Write the ESP Web Tools manifest. Usage: make_manifest.py <version> <full-bin file name> <out.json>"""
import json, sys

version, fname, out = sys.argv[1:4]
manifest = {
    "name": "SC01 Plus Bedside Controller",
    "version": version,
    "new_install_prompt_erase": False,
    "builds": [{
        "chipFamily": "ESP32-S3",
        "improv": True,  # lets the flasher ask for Wi-Fi credentials after install
        "parts": [{"path": "firmware/" + fname, "offset": 0}],
    }],
}
json.dump(manifest, open(out, "w"), indent=2)
