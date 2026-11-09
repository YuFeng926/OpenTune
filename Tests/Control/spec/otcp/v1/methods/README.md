# Method schema files

Every OTCP method has one file in this directory, `<method>.schema.json` (for example `job.get.schema.json`). `index.json` lists every method of protocol major 1, including those whose file is not written yet. The shape of a method file is defined by `../schemas/method-file.schema.json`.

## Format

A method file is a JSON Schema 2020-12 document:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://otcp.invalid/v1/methods/job.get.schema.json",
  "title": "job.get",
  "description": "What the method does, in one paragraph.",
  "x-otcp": {
    "method": "job.get",
    "layer": "L3",
    "capability": "jobs.v1",
    "since": "1.0",
    "write": "no",
    "resultKind": "plain",
    "startsJob": null,
    "requiresSession": true,
    "mainThread": false,
    "errors": [
      "INVALID_ARGUMENT",
      "NOT_FOUND",
      "UNSUPPORTED",
      "BUSY",
      "SHUTTING_DOWN",
      "INTERNAL"
    ]
  },
  "$defs": {
    "params": { "type": "object" },
    "result": { "type": "object" }
  }
}
```

| Member                     | Rule                                                                                                                                                                                                                                                                                                                                                                                                             |
| -------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `$id`                      | `https://otcp.invalid/v1/methods/<method>.schema.json`. The `otcp.invalid` host is an identifier, not a download location (the `.invalid` top-level domain never resolves); tools map it to this directory.                                                                                                                                                                                                      |
| `title`                    | The method name. Equal to `x-otcp.method` and to the file name without `.schema.json`.                                                                                                                                                                                                                                                                                                                           |
| `description`              | What the method does, with references to the PROTOCOL.md sections that govern it.                                                                                                                                                                                                                                                                                                                                |
| `x-otcp.method`            | Method name, `<namespace>.<name>`, matching `../schemas/common.schema.json#/$defs/methodName`. This form binds the names of defined methods only; in a request any string is a valid `method`, and one that names no defined method gets `UNSUPPORTED(unknown_method)` (PROTOCOL.md section 3.3).                                                                                                                |
| `x-otcp.layer`             | `L2` to `L8` (PROTOCOL.md section 22).                                                                                                                                                                                                                                                                                                                                                                           |
| `x-otcp.capability`        | Capability from `../registry/capabilities.json` that the method needs.                                                                                                                                                                                                                                                                                                                                           |
| `x-otcp.since`             | Protocol version (`MAJOR.MINOR`) that introduced the method.                                                                                                                                                                                                                                                                                                                                                     |
| `x-otcp.write`             | `yes`, `no` or `conditional` (PROTOCOL.md section 22).                                                                                                                                                                                                                                                                                                                                                           |
| `x-otcp.resultKind`        | `write` (the result extends the write envelope, PROTOCOL.md section 14), `read` (the result carries `asOf` and `derived`, section 16) or `plain`. A method with `write` `yes` or `conditional` has `resultKind` `write`, except `app.quit` (`plain`, PROTOCOL.md section 14.1).                                                                                                                                                                                                                                                              |
| `x-otcp.startsJob`         | Job kind the method may start, or null. A job's result is defined by the `$defs.jobResult` of the method that started it, not by its kind (PROTOCOL.md section 12.1).                                                                                                                                                                                                                                            |
| `x-otcp.requiresSession`   | false only for `session.hello`.                                                                                                                                                                                                                                                                                                                                                                                  |
| `x-otcp.mainThread`        | true when the method runs on OpenTune's main thread and may therefore return `TIMEOUT(dispatch_timeout)` (PROTOCOL.md section 13.3).                                                                                                                                                                                                                                                                             |
| `x-otcp.errors`            | Every error kind the method may return, generic ones included, with the rules below. Kinds come from `../registry/error-kinds.json`.                                                                                                                                                                                                                                                                             |
| `x-otcp.jobErrors`         | Present exactly when `startsJob` is not null: every error kind a job that the method starts may end with (the job's `error`, PROTOCOL.md section 12.1). `INTERNAL` always, `CANCELLED` when the job kind can be cancelled (section 12.3), and the kinds the job itself can produce.                                                                                                                              |
| `x-otcp.valueCapabilities` | Optional. Values of top-level params members that need a capability besides `x-otcp.capability`, as `{"<member>": {"<value>": "<capability>"}}`; no method of this version uses it. A request with such a value fails with `UNSUPPORTED(capability_missing)` on an instance that does not report the capability (PROTOCOL.md section 3.3 rule 5 case 10). Clients use it to check capabilities before they call. |
| `$defs.params`             | Schema of `params`. Closed: unknown members are invalid (PROTOCOL.md section 2.3). A method without params has an empty, closed object.                                                                                                                                                                                                                                                                          |
| `$defs.result`             | Schema of `result`. A `write` result composes `../schemas/common.schema.json#/$defs/writeEnvelope` with `allOf` and closes itself with `unevaluatedProperties: false`; a `read` result does the same with `#/$defs/readMeta`.                                                                                                                                                                                    |
| `$defs.jobResult`          | Present exactly when `startsJob` is not null: schema of the `result` of a job the method starts. For a job that changes project state it extends the write envelope like `$defs.result`.                                                                                                                                                                                                                         |

Shared types are referenced as `../schemas/common.schema.json#/$defs/<name>`. Other documents refer to a method's schemas as `<method>.schema.json#/$defs/params`, `#/$defs/result` and `#/$defs/jobResult`. A method file may define further `$defs` for its own use.

`x-otcp` is an annotation keyword. Validators that reject unknown keywords (for example Ajv in strict mode) need it registered, for instance with `ajv.addKeyword({ keyword: "x-otcp" })`; its content is validated against `../schemas/method-file.schema.json`.

`x-otcp.errors` lists every kind explicitly, so that mocks and coverage tests can read it without knowing the generic rules:

| Kind               | Listed when                                                                                                |
| ------------------ | ---------------------------------------------------------------------------------------------------------- |
| `INVALID_ARGUMENT` | always (malformed requests and params, PROTOCOL.md section 3.3)                                            |
| `BUSY`             | always (`request_limit`, PROTOCOL.md section 3.3)                                                          |
| `SHUTTING_DOWN`    | always (PROTOCOL.md section 3.6)                                                                           |
| `INTERNAL`         | always                                                                                                     |
| `UNSUPPORTED`      | always when the capability is not `session.v1` (`capability_missing`); otherwise if the method produces it |
| `TIMEOUT`          | exactly when `mainThread` is true                                                                          |
| other kinds        | the method itself can produce them                                                                         |

`UNSUPPORTED(unknown_method)` concerns methods that are not defined and therefore belongs to no method file.

## Rules

1. Adding a method file, or a member to one, follows the versioning rules of PROTOCOL.md section 2.2. A method file is never deleted or renamed within a major version.
2. `index.json` and the table in PROTOCOL.md section 22 list the same methods with the same layer, capability, write classification, `since` and schema status; a method file exists exactly for the methods marked `defined`.
3. Every method file has at least one success vector in `../vectors/`.
4. `x-otcp.errors` and `x-otcp.jobErrors` follow the rules above.
5. Codes used in descriptions (error kinds, reasons, warnings) come from `../registry/`.
6. In params, a member whose values select what the method does (`op`, `mode`, `kind`, `layout`, `action`, `target`) lists them with `enum` or `const`, and each value has a success vector (`../vectors/README.md`, "Coverage"). Results use open forms such as `camelName` for enumerated strings, because clients accept values they do not know (PROTOCOL.md section 2.3).
