# Schemas and data description API
The content of Hound records (the data field) is different for each Hound data
type. However, the contents of the data type can be queried programatically via
`hound_get_fmt` and `hound_get_fmt_by_name` calls. These calls will return a
`struct hound_data_fmt` describing the given data ID or friendly name. Having
this API is important because it allows library users to programatically
translate Hound records into JSON or something other hierarchical format without
having to hardcode knowledge of each data type. In fact, picking up new data
types will "just work" without any code change on the part of the users, as the
API is guaranteed.

That said, we would like a stronger guarantee that data formats don't
accidentally change because of code changes. So, we use a YAML schema to
describe the format for each Hound data type. These schemas are checked into the
`schema` top-level directory of the Hound repository and automatically
translated into `struct hound_data_fmt`. This logic is handled by the driver
core rather than the drivers themselves in order to enforce that drivers stick
to the schemas.

In addition, the schemas have a meta-schema to describe the format of the schema
itself. The meta-schema allows us to check the schemas at compile-time so that
the runtime parser can be assured of schema correctness.

By using this schema mechanism, we achieve a few goals:
- Data formats are explicitly declared in a data-driven manner rather than
  implicitly through code. This makes it clear that the data format is a
  contract.
- Data schema can be version-controlled in exactly one location, so there is one
  canonical source of truth.
- Programs parsing Hound data can implement their own parsing routines using the
  schema, if they so wish. This can be convenient for people writing quick
  scripts, or those using languages that do not provide Hound bindings.
- We can check schema correctness at compile-time, and include them in unit
  tests to avoid accidental data format changes.
- We avoid code generation (an alternative to writing a runtime parser).
  Although this would slightly improve performance by eliminating a runtime
  parser, it would create maintenance issues, as generated code is hard to read
  and maintain. If the runtime parser's performance becomes an issue (unlikely,
  since it runs once per driver and parses a very small document), we could
  consider code generation as an optimization, but at this point, that seems
  premature.
