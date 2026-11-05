# OpenTune Control Protocol (OTCP), version 1

OTCP is the local control interface built into OpenTune Standalone: JSON-RPC 2.0, one JSON message per line, over TCP on `127.0.0.1`. A client on the same computer, such as an MCP server for AI agents, uses it to perform the operations a person can perform in the OpenTune window and to receive clear, structured results.

This directory is the complete specification of protocol major 1. It is licensed under the MIT License ([LICENSE](LICENSE)) so that it can be copied unchanged into any repository that implements it. Copies MUST be byte-identical; all files use UTF-8 without a byte order mark and LF line endings.

Current version: `1.0.0` (see [CHANGELOG.md](CHANGELOG.md)).

## Files

| Path                                   | Content                                                                                                                                                                                                         | Normative      |
| -------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------- |
| [PROTOCOL.md](PROTOCOL.md)             | The protocol: transport, session, discovery, enabling, clocks and units, handles, tokens, jobs, concurrency, result conventions, undo, errors, warnings, export contract, paths, editing schemes, method index. | yes            |
| [SECURITY.md](SECURITY.md)             | Threat model, mitigations and residual risks.                                                                                                                                                                   | yes            |
| [CHANGELOG.md](CHANGELOG.md)           | Changes per specification version.                                                                                                                                                                              | –              |
| `registry/error-kinds.json`            | Error kinds.                                                                                                                                                                                                    | yes            |
| `registry/reasons.json`                | Reason codes for errors, undo descriptors and unavailable values.                                                                                                                                               | yes            |
| `registry/warnings.json`               | Warning codes.                                                                                                                                                                                                  | yes            |
| `registry/capabilities.json`           | Capability strings.                                                                                                                                                                                             | yes            |
| `schemas/common.schema.json`           | Shared types: handles, tokens, units, keys, note parameters, transport, undo history, write envelope, undo descriptor, errors, jobs, limits.                                                                    | yes            |
| `schemas/discovery-file.schema.json`   | Discovery file and ready file: closed forms that servers write, open forms (`knownMembers`) that clients validate against.                                                                                      | yes            |
| `schemas/registry.schema.json`         | Shape of the registry files.                                                                                                                                                                                    | yes            |
| `schemas/method-file.schema.json`      | Shape of the method schema files.                                                                                                                                                                               | yes            |
| `schemas/vector.schema.json`           | Shape of the test vectors.                                                                                                                                                                                      | yes            |
| [methods/README.md](methods/README.md) | Format of the method schema files.                                                                                                                                                                              | yes            |
| `methods/index.json`                   | Every method of major 1 with layer, capability, write classification, `since` and schema status.                                                                                                                | yes            |
| `methods/<method>.schema.json`         | Params, result and metadata of one method.                                                                                                                                                                      | yes            |
| [vectors/README.md](vectors/README.md) | Format of the test vectors, matchers and reproducibility rules.                                                                                                                                                 | yes            |
| `vectors/<area>/<slug>.json`           | Test vectors.                                                                                                                                                                                                   | yes (examples) |

All schemas use JSON Schema 2020-12. Their `$id` values start with `https://otcp.invalid/v1/`, which mirrors this directory: `https://otcp.invalid/v1/schemas/common.schema.json` is `schemas/common.schema.json`. The `.invalid` top-level domain never resolves; the ids are identifiers, and tools load the files from this directory.

## Reading order

1. PROTOCOL.md sections 0 to 4: conventions, versioning, transport and session.
2. PROTOCOL.md sections 5 and 6 and SECURITY.md: discovery, enabling and the threat model.
3. PROTOCOL.md sections 8 to 16: clocks, handles, tokens, readiness, jobs, concurrency, write results, undo and reads.
4. PROTOCOL.md sections 17 to 21 and the registries: errors, warnings, the export contract, paths and editing schemes.
5. PROTOCOL.md section 22, `methods/README.md` and the method files; then `vectors/README.md` and the vectors.

Implementers of a server read all of it. Implementers of a client can skip section 6. Authors of mocks and contract tests need sections 2, 3, 4, 12, 14 and 17 and the vector format.

## Versions

1. The specification is released as tags `spec-vMAJOR.MINOR.PATCH`; release candidates carry `-rc.N`, for example `spec-v1.0.0-rc.1`.
2. On the wire only `MAJOR.MINOR` is exchanged, in `session.hello`.
3. A minor version only adds (methods, members, codes, capabilities); removing, renaming or changing the meaning or unit of anything needs a new major version, which lives in a new directory (`v2/`).
4. Registries are append-only within a major version, and clients accept codes they do not know.
5. Every method and every vector states the capability it needs and the version that introduced it (`since`).

## How to cite

Refer to a released version by its tag and this directory, for example: "OpenTune Control Protocol (OTCP) 1.0.0, `spec/otcp/v1/` at tag `spec-v1.0.0`". To pin an exact copy, record the tag and the SHA-256 of each file. Sections of PROTOCOL.md are cited by number, for example "OTCP 1.0, section 14.4".
