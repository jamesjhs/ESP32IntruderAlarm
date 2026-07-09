#!/usr/bin/env bash
set -Eeuo pipefail

APP_NAME="esp32-intruder-alarm"
WORKER_SERVICE="esp32-alarm-worker"
PWA_PM2_NAME="esp32-alarm-pwa"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_DIR="${SCRIPT_DIR}/python"
PWA_DIR="${SCRIPT_DIR}/PWA"
ENV_FILE="${SCRIPT_DIR}/.env"
ENV_EXAMPLE="${SCRIPT_DIR}/.env.example"
PYTHON_VENV="${PYTHON_DIR}/.venv"
RUN_USER="${SUDO_USER:-$(id -un)}"
RUN_GROUP="$(id -gn "${RUN_USER}")"
RUN_HOME="$(getent passwd "${RUN_USER}" | cut -d: -f6)"

log() {
  printf '\n[%s] %s\n' "${APP_NAME}" "$*"
}

warn() {
  printf '\n[%s] WARNING: %s\n' "${APP_NAME}" "$*" >&2
}

die() {
  printf '\n[%s] ERROR: %s\n' "${APP_NAME}" "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

if [[ "${EUID}" -eq 0 ]]; then
  SUDO=()
else
  require_command sudo
  SUDO=(sudo)
fi

run_as_user() {
  if [[ "$(id -un)" == "${RUN_USER}" ]]; then
    "$@"
  else
    sudo -H -u "${RUN_USER}" "$@"
  fi
}

require_command systemctl
require_command python3
require_command node
require_command npm

if [[ ! -f "${ENV_FILE}" ]]; then
  if [[ -f "${ENV_EXAMPLE}" ]]; then
    log "Creating ${ENV_FILE} from .env.example"
    cp "${ENV_EXAMPLE}" "${ENV_FILE}"
    chown "${RUN_USER}:${RUN_GROUP}" "${ENV_FILE}" || true
    chmod 600 "${ENV_FILE}" || true
    warn "Edit ${ENV_FILE} and replace bootstrap secrets before relying on this install."
  else
    die "Missing ${ENV_FILE} and ${ENV_EXAMPLE}"
  fi
fi

log "Installing Python worker into ${PYTHON_VENV}"
run_as_user python3 -m venv "${PYTHON_VENV}"
run_as_user "${PYTHON_VENV}/bin/python" -m pip install --upgrade pip setuptools
run_as_user "${PYTHON_VENV}/bin/python" -m pip install -e "${PYTHON_DIR}"
run_as_user "${PYTHON_VENV}/bin/python" -m pip check

log "Installing systemd unit ${WORKER_SERVICE}.service"
SERVICE_FILE="/etc/systemd/system/${WORKER_SERVICE}.service"
"${SUDO[@]}" tee "${SERVICE_FILE}" >/dev/null <<EOF
[Unit]
Description=ESP32 Intruder Alarm Python telemetry worker
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${RUN_USER}
Group=${RUN_GROUP}
WorkingDirectory=${SCRIPT_DIR}
EnvironmentFile=${ENV_FILE}
ExecStart=${PYTHON_VENV}/bin/python -m esp32_alarm_worker.server
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

"${SUDO[@]}" systemctl daemon-reload
"${SUDO[@]}" systemctl enable "${WORKER_SERVICE}.service"
"${SUDO[@]}" systemctl restart "${WORKER_SERVICE}.service"
"${SUDO[@]}" systemctl --no-pager --full status "${WORKER_SERVICE}.service" || true

log "Installing PWA dependencies"
cd "${PWA_DIR}"
if [[ -f package-lock.json ]]; then
  run_as_user npm ci
else
  run_as_user npm install
fi
run_as_user npm audit fix
run_as_user npm run build
run_as_user npm audit --omit=dev

if ! command -v pm2 >/dev/null 2>&1; then
  log "Installing PM2 globally"
  if npm install -g pm2; then
    :
  else
    "${SUDO[@]}" npm install -g pm2
  fi
fi

log "Starting or restarting PWA with PM2"
if run_as_user pm2 describe "${PWA_PM2_NAME}" >/dev/null 2>&1; then
  run_as_user pm2 restart "${PWA_PM2_NAME}" --update-env
else
  run_as_user pm2 start "${PWA_DIR}/dist/server.js" --name "${PWA_PM2_NAME}" --time
fi
run_as_user pm2 save

log "Configuring PM2 startup integration"
if ! "${SUDO[@]}" env PATH="${PATH}" pm2 startup systemd -u "${RUN_USER}" --hp "${RUN_HOME}"; then
  warn "PM2 startup setup did not complete automatically. Run 'pm2 startup' and follow its printed command."
fi

log "Deployment complete"
printf 'Python worker: systemctl status %s.service\n' "${WORKER_SERVICE}"
printf 'PWA service:    pm2 status %s\n' "${PWA_PM2_NAME}"
printf 'PWA URL:        http://127.0.0.1:3000\n'
printf 'ESP32 ingest:   http://<pi-lan-ip>:1000/espdata\n'
