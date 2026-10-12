# Test certificate

The sender and receiver expect `cert.pem` and `key.pem` in this directory.
Do not commit a private key from an experimental machine.

Generate a local self-signed certificate from the repository root:

```bash
./scripts/generate_test_cert.sh
```
