# MyShell

A simple Unix-like shell written in C.

## Features
- Run external commands via `PATH`
- Built-ins: `cd`, `jobs`, `fg`, `bg`, `history`, `exit`
- Pipelines with `|`
- Redirection: `<`, `>`, `>>`, `2>`, `2>>`
- Background jobs with `&`
- Basic job control (foreground/background switching)
- Shell variables and `$VAR` expansion
- Custom prompt with `PS1`
- Command history persisted in `myshell_hist.txt`

## Build & Run
```bash
gcc shell.c -lreadline -o myshell
./myshell
```
