export PS1="\[\e[31m\][\[\e[m\]\[\e[32m\]\u\[\e[m\]\[\e[33m\]@\[\e[m\]\[\e[35m\]\h\[\e[m\]:\[\e[36m\]\w\[\e[m\]\[\e[31m\]]\[\e[m\]\[\e[1;32m\]\\$\[\e[m\] "

export PATH="$PATH:/apollo/scripts"

if [[ -e "/apollo/scripts/apollo_base.sh" ]]; then
    . /apollo/scripts/apollo_base.sh
fi

alias bb="bazel build --distdir=/apollo/.cache/distdir"
alias bt="bazel test  --distdir=/apollo/.cache/distdir"

function inc() {
    local _path="$1"
    if [[ -d "${_path}" ]]; then
        /bin/grep "#include" -r "${_path}" | awk -F':' '{print $2}' | sort -u
    elif [[ -f "${_path}" ]]; then
        /bin/grep "#include" -r "${_path}" | sort -u
    fi
}

ulimit -c unlimited