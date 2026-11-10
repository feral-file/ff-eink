# Keys

No private key is ever committed here (`*.pem` is gitignored).

- `release-key.sha256`: SHA-256 digest of the **release** public key, as
  printed by `espsecure.py signature_info_v2` ("Public key digest for
  block 0"). `release.yml` refuses to publish an image signed with any other
  key. Added when the release key is created
  ([docs/ota.md](../docs/ota.md#signing-and-keys)).
