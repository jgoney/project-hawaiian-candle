# Local Godot editor plugin

The locally installed `godot_ai/` directory is ignored by Git. It is third-party
tooling, not part of the research source tree.

- Upstream: <https://github.com/hi-godot/godot-ai>
- Release: `v3.0.4`
- Commit: `f94116ce0f75e64350ed03277d48c27d4535fa20`
- Installed content: upstream `plugin/addons/godot_ai/`

Launch through `scripts/run-godot-editor.sh`. It binds the MCP service to its
default loopback endpoints and opts out of telemetry. Do not expose ports 8000
or 9500 to another host.

The plugin's self-update feature can replace the ignored local directory. When
reproducing research, keep the pinned version above or update this record after
reviewing and testing a newer release.
