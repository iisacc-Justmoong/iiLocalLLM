# Repeated parameter validation

Imported catalogs build private field-name and required-field indexes once.
Public field order and exported JSON remain unchanged. Nested object validation
does not linearly search the whole group for each supplied property, and direct
field lookup uses the same index. Required/default/read-only checks retain their
existing behavior.

Schema regular expressions are validated and compiled at catalog construction,
then reused by const validation calls. The immutable catalog owns all indexes
and expressions; there is no global unbounded cache or mutation during reads.
An `anyOf` stops evaluating alternatives after a match, while all constraints
outside the union continue to be checked. Secret redaction still visits all
branches, since it has different semantics from acceptance.

Parameter tests cover nested references, independent imported catalogs with the
same field names, required/default/read-only fields, regex failures, concurrent
const validation, and union constraints. Native inference settings and native
binding rejection remain part of the full module suite.
