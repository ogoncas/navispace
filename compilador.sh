#!/bin/bash
# Compila e instala o plugin (executar como root)
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins"
DESKTOP_DIR="/usr/share/xfce4/panel/plugins"

if [ "$(id -u)" -ne 0 ]; then
    echo "Erro: execute como root." >&2
    exit 1
fi

cd "$DIR"

echo "==> Compilando..."
gcc -O2 -shared -fPIC navispace.c -o libnavispace.so \
    $(pkg-config --cflags --libs gtk+-3.0 libxfce4panel-2.0 libwnck-3.0 libxfce4util-1.0) -lm

echo "==> Instalando..."
install -d "$LIB_DIR" "$DESKTOP_DIR"
cp libnavispace.so "$LIB_DIR/"
cp navispace.desktop "$DESKTOP_DIR/"
chmod 644 "$LIB_DIR/libnavispace.so" "$DESKTOP_DIR/navispace.desktop"

echo "==> Reiniciando o painel..."
# O painel precisa ser reiniciado para carregar a nova versão do plugin.
# Como root, o comando é executado como o usuário da sessão gráfica.
SESSION_USER="${SUDO_USER:-$(logname 2>/dev/null || true)}"
if [ -n "$SESSION_USER" ] && [ "$SESSION_USER" != "root" ]; then
    su - "$SESSION_USER" -c 'DISPLAY="${DISPLAY:-:0}" xfce4-panel -r' >/dev/null 2>&1 &
    echo "Painel reiniciado para o usuário $SESSION_USER."
else
    echo "Não foi possível detectar o usuário da sessão."
    echo "Reinicie o painel manualmente com: xfce4-panel -r"
fi

echo "Concluído."