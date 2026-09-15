"""All public entrypoints must report the version selected by their CMake project."""
import subprocess
import sys

expected, *executables = sys.argv[1:]
assert executables, "At least one executable is required"
failures = []
for executable in executables:
    result = subprocess.run([executable, "--version"], capture_output=True, text=True,
                            timeout=15)
    output = result.stdout.strip()
    if result.returncode != 0 or output.rsplit(" ", 1)[-1] != expected:
        failures.append((executable, result.returncode, output, result.stderr))
assert not failures, failures
print(f"{len(executables)} entrypoint versions match {expected}")
