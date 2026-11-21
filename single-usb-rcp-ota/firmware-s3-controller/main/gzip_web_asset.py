"""Generate a deterministic gzip representation for an embedded web asset."""
import gzip
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
destination.write_bytes(gzip.compress(source.read_bytes(), compresslevel=9, mtime=0))
