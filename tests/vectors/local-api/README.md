# Local API regression examples

These are synthetic, non-secret examples derived from the current response
shapes in `src/WebStatus.cpp` and `src/WebApplication.cpp`. They are **not
hardware captures**. They cover Bluetooth-only management replies (HTTP is off
in that mode), null mutation IDs, and OTA reservation (disabled in the preview).

`tests/integration/schema_contract_test.py` validates them and checks rejected
variants. The preview integration tests additionally validate their actual
HTTP responses, SSE status frames and accepted JSON requests. Hardware captures
can be checked without contacting a timer:

```sh
python tests/integration/api_contract.py wifi-status /path/to/captured-reply.json
```

Use an environment with the root `requirements-dev.txt` installed. Capture
provenance/build IDs and hardware qualification remain separate release evidence.
