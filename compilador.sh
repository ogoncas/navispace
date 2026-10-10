#!/bin/bash
set -euo pipefail

NAME="navispace"
DEPS=(gtk+-3.0 libxfce4panel-2.0 libwnck-3.0 libxfce4util-1.0)
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -t 1 ]; then
    BOLD=$'\e[1m'; GREEN=$'\e[32m'; YELLOW=$'\e[33m'; RED=$'\e[31m'; RESET=$'\e[0m'
else
    BOLD=""; GREEN=""; YELLOW=""; RED=""; RESET=""
fi

step() { echo "${BOLD}==>${RESET} $*"; }
die()  { echo "${RED}Erro:${RESET} $*" >&2; exit 1; }

usage() {
    cat <<USAGE
Uso (como root):
  $0               compila e instala
  $0 --uninstall   remove o plugin
O painel não é reiniciado; o script avisa quando for necessário.
USAGE
}

notify_restart() {
    echo
    if pgrep -x xfce4-panel >/dev/null 2>&1; then
        echo "${YELLOW}${BOLD}Reinicie o painel para aplicar a mudança:${RESET} xfce4-panel -r"
    else
        echo "O painel não está em execução; a mudança valerá na próxima inicialização."
    fi
}

case "${1:-}" in
    "")          ACTION=install ;;
    --uninstall) ACTION=uninstall ;;
    -h|--help)   usage; exit 0 ;;
    *)           usage >&2; die "opção desconhecida: $1" ;;
esac

[ "$(id -u)" -eq 0 ] || die "execute como root (sudo $0)."
command -v pkg-config >/dev/null || die "pkg-config não encontrado."
pkg-config --exists libxfce4panel-2.0 || die "libxfce4panel-2.0 (desenvolvimento) não encontrada."

LIB_FILE="$(pkg-config --variable=libdir libxfce4panel-2.0)/xfce4/panel/plugins/lib$NAME.so"
DESKTOP_FILE="$(pkg-config --variable=datarootdir libxfce4panel-2.0)/xfce4/panel/plugins/$NAME.desktop"

if [ "$ACTION" = uninstall ]; then
    step "Removendo $NAME..."
    removed=0
    for f in "$LIB_FILE" "$DESKTOP_FILE"; do
        if [ -e "$f" ]; then
            rm -f -- "$f"
            echo "  removido: $f"
            removed=1
        fi
    done
    if [ "$removed" -eq 0 ]; then
        echo "Nada para remover: o plugin não está instalado."
        exit 0
    fi
    echo "${GREEN}Concluído.${RESET}"
    notify_restart
    exit 0
fi

command -v gcc >/dev/null || die "gcc não encontrado (Debian/Ubuntu: apt install build-essential)."

missing=()
for dep in "${DEPS[@]}"; do
    pkg-config --exists "$dep" || missing+=("$dep")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "${RED}Erro:${RESET} dependências ausentes: ${missing[*]}" >&2
    echo "Debian/Ubuntu: apt install libgtk-3-dev libxfce4panel-2.0-dev libwnck-3-dev libxfce4util-dev" >&2
    exit 1
fi

for f in "$NAME.c" "$NAME.desktop"; do
    [ -f "$DIR/$f" ] || die "arquivo ausente: $DIR/$f"
done

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

step "Compilando..."
# shellcheck disable=SC2046
gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
    -shared -fPIC "$DIR/$NAME.c" -o "$BUILD/lib$NAME.so" \
    $(pkg-config --cflags --libs "${DEPS[@]}") -lm

step "Instalando..."
was_installed=0
[ -e "$LIB_FILE" ] && was_installed=1

# install troca o arquivo
install -D -m 644 "$BUILD/lib$NAME.so" "$LIB_FILE"
install -D -m 644 "$DIR/$NAME.desktop" "$DESKTOP_FILE"

echo "${GREEN}Concluído.${RESET}"
if [ "$was_installed" -eq 1 ]; then
    notify_restart
else
    echo
    echo "Adicione o plugin em Painel > Itens > Adicionar > NaviSpace."
    echo "Se ele não aparecer na lista, reinicie o painel: xfce4-panel -r"
fi