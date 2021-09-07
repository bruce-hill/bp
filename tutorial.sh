#!/bin/sh
# Run a small tutorial on basic bp functionality

tmpfile="$(mktemp /tmp/bp-tutorial.XXXXXX)"
trap 'rm "$tmpfile"' EXIT

for t in $([ $# -gt 0 ] && echo "$@" || ls -v tests/*.sh); do
    echo
    printf "\033[1m"
    sed -n 's/^# //p' "$t"
    printf "\033[m"
    printf "\033[33;1mGiven these lines:              Give this output:\033[m\n"
    diff -y -W60 --color=always "${t/.sh/.in}" "${t/.sh/.out}"
    while true; do
        printf "\n\033[1mbp pattern: \033[m"
        read -r pat
        printf "\033[0;2mRunning: \033[32m%s\033[m\n\n" "bp -p '$pat'"
        printf "\033[33;1mExpected output:                Your pattern's output:\033[m\n"
        bp -p "$pat" < "${t/.sh/.in}" 2>"$tmpfile" | diff -y -W60 --color=always "${t/.sh/.out}" - && break
        cat "$tmpfile"
        printf "\n\033[0;1;31mSorry, try again!\033[m\n"
    done
    printf "\n\033[0;1;32mCorrect!\033[m\n"
done
