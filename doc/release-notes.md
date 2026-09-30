# sv2-tp v1.1.2

## Bitcoin Core compatibility

This release supports Bitcoin Core v31 and v32. When upgrading to Bitcoin
Core v32, upgrade sv2-tp to v1.1.2 first. Earlier sv2-tp releases do not
support v32's changed IPC block-submission interface.

sv2-tp automatically selects the appropriate mining interface for the
connected node. Bitcoin Core v31 users can also upgrade to this release.
sv2-tp v1.0.6 remains the last release compatible with Bitcoin Core v30.2.

## Notable changes

- Support Bitcoin Core v32's block-submission interface and log the
  rejection reason and debug details returned by the node.
- Exit safely when the Bitcoin Core IPC connection is lost, including
  when no SV2 clients are connected. Backend loss removes the PID file
  and returns a failure exit status so a supervisor can restart a fresh
  process. Avoid an IPC cleanup race that could crash sv2-tp while clients
  were connected. Requested shutdown through SIGINT or SIGTERM continues
  to return success. The bundled systemd service retries after five seconds.
- Forward `SubmitSolution` even before a client completes
  `SetupConnection` and `CoinbaseOutputConstraints`. A reconnecting
  client can submit a solution for a previously cached template.
- Reply to premature `RequestTransactionData` messages with a
  `setup-incomplete` error instead of disconnecting the client, allowing
  a following solution to be submitted.
- Preserve each client's template ID when sending concurrent updates,
  keeping it paired with the correct cached template.
- Fix locking around client disconnect flags and template identifiers.
- Improve Noise handshake diagnostics and document authority keys,
  static keys, and certificates.
- Update the bundled libmultiprocess library and Guix release tooling.

## Contributors

- Enoch Azariah
- Sjors Provoost
