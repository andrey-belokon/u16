# BScript Examples

These scripts and programs demonstrate the Phase 1 language and library (BScript version 1.1). Every example is tested automatically. Running `make examples` (or `python3 tools/run_tests.py --examples`) copies the example directory to the FS16 volume as `/ex`, runs the command specified by the first-line `// run: ...` directive, and compares the output with `expected/NAME.out`.

| Example | Demonstrates |
| --- | --- |
| `hello.bs` | Minimal script: `args` and string interpolation |
| `logscan.bs` | Specification example B.1: `fs.list`, `path.match`, reading file lines, and an error captured in an additional return variable |
| `wc.bs` | Program with `main` and `include`, multiple function results, utility-style diagnostics, and exit status |
| `grep.bs` | Command-line switches, `switch`, string membership with `in`, `lower()`, and exit statuses 0/1/2 |
| `tree.bs` | Recursion, `fs.list`, directory-entry flags, and sorting |
| `backup.bs` | Recursive directory copying, handling `errors.EXIST`, a custom error code, and `fail` |
| `wordfreq.bs` | Maps, sort keys, number parsing with `to_int`, and error handling |
| `calc.bs` | Recursive-descent calculator, variables, parse errors returned as `error` values, and input from stream 0 |
| `todo.bs` | File-backed task list: `fs.open`, reading and writing, and treating `errors.NOENT` as “file not created yet” |
| `checkutils.bs` | Shell-utility tests: `proc.run`, `proc.capture`, redirection, and assertion helpers |
| `lib/text.bs` | Shared functions imported with `include` |

Example input files are stored in `data/`. To run an example on the CPU16 machine, copy the directory to a volume and execute `bs file.bs ...`. If the script is in the current directory or in `A:/bin`, you can invoke it by name alone, for example `wc poem.txt`.
