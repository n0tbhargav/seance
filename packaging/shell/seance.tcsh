# Séance shell integration for tcsh.
# (Ghostty's built-in integration covers bash, zsh, fish, elvish and nushell — not tcsh.)
#
# Gives tcsh: prompt marks (OSC 133), current-directory tracking (OSC 7: new tabs/splits open in the same directory),
# window/tab titles (OSC 2), and command-finished events with exit code + duration (`seancectl events`, notifications).
#
# Install (works for any install path):   seance --shell-hook tcsh >> ~/.tcshrc
# or by hand in ~/.tcshrc, AFTER your own `set prompt`:
#   if ($?SEANCE_PANE && -f /path/to/seance.tcsh) source /path/to/seance.tcsh
#
# It wraps your existing prompt and keeps any existing precmd/postcmd alias (including the one SUSE's /etc/csh.cshrc sets):
# the old body is saved to a file and sourced after ours, so it is never re-parsed or altered.
if ($?prompt && $?SEANCE_PANE) then
  if (! $?_seance_hooked) then
    set _seance_hooked = 1
    set _seance_esc = "`printf '\033'`"
    set _seance_bel = "`printf '\007'`"
    # A = prompt start, B = prompt end (input start); %{ %} marks them zero-width for tcsh's line editor.
    set prompt = "%{${_seance_esc}]133;A${_seance_bel}%}${prompt}%{${_seance_esc}]133;B${_seance_bel}%}"

    # Before each prompt: last command's exit status (D), current directory (OSC 7) and title (OSC 2).
    alias _seance_pre 'set _seance_st = $status; printf "\033]133;D;%d\007\033]7;file://%s%s\007\033]2;%s\007" $_seance_st "${HOST}" "`echo ${cwd} | sed s/\ /%20/g`" "${USER}@${HOST}:${cwd}"'
    # Before each command runs: mark the start (C) so Séance can time it.
    alias _seance_post 'printf "\033]133;C\007"'

    set _seance_dir = "${HOME}/.cache/seance"
    mkdir -p "$_seance_dir"
    set _seance_old_pre = "$_seance_dir/precmd.$$"
    set _seance_old_post = "$_seance_dir/postcmd.$$"
    alias precmd  >! "$_seance_old_pre"
    alias postcmd >! "$_seance_old_post"
    if (-s "$_seance_old_pre") then
      alias precmd '_seance_pre; source '"$_seance_old_pre"
    else
      alias precmd '_seance_pre'
    endif
    if (-s "$_seance_old_post") then
      alias postcmd '_seance_post; source '"$_seance_old_post"
    else
      alias postcmd '_seance_post'
    endif
    unset _seance_old_pre _seance_old_post _seance_dir
  endif
endif
