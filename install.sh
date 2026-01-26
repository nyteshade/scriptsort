#!/usr/bin/env sh

printf "\x1b[1mBuilding scriptsort and ms\x1b[22m..."
./build.sh

[[ ${RC} -eq 0 ]] && echo "done" || echo "\x1b[31mfailed\x1b[39m"


printf "\x1b[1mInstalling files\x1b[22m...\n\n"

if [[ ! -d ${HOME}/.local ]]; then
  mkdir -p ${HOME}/.local
fi

cp -rpv .local/* ${HOME}/.local

if [[ ${SHELL} =~ "zsh" ]]; then
  printf "\x1b[1mInstalling ZSH support...\x1b[22m"
  ./addtozsh
  printf "done\n"
fi

if [[ ${SHELL} =~ "bash" ]]; then
  printf "\x1b[1mInstalling BASH support...\x1b[22m"
  ./addtobash
  printf "done\n"
fi
