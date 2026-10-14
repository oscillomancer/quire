# SPDX-License-Identifier: GPL-3.0-or-later
"""The sandbox CLI discovers its bundled player without --plugin-path."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="supple-dsp-discovery-") as folder:
    env = dict(os.environ, XDG_CACHE_HOME=folder,
               CLAP_PATH="", VST3_PATH="", SUPPLE_SCRIPT_LIBRARY="")
    source = '''#include <supple/script.h>
struct Discovery : Effect {
    static constexpr Info info = {.name = "Discovery", .id = "com.example.discovery"};
    void process(Block& io) {}
};
'''

    def call(command, args):
        reply = subprocess.run(
            [sys.argv[1], "exec", json.dumps({"command": command, "args": args}),
             "--workdir", folder], env=env, capture_output=True, text=True, check=True)
        return json.loads(reply.stdout)

    written = call("dsp.write", {"name": "discovery", "source": source})
    assert written["ok"], written
    assert written["listed"], written
    assert Path(written["path"]).read_text() == source
    loaded = call("plugin.load", {"plugin": "com.example.discovery"})
    assert loaded["semantic"], loaded
