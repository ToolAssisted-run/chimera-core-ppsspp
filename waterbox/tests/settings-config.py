#!/usr/bin/env python3
# Writes a copy of a miniHawk config with the given core sync settings applied - the same shape the
# settings dialog (and a movie header) stores: a name -> value map under the adapter's type name.
#
# Usage: settings-config.py <source config> <output config> <settings JSON>
import json
import sys

cfg = json.load(open(sys.argv[1]))
cfg.setdefault("CoreSyncSettings", {})["BizHawk.Emulation.Common.Waterbox.WaterboxCore"] = {
    "Values": json.loads(sys.argv[3])
}
# Pick this core explicitly: a miniHawk install can hold several packages claiming the PSP, and --core only LOADS a package, it does not choose it.
cfg.setdefault("DefaultCores", {})["PSP"] = "PPSSPP"
json.dump(cfg, open(sys.argv[2], "w"), indent=2)
