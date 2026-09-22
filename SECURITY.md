# Security Policy

## Reporting a vulnerability

Report vulnerabilities in OSHI Mesh privately to **contact@oshi-messenger.com**, or through GitHub's private vulnerability
reporting on this repository (Security tab). Please include the firmware version or commit, the board, and steps or a script
to reproduce. Do not open a public issue for a vulnerability.

We aim to acknowledge a report within a few days and will agree on a disclosure date with you.

## Scope

In scope here: code that OSHI Mesh adds or changes, which is `src/oshi/`, `src/modules/OshiModule.*`, the OSHI hooks in
`src/mesh/MeshService.cpp`, `src/mesh/Router.cpp` and `src/modules/Modules.cpp`, and the OMP specification in `docs/omp/`.

Vulnerabilities in the Meshtastic firmware itself (code that is the same as upstream) should be reported to the Meshtastic
project, following the [upstream security policy](https://github.com/meshtastic/firmware/security/policy). If you are not
sure which side a problem is on, write to us and we will help route it.

## Supported versions

Only the latest commit of the `oshi` branch and the latest release are supported.

## Security model

OSHI Mesh inherits the Meshtastic security model: channel PSK encryption (not authenticated), PKI direct messages, and full
trust in the local API client (BLE, serial, TCP). See the upstream documentation:
<https://meshtastic.org/docs/overview/encryption/>.

What OMP adds, and what it does not protect, is described in
[docs/omp/OMP-v1.md, section 11](docs/omp/OMP-v1.md#11-security-considerations). In short:

- The `OSHI` channel key is public by design. It separates OMP from the default channel; it does not keep anything secret.
  Message confidentiality comes from end-to-end encryption in the apps, and from Meshtastic PKI when the recipient's key is
  known.
- Delivery reports (SACK, CUSTODY, RECEIPT) from a node whose key is known are only accepted over PKI. From other nodes they
  are unauthenticated.
- Custodians and gateways are trusted for availability only: they can delay or drop messages, not read or forge them.

Reports that only restate these documented properties are not vulnerabilities, but reports that show them to be weaker than
described are.
