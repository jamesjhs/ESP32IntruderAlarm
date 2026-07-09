#!/usr/bin/env bash
set -Eeuo pipefail

APP_NAME="esp32-intruder-alarm"
WORKER_SERVICE="esp32-alarm-worker"
PWA_PM2_NAME="esp32-alarm-pwa"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PWA_DIR="${SCRIPT_DIR}/PWA"
ENV_FILE="${SCRIPT_DIR}/.env"
ENV_EXAMPLE="${SCRIPT_DIR}/.env.example"
RUN_USER="${SUDO_USER:-$(id -un)}"

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

set_env_value() {
  local key="$1"
  local value="$2"

  if grep -qE "^${key}=" "${ENV_FILE}"; then
    sed -i "s|^${key}=.*|${key}=${value}|" "${ENV_FILE}"
  else
    printf '%s=%s\n' "${key}" "${value}" >> "${ENV_FILE}"
  fi
}

require_command systemctl

if [[ ! -f "${ENV_FILE}" ]]; then
  if [[ -f "${ENV_EXAMPLE}" ]]; then
    log "Creating ${ENV_FILE} from .env.example"
    cp "${ENV_EXAMPLE}" "${ENV_FILE}"
    chown "${RUN_USER}:$(id -gn "${RUN_USER}")" "${ENV_FILE}" || true
    chmod 600 "${ENV_FILE}" || true
    warn "Review ${ENV_FILE} for secrets after this amendment."
  else
    die "Missing ${ENV_FILE} and ${ENV_EXAMPLE}"
  fi
fi

log "Amending service ports in ${ENV_FILE}"
set_env_value "PWA_PORT" "3015"
set_env_value "WORKER_INTERNAL_URL" "http://127.0.0.1:3005"
set_env_value "WORKER_PORT" "3005"

log "Restarting Python worker on port 3005"
"${SUDO[@]}" systemctl daemon-reload
if "${SUDO[@]}" systemctl cat "${WORKER_SERVICE}.service" >/dev/null 2>&1; then
  "${SUDO[@]}" systemctl restart "${WORKER_SERVICE}.service"
  "${SUDO[@]}" systemctl --no-pager --full status "${WORKER_SERVICE}.service" || true
else
  warn "Systemd unit ${WORKER_SERVICE}.service was not found; run ./install-and-run.sh for a fresh service install."
fi

log "Restarting PWA with PM2 on port 3015"
cd "${PWA_DIR}"

if [[ -d node_modules && -f package.json ]]; then
  require_command npm
  run_as_user npm run build
else
  warn "Skipping PWA rebuild because node_modules is missing. Existing dist output will be restarted."
fi

if ! command -v pm2 >/dev/null 2>&1; then
  die "Missing pm2. This amendment script expects an existing install; run ./install-and-run.sh for a fresh setup."
fi

if run_as_user pm2 describe "${PWA_PM2_NAME}" >/dev/null 2>&1; then
  run_as_user pm2 restart "${PWA_PM2_NAME}" --update-env
else
  run_as_user pm2 start "${PWA_DIR}/dist/server.js" --name "${PWA_PM2_NAME}" --time
fi
run_as_user pm2 save

log "Port amendment complete"
printf 'Python worker: systemctl status %s.service\n' "${WORKER_SERVICE}"
printf 'PWA service:    pm2 status %s\n' "${PWA_PM2_NAME}"
printf 'PWA URL:        http://127.0.0.1:3015\n'
printf 'ESP32 ingest:   http://<pi-lan-ip>:3005/espdata\n'
