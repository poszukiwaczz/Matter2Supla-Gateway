"""Allocate the HTTP transport before TLS in the pinned ESP-IDF server."""
from pathlib import Path
import hashlib
import sys

source, output = map(Path, sys.argv[1:])
expected = '39975d1e6ef42136e6f3cc97f48eb48d8d053676ed4a4167f852fe8b49233a9d'
if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
    raise SystemExit('Review the local HTTPS session fix for this ESP-IDF revision')
text = source.read_text(encoding='utf-8')
start = text.index('    // Pass a new structure containing')
end = text.index('    transport_ctx->tls = tls;', start)
transport = text[start:end]
text = text[:start] + text[end:]
marker = '    esp_tls_t *tls = esp_tls_init();'
assert text.count(marker) == 1
text = text.replace(marker, transport + marker)
text = text.replace('    if (!tls) {\n        esp_https_server_last_error_t',
                    '    if (!tls) {\n        free(transport_ctx);\n        esp_https_server_last_error_t', 1)
text = text.replace('fail:\n    {\n        esp_tls_error_handle_t',
                    'fail:\n    {\n        free(transport_ctx);\n        esp_tls_error_handle_t', 1)
output.parent.mkdir(parents=True, exist_ok=True)
if not output.is_file() or output.read_text(encoding='utf-8') != text:
    output.write_text(text, encoding='utf-8')
