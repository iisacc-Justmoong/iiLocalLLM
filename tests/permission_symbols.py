"""Private parser symbols must not interpose on a host application's parser."""
import re
import subprocess
import sys

symbols = subprocess.check_output([sys.argv[1], "-gU", sys.argv[2]], text=True)
exported = re.findall(r"\b_(?:ts_|tree_sitter_)\w+", symbols)
assert not exported, exported
print("No private tree-sitter runtime or Bash grammar symbols are exported")
