from __future__ import annotations

from html import escape
from typing import Any

_PAGE_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Alarm Worker Status</title>
<meta http-equiv="refresh" content="10">
<style>
  body {{ font-family: system-ui, sans-serif; margin: 2rem; background: #0f172a; color: #e2e8f0; }}
  h1 {{ margin-bottom: 0.25rem; }}
  .subtitle {{ color: #94a3b8; margin-top: 0; }}
  .summary {{ display: flex; gap: 1.5rem; margin: 1.5rem 0; }}
  .card {{ background: #1e293b; border-radius: 8px; padding: 1rem 1.5rem; }}
  .card .value {{ font-size: 1.75rem; font-weight: bold; }}
  .card .label {{ color: #94a3b8; font-size: 0.85rem; text-transform: uppercase; }}
  table {{ border-collapse: collapse; width: 100%; margin-top: 1rem; }}
  th, td {{ text-align: left; padding: 0.5rem 0.75rem; border-bottom: 1px solid #334155; }}
  th {{ color: #94a3b8; text-transform: uppercase; font-size: 0.75rem; }}
  .state-online {{ color: #4ade80; font-weight: bold; }}
  .state-stale {{ color: #f87171; font-weight: bold; }}
  .empty {{ color: #94a3b8; margin-top: 1rem; }}
  footer {{ margin-top: 2rem; color: #64748b; font-size: 0.85rem; }}
</style>
</head>
<body>
<h1>ESP32 Alarm Worker</h1>
<p class="subtitle">Version {version} &middot; Service is running</p>
<div class="summary">
  <div class="card">
    <div class="value">{healthy_nodes}</div>
    <div class="label">Online nodes</div>
  </div>
  <div class="card">
    <div class="value">{known_nodes}</div>
    <div class="label">Known nodes</div>
  </div>
</div>
{nodes_section}
<footer>Auto-refreshes every 10s &middot; JSON status: <a href="/internal/status" style="color:#94a3b8;">/internal/status</a></footer>
</body>
</html>
"""

_ROW_TEMPLATE = """<tr>
  <td>{device_id}</td>
  <td>{name}</td>
  <td>{ip}</td>
  <td class="state-{state}">{state}</td>
  <td>{last_seen_age_s:.1f}s ago</td>
</tr>
"""


def render_index_html(status_data: dict[str, Any]) -> str:
    nodes = status_data.get("nodes", [])
    if nodes:
        rows = "".join(
            _ROW_TEMPLATE.format(
                device_id=escape(str(node.get("device_id", ""))),
                name=escape(str(node.get("name", ""))),
                ip=escape(str(node.get("ip", ""))),
                state=escape(str(node.get("state", "stale"))),
                last_seen_age_s=float(node.get("last_seen_age_s", 0.0)),
            )
            for node in nodes
        )
        nodes_section = (
            "<table><thead><tr><th>ID</th><th>Name</th><th>IP</th>"
            "<th>State</th><th>Last seen</th></tr></thead>"
            f"<tbody>{rows}</tbody></table>"
        )
    else:
        nodes_section = '<p class="empty">No nodes have reported telemetry yet.</p>'

    return _PAGE_TEMPLATE.format(
        version=escape(str(status_data.get("version", ""))),
        healthy_nodes=escape(str(status_data.get("healthy_nodes", 0))),
        known_nodes=escape(str(status_data.get("known_nodes", 0))),
        nodes_section=nodes_section,
    )
