#!/bin/env bash

error() {
    printf "\033[31;1m%s\033[m\n" "$@"
    exit 1
}

default_prefix='/usr/local'
if echo "$PATH" | tr ':' '\n' | grep -qx "$HOME/.local/bin"; then
    default_prefix="~/.local"
fi

printf '\033[1mChoose where to install bp (default: %s):\033[m ' "$default_prefix"
read PREFIX
if [ -z "$PREFIX" ]; then PREFIX="$default_prefix"; fi
PREFIX="${PREFIX/#\~/$HOME}"

if ! echo "$PATH" | tr ':' '\n' | grep -qx "$PREFIX/bin"; then
    error "Your \$PATH does not include this prefix, so you won't be able to run tomo!" \
        "Please put this in your .profile or .bashrc: export PATH=\"$PREFIX/bin:\$PATH\""
fi

if command -v doas >/dev/null; then
    SUDO=doas
else
    SUDO=sudo
fi

cat <<END >config.mk
PREFIX=$PREFIX
SUDO=$SUDO
END
