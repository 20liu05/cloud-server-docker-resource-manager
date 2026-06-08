#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker 未安装，请先安装 Docker Engine。"
  exit 1
fi

if ! docker compose version >/dev/null 2>&1; then
  echo "Docker Compose 插件不可用，请先安装 docker compose。"
  exit 1
fi

if [ ! -f .env ]; then
  cat > .env <<EOF
PANEL_USER=admin
PANEL_PASSWORD=123456
PANEL_PORT=9849
EOF
  echo "已生成 .env。"
fi

mkdir -p data
if [ ! -f data/panel.conf ]; then
  cat > data/panel.conf <<EOF
USER=admin
PASSWORD=123456
PROTECTED=docker-panel
EOF
  chmod 600 data/panel.conf
  echo "已生成 data/panel.conf。"
fi

echo "正在编译面板后端..."
gcc -O2 -static -s -o docker-panel panel.c

docker compose up -d --build

PANEL_PORT_VALUE="$(grep '^PANEL_PORT=' .env | cut -d= -f2-)"
PANEL_HOST_VALUE="${PANEL_HOST:-你的服务器IP}"

echo "Docker 控制面板已启动："
echo "  URL: http://${PANEL_HOST_VALUE}:${PANEL_PORT_VALUE:-9849}"
echo "  用户名: $(grep '^USER=' data/panel.conf | cut -d= -f2-)"
echo "  密码: $(grep '^PASSWORD=' data/panel.conf | cut -d= -f2-)"
