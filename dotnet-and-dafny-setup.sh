#!/bin/bash

set -e

if [ ! -f /usr/include/numa.h ]; then
  echo "Missing libnuma headers; installing"
  sudo apt-get update
  sudo apt-get -y install libnuma-dev
fi

if ! command -v make &> /dev/null
then
    echo "make could not be found in PATH."
    sudo apt-get -y install make
fi

if ! command -v dotnet &> /dev/null
then
    echo "dotnet could not be found in PATH."
    if [ -d "$HOME/.dotnet" ]
    then
        echo "Adding $HOME/.dotnet to PATH."
        export PATH="$PATH:$HOME/.dotnet"
    else
        echo "$HOME/.dotnet directory not found. Running install-dotnet script."
        wget https://dot.net/v1/dotnet-install.sh -O dotnet-install.sh
        chmod +x dotnet-install.sh
        ./dotnet-install.sh --channel 5.0
        export PATH="$PATH:$HOME/.dotnet"
    fi
else
    echo "dotnet is in PATH."
fi

if command -v dotnet -h &> /dev/null; then
    echo "dotnet seems to be working"
else
    echo "dotnet install didn't seem to work"
    exit -1
fi

source "$HOME/.cargo/env" || true
if cargo version &> /dev/null; then
    echo "cargo+rustc seems to be working; skipping install"
else
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    source "$HOME/.cargo/env"
    if cargo version &> /dev/null; then
        echo "cargo+rustc seems to be working"
    else
        echo "cargo+rustc install failed"
        exit -1
    fi
fi

if ! tools/local-dafny.sh /version &> /dev/null; then
    rm -rf .dafny
    ./tools/artifact-setup-dafny.sh
fi

pip3 install toposort
